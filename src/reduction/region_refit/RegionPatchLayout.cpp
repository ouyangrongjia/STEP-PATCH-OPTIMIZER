#include "reduction/region_refit/RegionPatchLayout.h"

#include <Eigen/Eigenvalues>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCore>

#include <gp_Vec.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace spo {

namespace {

using MeshEdge = std::array<std::size_t, 2>;

MeshEdge orderedEdge(const std::size_t first, const std::size_t second) {
    return {std::min(first, second), std::max(first, second)};
}

struct TriangleFeature {
    gp_Pnt centroid;
    gp_Dir normal {0.0, 0.0, 1.0};
    double curvature = 0.0;
    double area = 0.0;
};

struct MeshGraph {
    std::vector<TriangleFeature> features;
    std::vector<std::vector<std::size_t>> adjacency;
    std::map<MeshEdge, std::vector<std::size_t>> edgeTriangles;
    double diagonal = 1.0;
    double curvatureScale = 1.0;
};

double curvatureFor(const RegionReferenceVertex& vertex) {
    double value = 0.0;
    for (const auto& source : vertex.sources) {
        if (source.curvatureDefined) {
            value = std::max(value, std::max(
                std::abs(source.minimumCurvature),
                std::abs(source.maximumCurvature)));
        }
    }
    return value;
}

bool buildGraph(const RegionReferenceMesh& mesh, MeshGraph& graph, std::string& message) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) {
        message = "Reference mesh is empty.";
        return false;
    }
    graph.features.resize(mesh.triangles.size());
    graph.adjacency.resize(mesh.triangles.size());
    gp_Pnt minimum = mesh.vertices.front().point;
    gp_Pnt maximum = minimum;
    for (const auto& vertex : mesh.vertices) {
        minimum.SetX(std::min(minimum.X(), vertex.point.X()));
        minimum.SetY(std::min(minimum.Y(), vertex.point.Y()));
        minimum.SetZ(std::min(minimum.Z(), vertex.point.Z()));
        maximum.SetX(std::max(maximum.X(), vertex.point.X()));
        maximum.SetY(std::max(maximum.Y(), vertex.point.Y()));
        maximum.SetZ(std::max(maximum.Z(), vertex.point.Z()));
    }
    graph.diagonal = std::max(minimum.Distance(maximum), 1.0e-12);

    for (std::size_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
        const auto& triangle = mesh.triangles[triangleIndex];
        if (std::any_of(triangle.vertices.begin(), triangle.vertices.end(), [&](const auto vertex) {
                return vertex >= mesh.vertices.size();
            })) {
            message = "Reference mesh contains an invalid triangle index.";
            return false;
        }
        const auto& first = mesh.vertices[triangle.vertices[0]];
        const auto& second = mesh.vertices[triangle.vertices[1]];
        const auto& third = mesh.vertices[triangle.vertices[2]];
        auto& feature = graph.features[triangleIndex];
        feature.centroid = gp_Pnt(
            (first.point.X() + second.point.X() + third.point.X()) / 3.0,
            (first.point.Y() + second.point.Y() + third.point.Y()) / 3.0,
            (first.point.Z() + second.point.Z() + third.point.Z()) / 3.0);
        gp_Vec areaVector(first.point, second.point);
        areaVector.Cross(gp_Vec(first.point, third.point));
        feature.area = areaVector.Magnitude() * 0.5;
        gp_Vec normalSum(first.normal);
        normalSum += gp_Vec(second.normal);
        normalSum += gp_Vec(third.normal);
        if (normalSum.SquareMagnitude() > 1.0e-24) {
            feature.normal = gp_Dir(normalSum);
        } else if (areaVector.SquareMagnitude() > 1.0e-24) {
            feature.normal = gp_Dir(areaVector);
        }
        feature.curvature = (
            curvatureFor(first) + curvatureFor(second) + curvatureFor(third)) / 3.0;
        graph.curvatureScale = std::max(graph.curvatureScale, feature.curvature);
        for (int edge = 0; edge < 3; ++edge) {
            graph.edgeTriangles[orderedEdge(
                triangle.vertices[static_cast<std::size_t>(edge)],
                triangle.vertices[static_cast<std::size_t>((edge + 1) % 3)])].push_back(triangleIndex);
        }
    }
    for (const auto& [edge, triangles] : graph.edgeTriangles) {
        (void)edge;
        if (triangles.size() > 2) {
            message = "Reference mesh contains a non-manifold edge.";
            return false;
        }
        if (triangles.size() == 2) {
            graph.adjacency[triangles[0]].push_back(triangles[1]);
            graph.adjacency[triangles[1]].push_back(triangles[0]);
        }
    }
    for (auto& adjacent : graph.adjacency) {
        std::sort(adjacent.begin(), adjacent.end());
    }
    std::vector<bool> visited(mesh.triangles.size(), false);
    std::queue<std::size_t> queue;
    queue.push(0);
    visited[0] = true;
    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop();
        for (const auto next : graph.adjacency[current]) {
            if (!visited[next]) {
                visited[next] = true;
                queue.push(next);
            }
        }
    }
    if (std::find(visited.begin(), visited.end(), false) != visited.end()) {
        message = "Reference mesh triangle graph is disconnected.";
        return false;
    }
    return true;
}

double graphEdgeCost(const MeshGraph& graph, const std::size_t first, const std::size_t second) {
    const auto& lhs = graph.features[first];
    const auto& rhs = graph.features[second];
    const auto position = lhs.centroid.Distance(rhs.centroid) / graph.diagonal;
    const auto normal = std::max(0.0, 1.0 - lhs.normal.Dot(rhs.normal));
    const auto curvature = std::abs(lhs.curvature - rhs.curvature) / graph.curvatureScale;
    return std::max(1.0e-12, position + normal * 0.5 + curvature * 0.05);
}

std::vector<std::size_t> deterministicSeeds(const MeshGraph& graph, const int count) {
    std::vector<std::size_t> seeds;
    seeds.reserve(static_cast<std::size_t>(count));
    const auto first = static_cast<std::size_t>(std::distance(
        graph.features.begin(),
        std::max_element(graph.features.begin(), graph.features.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.area < rhs.area;
        })));
    seeds.push_back(first);
    std::vector<double> minimumDistance(graph.features.size(), std::numeric_limits<double>::infinity());

    while (static_cast<int>(seeds.size()) < count) {
        const auto source = seeds.back();
        using Item = std::pair<double, std::size_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        std::vector<double> distance(graph.features.size(), std::numeric_limits<double>::infinity());
        distance[source] = 0.0;
        queue.emplace(0.0, source);
        while (!queue.empty()) {
            const auto [cost, current] = queue.top();
            queue.pop();
            if (cost != distance[current]) {
                continue;
            }
            for (const auto next : graph.adjacency[current]) {
                const auto candidate = cost + graphEdgeCost(graph, current, next);
                if (candidate < distance[next]) {
                    distance[next] = candidate;
                    queue.emplace(candidate, next);
                }
            }
        }
        for (std::size_t index = 0; index < minimumDistance.size(); ++index) {
            minimumDistance[index] = std::min(minimumDistance[index], distance[index]);
        }
        std::size_t farthest = 0;
        for (std::size_t index = 1; index < minimumDistance.size(); ++index) {
            if (minimumDistance[index] > minimumDistance[farthest]) {
                farthest = index;
            }
        }
        if (std::find(seeds.begin(), seeds.end(), farthest) != seeds.end()) {
            break;
        }
        seeds.push_back(farthest);
    }
    return seeds;
}

std::vector<int> assignConnectedRegions(
    const MeshGraph& graph,
    const std::vector<std::size_t>& seeds,
    const int patchCount) {
    struct Item {
        double cost = 0.0;
        std::size_t triangle = 0;
        int patch = 0;
    };
    const auto greater = [](const Item& lhs, const Item& rhs) {
        return std::tie(lhs.cost, lhs.patch, lhs.triangle) >
            std::tie(rhs.cost, rhs.patch, rhs.triangle);
    };
    std::priority_queue<Item, std::vector<Item>, decltype(greater)> queue(greater);
    std::vector<double> distance(graph.features.size(), std::numeric_limits<double>::infinity());
    std::vector<int> assignments(graph.features.size(), -1);
    for (int patch = 0; patch < patchCount; ++patch) {
        const auto seed = seeds[static_cast<std::size_t>(patch)];
        distance[seed] = 0.0;
        assignments[seed] = patch;
        queue.push({0.0, seed, patch});
    }
    while (!queue.empty()) {
        const auto item = queue.top();
        queue.pop();
        if (item.cost != distance[item.triangle] || item.patch != assignments[item.triangle]) {
            continue;
        }
        for (const auto next : graph.adjacency[item.triangle]) {
            const auto candidate = item.cost + graphEdgeCost(graph, item.triangle, next);
            if (candidate < distance[next] - 1.0e-15 ||
                (std::abs(candidate - distance[next]) <= 1.0e-15 && item.patch < assignments[next])) {
                distance[next] = candidate;
                assignments[next] = item.patch;
                queue.push({candidate, next, item.patch});
            }
        }
    }
    return assignments;
}

double proxyError(
    const MeshGraph& graph,
    const std::vector<int>& assignments,
    const int patchCount) {
    struct Proxy {
        double weight = 0.0;
        gp_XYZ point;
        gp_XYZ normal;
        double curvature = 0.0;
    };
    std::vector<Proxy> proxies(static_cast<std::size_t>(patchCount));
    for (std::size_t triangle = 0; triangle < graph.features.size(); ++triangle) {
        const auto patch = assignments[triangle];
        if (patch < 0) {
            return std::numeric_limits<double>::infinity();
        }
        const auto& feature = graph.features[triangle];
        const auto weight = std::max(feature.area, 1.0e-18);
        auto& proxy = proxies[static_cast<std::size_t>(patch)];
        proxy.weight += weight;
        proxy.point += feature.centroid.XYZ() * weight;
        proxy.normal += feature.normal.XYZ() * weight;
        proxy.curvature += feature.curvature * weight;
    }
    for (auto& proxy : proxies) {
        if (proxy.weight <= 0.0) {
            return std::numeric_limits<double>::infinity();
        }
        proxy.point /= proxy.weight;
        proxy.curvature /= proxy.weight;
        if (proxy.normal.SquareModulus() > 1.0e-24) {
            proxy.normal.Normalize();
        }
    }
    double energy = 0.0;
    double totalWeight = 0.0;
    for (std::size_t triangle = 0; triangle < graph.features.size(); ++triangle) {
        const auto& feature = graph.features[triangle];
        const auto& proxy = proxies[static_cast<std::size_t>(assignments[triangle])];
        const auto weight = std::max(feature.area, 1.0e-18);
        const gp_Pnt proxyPoint(proxy.point);
        const auto position = feature.centroid.SquareDistance(proxyPoint) /
            (graph.diagonal * graph.diagonal);
        const auto normal = proxy.normal.SquareModulus() > 1.0e-24
            ? std::max(0.0, 1.0 - feature.normal.XYZ().Dot(proxy.normal))
            : 0.0;
        const auto curvature = (feature.curvature - proxy.curvature) / graph.curvatureScale;
        energy += weight * (position + normal * 0.5 + curvature * curvature * 0.05);
        totalWeight += weight;
    }
    return totalWeight > 0.0 ? energy / totalWeight : std::numeric_limits<double>::infinity();
}

struct VsaProxy {
    double weight = 0.0;
    gp_XYZ point;
    gp_XYZ normal;
    double curvature = 0.0;
};

std::vector<VsaProxy> buildVsaProxies(
    const MeshGraph& graph,
    const std::vector<int>& assignments,
    const int patchCount) {
    std::vector<VsaProxy> proxies(static_cast<std::size_t>(patchCount));
    for (std::size_t triangle = 0; triangle < graph.features.size(); ++triangle) {
        const auto patch = assignments[triangle];
        if (patch < 0 || patch >= patchCount) continue;
        const auto& feature = graph.features[triangle];
        const auto weight = std::max(feature.area, 1.0e-18);
        auto& proxy = proxies[static_cast<std::size_t>(patch)];
        proxy.weight += weight;
        proxy.point += feature.centroid.XYZ() * weight;
        proxy.normal += feature.normal.XYZ() * weight;
        proxy.curvature += feature.curvature * weight;
    }
    for (auto& proxy : proxies) {
        if (proxy.weight <= 0.0) continue;
        proxy.point /= proxy.weight;
        proxy.curvature /= proxy.weight;
        if (proxy.normal.SquareModulus() > 1.0e-24) proxy.normal.Normalize();
    }
    return proxies;
}

double triangleProxyCost(
    const MeshGraph& graph,
    const std::size_t triangle,
    const VsaProxy& proxy) {
    const auto& feature = graph.features[triangle];
    const auto position = feature.centroid.SquareDistance(gp_Pnt(proxy.point)) /
        (graph.diagonal * graph.diagonal);
    const auto normal = proxy.normal.SquareModulus() > 1.0e-24
        ? std::max(0.0, 1.0 - feature.normal.XYZ().Dot(proxy.normal))
        : 0.0;
    const auto curvature = (feature.curvature - proxy.curvature) / graph.curvatureScale;
    return position + normal + curvature * curvature * 0.05;
}

std::vector<int> refineConnectedRegions(
    const MeshGraph& graph,
    std::vector<int> assignments,
    const int patchCount) {
    struct Item {
        double cost = 0.0;
        std::size_t triangle = 0;
        int patch = 0;
    };
    const auto greater = [](const Item& lhs, const Item& rhs) {
        return std::tie(lhs.cost, lhs.patch, lhs.triangle) >
            std::tie(rhs.cost, rhs.patch, rhs.triangle);
    };

    constexpr int maximumIterations = 8;
    for (int iteration = 0; iteration < maximumIterations; ++iteration) {
        const auto proxies = buildVsaProxies(graph, assignments, patchCount);
        std::vector<std::size_t> seeds(static_cast<std::size_t>(patchCount), 0);
        for (int patch = 0; patch < patchCount; ++patch) {
            double bestCost = std::numeric_limits<double>::infinity();
            bool found = false;
            for (std::size_t triangle = 0; triangle < assignments.size(); ++triangle) {
                if (assignments[triangle] != patch) continue;
                const auto cost = triangleProxyCost(graph, triangle, proxies[static_cast<std::size_t>(patch)]);
                if (!found || cost < bestCost - 1.0e-15 ||
                    (std::abs(cost - bestCost) <= 1.0e-15 && triangle < seeds[static_cast<std::size_t>(patch)])) {
                    found = true;
                    bestCost = cost;
                    seeds[static_cast<std::size_t>(patch)] = triangle;
                }
            }
            if (!found) return assignments;
        }

        std::vector<int> refined(assignments.size(), -1);
        std::priority_queue<Item, std::vector<Item>, decltype(greater)> queue(greater);
        for (int patch = 0; patch < patchCount; ++patch) {
            const auto seed = seeds[static_cast<std::size_t>(patch)];
            refined[seed] = patch;
        }
        for (int patch = 0; patch < patchCount; ++patch) {
            const auto seed = seeds[static_cast<std::size_t>(patch)];
            for (const auto adjacent : graph.adjacency[seed]) {
                if (refined[adjacent] < 0) {
                    queue.push({triangleProxyCost(
                        graph, adjacent, proxies[static_cast<std::size_t>(patch)]), adjacent, patch});
                }
            }
        }
        while (!queue.empty()) {
            const auto item = queue.top();
            queue.pop();
            if (refined[item.triangle] >= 0) continue;
            refined[item.triangle] = item.patch;
            for (const auto adjacent : graph.adjacency[item.triangle]) {
                if (refined[adjacent] < 0) {
                    queue.push({triangleProxyCost(
                        graph, adjacent, proxies[static_cast<std::size_t>(item.patch)]), adjacent, item.patch});
                }
            }
        }
        if (std::find(refined.begin(), refined.end(), -1) != refined.end()) return assignments;
        if (refined == assignments) break;
        assignments = std::move(refined);
    }
    return assignments;
}

bool traverseBoundary(
    const std::map<std::size_t, std::vector<std::size_t>>& boundaryNeighbors,
    std::vector<std::size_t>& boundaryLoop) {
    if (boundaryNeighbors.size() < 3 || std::any_of(
            boundaryNeighbors.begin(), boundaryNeighbors.end(),
            [](const auto& item) { return item.second.size() != 2; })) {
        return false;
    }
    const auto start = boundaryNeighbors.begin()->first;
    auto current = start;
    auto previous = std::numeric_limits<std::size_t>::max();
    do {
        boundaryLoop.push_back(current);
        const auto& adjacent = boundaryNeighbors.at(current);
        const auto next = adjacent[0] == previous ? adjacent[1] : adjacent[0];
        previous = current;
        current = next;
        if (boundaryLoop.size() > boundaryNeighbors.size()) {
            return false;
        }
    } while (current != start);
    return boundaryLoop.size() == boundaryNeighbors.size();
}

double normalizedTriangleQuality(
    const gp_Pnt& first,
    const gp_Pnt& second,
    const gp_Pnt& third) {
    gp_Vec area(first, second);
    area.Cross(gp_Vec(first, third));
    const auto maximumEdgeSquared = std::max({
        first.SquareDistance(second), second.SquareDistance(third), third.SquareDistance(first)});
    return maximumEdgeSquared > 1.0e-24 ? area.Magnitude() / maximumEdgeSquared : 0.0;
}

double signedArea2d(
    const std::array<double, 2>& first,
    const std::array<double, 2>& second,
    const std::array<double, 2>& third) {
    return (second[0] - first[0]) * (third[1] - first[1]) -
        (second[1] - first[1]) * (third[0] - first[0]);
}

bool mapBoundaryToConvexLoop(
    const RegionReferenceMesh& mesh,
    const std::vector<std::size_t>& boundaryLoop,
    const std::vector<double>& cumulative,
    const std::unordered_map<std::size_t, std::size_t>& localIndex,
    RegionPatchParameterization& result,
    std::string& message) {
    for (auto& side : result.boundarySides) side.clear();
    const auto count = boundaryLoop.size();
    if (count < 3 || cumulative.size() != count + 1 || cumulative.back() <= 1.0e-12) {
        message = "A trimmed-disk parameter domain needs at least three non-degenerate boundary vertices.";
        return false;
    }
    Eigen::Vector3d center = Eigen::Vector3d::Zero();
    for (const auto vertex : boundaryLoop) {
        const auto& point = mesh.vertices[vertex].point;
        center += Eigen::Vector3d(point.X(), point.Y(), point.Z());
    }
    center /= static_cast<double>(boundaryLoop.size());
    Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
    for (const auto vertex : boundaryLoop) {
        const auto& point = mesh.vertices[vertex].point;
        const Eigen::Vector3d offset(
            point.X() - center.x(), point.Y() - center.y(), point.Z() - center.z());
        covariance += offset * offset.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eigensolver(covariance);
    std::vector<std::array<double, 2>> projected(count);
    auto convexProjection = eigensolver.info() == Eigen::Success;
    if (convexProjection) {
        const auto firstAxis = eigensolver.eigenvectors().col(2);
        const auto secondAxis = eigensolver.eigenvectors().col(1);
        std::array<double, 2> minimum {
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity()};
        std::array<double, 2> maximum {
            -std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity()};
        for (std::size_t position = 0; position < count; ++position) {
            const auto& point = mesh.vertices[boundaryLoop[position]].point;
            const Eigen::Vector3d offset(
                point.X() - center.x(), point.Y() - center.y(), point.Z() - center.z());
            projected[position] = {offset.dot(firstAxis), offset.dot(secondAxis)};
            for (std::size_t axis = 0; axis < 2; ++axis) {
                minimum[axis] = std::min(minimum[axis], projected[position][axis]);
                maximum[axis] = std::max(maximum[axis], projected[position][axis]);
            }
        }
        constexpr double crossTolerance = 1.0e-12;
        auto orientation = 0;
        for (std::size_t position = 0; position < count; ++position) {
            const auto cross = signedArea2d(
                projected[position],
                projected[(position + 1) % count],
                projected[(position + 2) % count]);
            if (std::abs(cross) <= crossTolerance) continue;
            const auto sign = cross > 0.0 ? 1 : -1;
            if (orientation == 0) orientation = sign;
            else if (orientation != sign) convexProjection = false;
        }
        convexProjection = convexProjection && orientation != 0 &&
            maximum[0] - minimum[0] > 1.0e-12 &&
            maximum[1] - minimum[1] > 1.0e-12;
        if (convexProjection) {
            constexpr double inset = 0.05;
            constexpr double span = 1.0 - 2.0 * inset;
            for (std::size_t position = 0; position < count; ++position) {
                for (std::size_t axis = 0; axis < 2; ++axis) {
                    projected[position][axis] = inset + span *
                        (projected[position][axis] - minimum[axis]) /
                        (maximum[axis] - minimum[axis]);
                }
            }
        }
    }

    constexpr double radius = 0.45;
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t position = 0; position < count; ++position) {
        const auto vertex = boundaryLoop[position];
        const auto parameter = cumulative[position] / cumulative.back();
        if (convexProjection) {
            result.parameters[localIndex.at(vertex)] = projected[position];
        } else {
            const auto angle = 2.0 * pi * parameter;
            result.parameters[localIndex.at(vertex)] = {
                0.5 + radius * std::cos(angle),
                0.5 + radius * std::sin(angle)};
        }
        const auto side = std::min<std::size_t>(
            3, static_cast<std::size_t>(std::floor(parameter * 4.0)));
        result.boundarySides[side].push_back(vertex);
    }
    return true;
}

bool parameterizePatch(
    const RegionReferenceMesh& mesh,
    const std::vector<int>& assignments,
    const int patch,
    const double maximumDistortion,
    const bool allowDistortionViolation,
    const std::chrono::steady_clock::time_point deadline,
    RegionPatchParameterization& result,
    std::string& message) {
    std::set<std::size_t> vertices;
    std::map<MeshEdge, int> edgeUse;
    struct DirectedUse {
        std::size_t triangle = 0;
        int direction = 0;
    };
    std::map<MeshEdge, std::vector<DirectedUse>> directedUses;
    std::vector<std::vector<std::size_t>> triangleAdjacency(mesh.triangles.size());
    std::map<MeshEdge, std::size_t> firstTriangle;
    for (std::size_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
        if (assignments[triangleIndex] != patch) {
            continue;
        }
        result.triangleIndices.push_back(triangleIndex);
        const auto& triangle = mesh.triangles[triangleIndex];
        vertices.insert(triangle.vertices.begin(), triangle.vertices.end());
        for (int edge = 0; edge < 3; ++edge) {
            const auto key = orderedEdge(
                triangle.vertices[static_cast<std::size_t>(edge)],
                triangle.vertices[static_cast<std::size_t>((edge + 1) % 3)]);
            ++edgeUse[key];
            const auto directedFirst = triangle.vertices[static_cast<std::size_t>(edge)];
            const auto directedSecond = triangle.vertices[static_cast<std::size_t>((edge + 1) % 3)];
            directedUses[key].push_back({
                triangleIndex,
                directedFirst < directedSecond ? 1 : -1});
            const auto found = firstTriangle.find(key);
            if (found == firstTriangle.end()) {
                firstTriangle.emplace(key, triangleIndex);
            } else {
                triangleAdjacency[triangleIndex].push_back(found->second);
                triangleAdjacency[found->second].push_back(triangleIndex);
            }
        }
    }
    if (result.triangleIndices.empty()) {
        message = "Layout contains an empty patch.";
        return false;
    }
    std::set<std::size_t> reached;
    std::queue<std::size_t> queue;
    queue.push(result.triangleIndices.front());
    reached.insert(result.triangleIndices.front());
    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop();
        for (const auto next : triangleAdjacency[current]) {
            if (reached.insert(next).second) {
                queue.push(next);
            }
        }
    }
    if (reached.size() != result.triangleIndices.size()) {
        message = "Layout patch is disconnected.";
        return false;
    }
    std::vector<int> triangleOrientation(mesh.triangles.size(), 0);
    triangleOrientation[result.triangleIndices.front()] = 1;
    queue.push(result.triangleIndices.front());
    while (!queue.empty()) {
        const auto currentTriangle = queue.front();
        queue.pop();
        const auto& triangle = mesh.triangles[currentTriangle];
        for (int edgeIndex = 0; edgeIndex < 3; ++edgeIndex) {
            const auto key = orderedEdge(
                triangle.vertices[static_cast<std::size_t>(edgeIndex)],
                triangle.vertices[static_cast<std::size_t>((edgeIndex + 1) % 3)]);
            const auto& uses = directedUses.at(key);
            if (uses.size() != 2) {
                continue;
            }
            const auto current = uses[0].triangle == currentTriangle ? uses[0] : uses[1];
            const auto other = uses[0].triangle == currentTriangle ? uses[1] : uses[0];
            const auto expected = -current.direction * triangleOrientation[currentTriangle] * other.direction;
            if (triangleOrientation[other.triangle] == 0) {
                triangleOrientation[other.triangle] = expected;
                queue.push(other.triangle);
            } else if (triangleOrientation[other.triangle] != expected) {
                message = "Layout patch triangle mesh is not orientable.";
                return false;
            }
        }
    }

    std::map<std::size_t, std::vector<std::size_t>> boundaryNeighbors;
    for (const auto& [edge, useCount] : edgeUse) {
        if (useCount > 2) {
            message = "Layout patch contains a non-manifold edge.";
            return false;
        }
        if (useCount == 1) {
            boundaryNeighbors[edge[0]].push_back(edge[1]);
            boundaryNeighbors[edge[1]].push_back(edge[0]);
        }
    }
    if (static_cast<long long>(vertices.size()) - static_cast<long long>(edgeUse.size()) +
            static_cast<long long>(result.triangleIndices.size()) != 1 ||
        !traverseBoundary(boundaryNeighbors, result.boundaryLoop)) {
        message = "Layout patch is not a single-boundary disk.";
        return false;
    }
    result.diskTopologyPassed = true;

    result.vertexIndices.assign(vertices.begin(), vertices.end());
    result.parameters.resize(result.vertexIndices.size());
    std::unordered_map<std::size_t, std::size_t> localIndex;
    localIndex.reserve(result.vertexIndices.size());
    for (std::size_t index = 0; index < result.vertexIndices.size(); ++index) {
        localIndex.emplace(result.vertexIndices[index], index);
    }
    std::vector<std::set<std::size_t>> neighbors(result.vertexIndices.size());
    for (const auto& [edge, useCount] : edgeUse) {
        (void)useCount;
        const auto first = localIndex.at(edge[0]);
        const auto second = localIndex.at(edge[1]);
        neighbors[first].insert(second);
        neighbors[second].insert(first);
    }
    std::vector<double> cumulative(result.boundaryLoop.size() + 1, 0.0);
    for (std::size_t index = 0; index < result.boundaryLoop.size(); ++index) {
        cumulative[index + 1] = cumulative[index] + mesh.vertices[result.boundaryLoop[index]].point.Distance(
            mesh.vertices[result.boundaryLoop[(index + 1) % result.boundaryLoop.size()]].point);
    }
    if (cumulative.back() <= 1.0e-12) {
        message = "Layout patch boundary length is degenerate.";
        return false;
    }
    std::set<std::size_t> boundaryLocal;
    for (const auto vertex : result.boundaryLoop) boundaryLocal.insert(localIndex.at(vertex));
    std::unordered_map<std::size_t, std::size_t> boundaryPosition;
    boundaryPosition.reserve(result.boundaryLoop.size());
    for (std::size_t position = 0; position < result.boundaryLoop.size(); ++position) {
        boundaryPosition.emplace(result.boundaryLoop[position], position);
    }
    std::vector<std::size_t> requiredCorners;
    std::map<MeshEdge, std::vector<int>> globalEdgePatches;
    for (std::size_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
        const auto& triangle = mesh.triangles[triangleIndex];
        for (int edge = 0; edge < 3; ++edge) {
            globalEdgePatches[orderedEdge(
                triangle.vertices[static_cast<std::size_t>(edge)],
                triangle.vertices[static_cast<std::size_t>((edge + 1) % 3)])]
                .push_back(assignments[triangleIndex]);
        }
    }
    std::vector<int> boundaryLabels(result.boundaryLoop.size(), -1);
    for (std::size_t position = 0; position < result.boundaryLoop.size(); ++position) {
        const auto edge = orderedEdge(
            result.boundaryLoop[position],
            result.boundaryLoop[(position + 1) % result.boundaryLoop.size()]);
        const auto& uses = globalEdgePatches.at(edge);
        if (uses.size() > 1) {
            const auto other = std::find_if(uses.begin(), uses.end(), [&](const auto owner) {
                return owner != patch;
            });
            if (other != uses.end()) boundaryLabels[position] = *other;
        }
    }
    std::map<int, int> chainCounts;
    for (std::size_t position = 0; position < boundaryLabels.size(); ++position) {
        const auto previous = boundaryLabels[
            (position + boundaryLabels.size() - 1) % boundaryLabels.size()];
        const auto current = boundaryLabels[position];
        if (current != previous || boundaryLabels.size() == 1) {
            ++chainCounts[current];
            if (current != previous) requiredCorners.push_back(position);
        }
    }
    if (chainCounts.empty() && !boundaryLabels.empty()) chainCounts[boundaryLabels.front()] = 1;
    std::vector<std::size_t> chainStarts;
    for (std::size_t position = 0; position < boundaryLabels.size(); ++position) {
        const auto previous = boundaryLabels[
            (position + boundaryLabels.size() - 1) % boundaryLabels.size()];
        if (position == 0 && std::all_of(
                boundaryLabels.begin(), boundaryLabels.end(),
                [&](const auto label) { return label == boundaryLabels.front(); })) {
            chainStarts.push_back(0);
            break;
        }
        if (boundaryLabels[position] != previous) chainStarts.push_back(position);
    }
    for (const auto start : chainStarts) {
        RegionBoundaryChain chain;
        chain.adjacentPatch = boundaryLabels[start];
        chain.originalBoundary = chain.adjacentPatch < 0;
        auto position = start;
        do {
            const auto vertex = result.boundaryLoop[position];
            chain.vertices.push_back(vertex);
            if (chain.originalBoundary) {
                const auto& edgeIds = mesh.vertices[vertex].boundaryEdgeIds;
                chain.originalEdgeIds.insert(
                    chain.originalEdgeIds.end(), edgeIds.begin(), edgeIds.end());
            }
            position = (position + 1) % boundaryLabels.size();
        } while (position != start && boundaryLabels[position] == chain.adjacentPatch);
        chain.vertices.push_back(result.boundaryLoop[position]);
        std::sort(chain.originalEdgeIds.begin(), chain.originalEdgeIds.end());
        chain.originalEdgeIds.erase(
            std::unique(chain.originalEdgeIds.begin(), chain.originalEdgeIds.end()),
            chain.originalEdgeIds.end());
        result.boundaryChains.push_back(std::move(chain));
    }
    result.boundaryChainCount = result.boundaryChains.size();
    for (const auto triangleIndex : result.triangleIndices) {
        const auto& triangle = mesh.triangles[triangleIndex];
        if (!boundaryPosition.contains(triangle.vertices[0]) ||
            !boundaryPosition.contains(triangle.vertices[1]) ||
            !boundaryPosition.contains(triangle.vertices[2])) {
            continue;
        }
        for (const auto vertex : triangle.vertices) {
            const auto position = boundaryPosition.at(vertex);
            const auto previous = result.boundaryLoop[
                (position + result.boundaryLoop.size() - 1) % result.boundaryLoop.size()];
            const auto next = result.boundaryLoop[(position + 1) % result.boundaryLoop.size()];
            const auto contains = [&](const std::size_t candidate) {
                return std::find(triangle.vertices.begin(), triangle.vertices.end(), candidate) !=
                    triangle.vertices.end();
            };
            if (contains(previous) && contains(next)) requiredCorners.push_back(position);
        }
    }
    std::sort(requiredCorners.begin(), requiredCorners.end());
    requiredCorners.erase(std::unique(requiredCorners.begin(), requiredCorners.end()), requiredCorners.end());
    std::vector<std::pair<double, std::size_t>> outerTurningCorners;
    for (std::size_t position = 0; position < result.boundaryLoop.size(); ++position) {
        const auto previousPosition =
            (position + result.boundaryLoop.size() - 1) % result.boundaryLoop.size();
        if (boundaryLabels[previousPosition] != -1 || boundaryLabels[position] != -1) continue;
        const auto& previous = mesh.vertices[result.boundaryLoop[previousPosition]].point;
        const auto& current = mesh.vertices[result.boundaryLoop[position]].point;
        const auto& next = mesh.vertices[
            result.boundaryLoop[(position + 1) % result.boundaryLoop.size()]].point;
        gp_Vec incoming(previous, current);
        gp_Vec outgoing(current, next);
        if (incoming.SquareMagnitude() <= 1.0e-24 || outgoing.SquareMagnitude() <= 1.0e-24) continue;
        incoming.Normalize();
        outgoing.Normalize();
        const auto turning = 1.0 - std::clamp(incoming.Dot(outgoing), -1.0, 1.0);
        if (turning > 1.0e-4) outerTurningCorners.emplace_back(turning, position);
    }
    std::sort(outerTurningCorners.begin(), outerTurningCorners.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first != rhs.first ? lhs.first > rhs.first : lhs.second < rhs.second;
    });
    for (const auto& [turning, position] : outerTurningCorners) {
        (void)turning;
        if (std::find(requiredCorners.begin(), requiredCorners.end(), position) == requiredCorners.end()) {
            requiredCorners.push_back(position);
        }
    }
    for (std::size_t position = 0; position < result.boundaryLoop.size(); ++position) {
        const auto& vertex = mesh.vertices[result.boundaryLoop[position]];
        if (vertex.hardCorner || vertex.boundaryEndpoint) requiredCorners.push_back(position);
    }
    std::sort(requiredCorners.begin(), requiredCorners.end());
    requiredCorners.erase(std::unique(requiredCorners.begin(), requiredCorners.end()), requiredCorners.end());
    result.trimCorners.reserve(requiredCorners.size());
    for (const auto corner : requiredCorners) {
        result.trimCorners.push_back(result.boundaryLoop[corner]);
    }
    result.trimCornerCount = static_cast<int>(result.trimCorners.size());
    if (!mapBoundaryToConvexLoop(
            mesh,
            result.boundaryLoop,
            cumulative,
            localIndex,
            result,
            message)) {
        return false;
    }
    std::vector<std::size_t> interior;
    std::vector<int> interiorIndex(result.vertexIndices.size(), -1);
    for (std::size_t vertex = 0; vertex < result.vertexIndices.size(); ++vertex) {
        if (!boundaryLocal.contains(vertex)) {
            interiorIndex[vertex] = static_cast<int>(interior.size());
            interior.push_back(vertex);
        }
    }
    result.matrixRows = interior.size();
    result.matrixNonZeros = interior.size();
    for (const auto vertex : interior) {
        result.matrixNonZeros += std::count_if(
            neighbors[vertex].begin(), neighbors[vertex].end(),
            [&](const auto neighbor) { return !boundaryLocal.contains(neighbor); });
    }
    if (!interior.empty()) {
        Eigen::Vector3d center = Eigen::Vector3d::Zero();
        for (const auto vertex : result.vertexIndices) {
            const auto& point = mesh.vertices[vertex].point;
            center += Eigen::Vector3d(point.X(), point.Y(), point.Z());
        }
        center /= static_cast<double>(result.vertexIndices.size());
        Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
        for (const auto vertex : result.vertexIndices) {
            const auto& point = mesh.vertices[vertex].point;
            const Eigen::Vector3d offset(point.X() - center.x(), point.Y() - center.y(), point.Z() - center.z());
            covariance += offset * offset.transpose();
        }
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eigensolver(covariance);
        if (eigensolver.info() != Eigen::Success) {
            message = "Harmonic parameterization PCA initialization failed.";
            return false;
        }
        const auto firstAxis = eigensolver.eigenvectors().col(2);
        const auto secondAxis = eigensolver.eigenvectors().col(1);
        std::array<double, 2> minimumProjection {
            std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
        std::array<double, 2> maximumProjection {
            -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
        for (std::size_t local = 0; local < result.vertexIndices.size(); ++local) {
            const auto& point = mesh.vertices[result.vertexIndices[local]].point;
            const Eigen::Vector3d offset(point.X() - center.x(), point.Y() - center.y(), point.Z() - center.z());
            result.parameters[local] = {offset.dot(firstAxis), offset.dot(secondAxis)};
            for (int axis = 0; axis < 2; ++axis) {
                minimumProjection[static_cast<std::size_t>(axis)] = std::min(
                    minimumProjection[static_cast<std::size_t>(axis)], result.parameters[local][static_cast<std::size_t>(axis)]);
                maximumProjection[static_cast<std::size_t>(axis)] = std::max(
                    maximumProjection[static_cast<std::size_t>(axis)], result.parameters[local][static_cast<std::size_t>(axis)]);
            }
        }
        const auto projectionScale = 0.75 / std::max({
            maximumProjection[0] - minimumProjection[0],
            maximumProjection[1] - minimumProjection[1],
            1.0e-12});
        for (auto& parameter : result.parameters) {
            parameter[0] = 0.5 +
                (parameter[0] - (minimumProjection[0] + maximumProjection[0]) * 0.5) * projectionScale;
            parameter[1] = 0.5 +
                (parameter[1] - (minimumProjection[1] + maximumProjection[1]) * 0.5) * projectionScale;
        }
        if (!mapBoundaryToConvexLoop(
                mesh,
                result.boundaryLoop,
                cumulative,
                localIndex,
                result,
                message)) {
            return false;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            message = "Harmonic parameterization reached the layout deadline.";
            return false;
        }
        using SparseMatrix = Eigen::SparseMatrix<double>;
        std::vector<Eigen::Triplet<double>> coefficients;
        coefficients.reserve(result.matrixNonZeros);
        Eigen::VectorXd rightU = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(interior.size()));
        Eigen::VectorXd rightV = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(interior.size()));
        for (const auto vertex : interior) {
            const auto row = interiorIndex[vertex];
            coefficients.emplace_back(row, row, static_cast<double>(neighbors[vertex].size()));
            for (const auto neighbor : neighbors[vertex]) {
                const auto column = interiorIndex[neighbor];
                if (column >= 0) {
                    coefficients.emplace_back(row, column, -1.0);
                } else {
                    rightU[row] += result.parameters[neighbor][0];
                    rightV[row] += result.parameters[neighbor][1];
                }
            }
        }
        SparseMatrix matrix(
            static_cast<Eigen::Index>(interior.size()),
            static_cast<Eigen::Index>(interior.size()));
        matrix.setFromTriplets(coefficients.begin(), coefficients.end());
        matrix.makeCompressed();
        Eigen::ConjugateGradient<
            SparseMatrix,
            Eigen::Lower | Eigen::Upper,
            Eigen::DiagonalPreconditioner<double>> solver;
        solver.setMaxIterations(static_cast<Eigen::Index>(interior.size()));
        solver.setTolerance(std::sqrt(std::numeric_limits<double>::epsilon()));
        solver.compute(matrix);
        if (solver.info() != Eigen::Success) {
            message = "Harmonic parameterization iterative solver setup failed.";
            return false;
        }
        const Eigen::VectorXd solutionU = solver.solve(rightU);
        if (solver.info() != Eigen::Success) {
            message = "Harmonic parameterization U solve did not converge.";
            return false;
        }
        const Eigen::VectorXd solutionV = solver.solve(rightV);
        if (solver.info() != Eigen::Success) {
            message = "Harmonic parameterization V solve did not converge.";
            return false;
        }
        for (const auto vertex : interior) {
            const auto row = interiorIndex[vertex];
            result.parameters[vertex] = {solutionU[row], solutionV[row]};
        }
        double maximumResidual = 0.0;
        for (const auto vertex : interior) {
            std::array<double, 2> average {};
            for (const auto neighbor : neighbors[vertex]) {
                average[0] += result.parameters[neighbor][0];
                average[1] += result.parameters[neighbor][1];
            }
            for (int axis = 0; axis < 2; ++axis) {
                average[static_cast<std::size_t>(axis)] /= static_cast<double>(neighbors[vertex].size());
                maximumResidual = std::max(maximumResidual, std::abs(
                    average[static_cast<std::size_t>(axis)] - result.parameters[vertex][static_cast<std::size_t>(axis)]));
            }
        }
        if (!std::isfinite(maximumResidual) || maximumResidual > 5.0e-5) {
            message = "Harmonic parameterization did not converge within its iteration budget.";
            return false;
        }
    }

    int positive = 0;
    int negative = 0;
    std::vector<double> distortions;
    distortions.reserve(result.triangleIndices.size());
    for (const auto triangleIndex : result.triangleIndices) {
        const auto& triangle = mesh.triangles[triangleIndex];
        const auto signedArea = static_cast<double>(triangleOrientation[triangleIndex]) * signedArea2d(
            result.parameters[localIndex.at(triangle.vertices[0])],
            result.parameters[localIndex.at(triangle.vertices[1])],
            result.parameters[localIndex.at(triangle.vertices[2])]);
        if (signedArea > 0.0) ++positive;
        else if (signedArea < 0.0) ++negative;
        else ++result.flippedTriangleCount;
    }
    if (negative > positive) {
        for (auto& parameter : result.parameters) {
            parameter[1] = 1.0 - parameter[1];
        }
        std::swap(result.boundarySides[0], result.boundarySides[2]);
        std::reverse(result.boundarySides[0].begin(), result.boundarySides[0].end());
        std::reverse(result.boundarySides[1].begin(), result.boundarySides[1].end());
        std::reverse(result.boundarySides[2].begin(), result.boundarySides[2].end());
        std::reverse(result.boundarySides[3].begin(), result.boundarySides[3].end());
        std::swap(positive, negative);
    }
    result.flippedTriangleCount += negative;
    if (result.flippedTriangleCount > 0) {
        message = "Harmonic parameterization contains flipped or collapsed triangles.";
        return false;
    }
    result.minimumSignedUvArea = std::numeric_limits<double>::infinity();
    for (const auto triangleIndex : result.triangleIndices) {
        const auto& triangle = mesh.triangles[triangleIndex];
        const auto first = localIndex.at(triangle.vertices[0]);
        const auto second = localIndex.at(triangle.vertices[1]);
        const auto third = localIndex.at(triangle.vertices[2]);
        const auto quality3d = normalizedTriangleQuality(
            mesh.vertices[triangle.vertices[0]].point,
            mesh.vertices[triangle.vertices[1]].point,
            mesh.vertices[triangle.vertices[2]].point);
        const gp_Pnt uvFirst(result.parameters[first][0], result.parameters[first][1], 0.0);
        const gp_Pnt uvSecond(result.parameters[second][0], result.parameters[second][1], 0.0);
        const gp_Pnt uvThird(result.parameters[third][0], result.parameters[third][1], 0.0);
        const auto quality2d = normalizedTriangleQuality(uvFirst, uvSecond, uvThird);
        const auto signedArea = static_cast<double>(triangleOrientation[triangleIndex]) * signedArea2d(
            result.parameters[first], result.parameters[second], result.parameters[third]);
        result.minimumSignedUvArea = std::min(result.minimumSignedUvArea, signedArea);
        if (quality3d <= 1.0e-14 || quality2d <= 1.0e-14) {
            message = "Harmonic parameterization contains a degenerate triangle.";
            return false;
        }
        distortions.push_back(std::max(quality3d / quality2d, quality2d / quality3d));
    }
    if (!distortions.empty()) {
        const auto percentileIndex = std::min(
            distortions.size() - 1,
            static_cast<std::size_t>(std::ceil(static_cast<double>(distortions.size()) * 0.995)) - 1);
        std::nth_element(
            distortions.begin(),
            distortions.begin() + static_cast<std::ptrdiff_t>(percentileIndex),
            distortions.end());
        result.maximumDistortion = distortions[percentileIndex];
    }
    if (result.maximumDistortion > maximumDistortion) {
        std::ostringstream stream;
        stream << "Harmonic parameterization 99.5th-percentile distortion "
               << result.maximumDistortion << " exceeds the limit " << maximumDistortion << '.';
        message = stream.str();
        if (!allowDistortionViolation) return false;
    }
    return true;
}

std::string layoutHash(const RegionPatchLayout& layout) {
    std::uint64_t hash = 1469598103934665603ull;
    auto append = [&](const std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    append(static_cast<std::uint64_t>(layout.patchCount));
    for (const auto patch : layout.trianglePatches) {
        append(static_cast<std::uint64_t>(patch));
    }
    for (const auto& parameterization : layout.patches) {
        for (const auto& uv : parameterization.parameters) {
            append(static_cast<std::uint64_t>(std::llround(uv[0] * 1.0e12)));
            append(static_cast<std::uint64_t>(std::llround(uv[1] * 1.0e12)));
        }
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

void buildSeams(const MeshGraph& graph, RegionPatchLayout& layout) {
    std::map<std::pair<int, int>, std::vector<MeshEdge>> grouped;
    for (const auto& [edge, triangles] : graph.edgeTriangles) {
        if (triangles.size() != 2) {
            continue;
        }
        const auto first = layout.trianglePatches[triangles[0]];
        const auto second = layout.trianglePatches[triangles[1]];
        if (first == second) {
            continue;
        }
        grouped[{std::min(first, second), std::max(first, second)}].push_back(edge);
    }
    for (auto& [patches, edges] : grouped) {
        std::set<std::size_t> remaining;
        for (std::size_t index = 0; index < edges.size(); ++index) remaining.insert(index);
        while (!remaining.empty()) {
            std::vector<MeshEdge> component;
            std::queue<std::size_t> pending;
            pending.push(*remaining.begin());
            remaining.erase(remaining.begin());
            while (!pending.empty()) {
                const auto index = pending.front();
                pending.pop();
                component.push_back(edges[index]);
                for (auto found = remaining.begin(); found != remaining.end();) {
                    const auto& candidate = edges[*found];
                    const auto connected =
                        candidate[0] == edges[index][0] ||
                        candidate[0] == edges[index][1] ||
                        candidate[1] == edges[index][0] ||
                        candidate[1] == edges[index][1];
                    if (connected) {
                        pending.push(*found);
                        found = remaining.erase(found);
                    } else {
                        ++found;
                    }
                }
            }
            std::sort(component.begin(), component.end());
            layout.seams.push_back({
                patches.first, patches.second, std::move(component)});
        }
    }
    std::sort(layout.seams.begin(), layout.seams.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.firstPatch, lhs.secondPatch, lhs.meshEdges) <
            std::tie(rhs.firstPatch, rhs.secondPatch, rhs.meshEdges);
    });
}

std::uint64_t estimateSolverBytes(
    const RegionPatchLayout& layout,
    const RegionReferenceMesh& mesh,
    std::size_t& rows,
    std::size_t& nonZeros) {
    const auto controlVariables = static_cast<std::size_t>(layout.patchCount) * 6u * 6u * 3u;
    rows = mesh.vertices.size() * 3u + mesh.boundarySamples.size() * 3u;
    for (const auto& seam : layout.seams) {
        rows += seam.meshEdges.size() * 3u;
    }
    nonZeros = mesh.vertices.size() * 16u * 3u + rows * 4u + controlVariables * 5u;
    return static_cast<std::uint64_t>(nonZeros) * 16ull * 8ull +
        static_cast<std::uint64_t>(controlVariables + rows) * 64ull;
}

std::vector<std::size_t> paretoSelection(const std::vector<RegionPatchLayout>& candidates) {
    const auto maximumDistortion = [](const RegionPatchLayout& layout) {
        auto value = 0.0;
        for (const auto& patch : layout.patches) value = std::max(value, patch.maximumDistortion);
        return value;
    };
    std::vector<std::size_t> frontier;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        bool dominated = false;
        for (std::size_t other = 0; other < candidates.size() && !dominated; ++other) {
            if (other == index) continue;
            const auto& lhs = candidates[other];
            const auto& rhs = candidates[index];
            const auto noWorse = lhs.patchCount <= rhs.patchCount &&
                lhs.proxyError <= rhs.proxyError &&
                maximumDistortion(lhs) <= maximumDistortion(rhs) &&
                lhs.estimatedSolverBytes <= rhs.estimatedSolverBytes &&
                lhs.estimatedOutputEntities <= rhs.estimatedOutputEntities;
            const auto strictlyBetter = lhs.patchCount < rhs.patchCount ||
                lhs.proxyError < rhs.proxyError - 1.0e-15 ||
                maximumDistortion(lhs) < maximumDistortion(rhs) - 1.0e-15 ||
                lhs.estimatedSolverBytes < rhs.estimatedSolverBytes ||
                lhs.estimatedOutputEntities < rhs.estimatedOutputEntities;
            dominated = noWorse && strictlyBetter;
        }
        if (!dominated) frontier.push_back(index);
    }
    if (frontier.size() <= 3) {
        return frontier;
    }
    const auto minimumPatch = *std::min_element(frontier.begin(), frontier.end(), [&](const auto lhs, const auto rhs) {
        return candidates[lhs].patchCount < candidates[rhs].patchCount;
    });
    const auto minimumError = *std::min_element(frontier.begin(), frontier.end(), [&](const auto lhs, const auto rhs) {
        return candidates[lhs].proxyError < candidates[rhs].proxyError;
    });
    auto knee = minimumPatch;
    auto bestImprovement = -std::numeric_limits<double>::infinity();
    std::vector<std::size_t> ordered = frontier;
    std::sort(ordered.begin(), ordered.end(), [&](const auto lhs, const auto rhs) {
        return candidates[lhs].patchCount < candidates[rhs].patchCount;
    });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        const auto previous = candidates[ordered[index - 1]].proxyError;
        const auto current = candidates[ordered[index]].proxyError;
        const auto patchDelta = std::max(
            1,
            candidates[ordered[index]].patchCount - candidates[ordered[index - 1]].patchCount);
        const auto improvement = (previous - current) / static_cast<double>(patchDelta);
        if (improvement > bestImprovement) {
            bestImprovement = improvement;
            knee = ordered[index];
        }
    }
    std::vector<std::size_t> selected {minimumPatch, knee, minimumError};
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    return selected;
}

std::vector<int> requestedPatchCounts(
    const RegionRefitLayoutMode mode,
    const RegionRefitOptions& options) {
    if (mode == RegionRefitLayoutMode::FixedPatchCount) return {options.fixedPatchCount};
    if (mode == RegionRefitLayoutMode::Adaptive) {
        std::vector<int> counts(static_cast<std::size_t>(options.maximumAdaptivePatchCount));
        std::iota(counts.begin(), counts.end(), 1);
        return counts;
    }
    std::vector<int> counts;
    counts.push_back(options.requestedPatchCount);
    for (int offset = 1; offset <= options.patchCountSearchRadius; ++offset) {
        if (options.requestedPatchCount - offset > 0) {
            counts.push_back(options.requestedPatchCount - offset);
        }
        counts.push_back(options.requestedPatchCount + offset);
    }
    return counts;
}

}

bool RegionPatchLayoutGenerationResult::success() const {
    return status == RegionRefitStatus::Success && !selectedCandidates.empty();
}

RegionPatchLayoutGenerationResult RegionPatchLayoutGenerator::generate(
    const RegionReferenceMesh& mesh,
    const RegionRefitLayoutMode mode,
    const RegionRefitOptions& options,
    const bool allowDiagnosticCandidates) const {
    RegionPatchLayoutGenerationResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(options.layoutBudgetMilliseconds);
    const auto expired = [&] {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count() >
            static_cast<double>(options.layoutBudgetMilliseconds);
    };
    MeshGraph graph;
    if (!buildGraph(mesh, graph, result.message)) {
        result.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        return result;
    }
    const auto patchCounts = requestedPatchCounts(mode, options);
    const auto maximumPatchCount = patchCounts.empty()
        ? 0
        : *std::max_element(patchCounts.begin(), patchCounts.end());
    if (patchCounts.empty() || patchCounts.front() <= 0 || maximumPatchCount <= 0 ||
        static_cast<std::size_t>(maximumPatchCount) > mesh.triangles.size()) {
        result.status = RegionRefitStatus::InvalidRequest;
        result.message = "Requested patch count is not valid for the reference mesh.";
        return result;
    }
    const auto seeds = deterministicSeeds(graph, maximumPatchCount);
    if (static_cast<int>(seeds.size()) != maximumPatchCount) {
        result.message = "Could not generate the deterministic layout seed family.";
        return result;
    }

    std::vector<RegionPatchLayout> accepted;
    std::vector<RegionPatchLayout> constructible;
    bool deadlineReached = false;
    for (const int patchCount : patchCounts) {
        if (expired()) {
            deadlineReached = true;
            break;
        }
        const auto candidateStarted = std::chrono::steady_clock::now();
        RegionLayoutCandidateReport report;
        report.patchCount = patchCount;
        RegionPatchLayout layout;
        layout.patchCount = patchCount;
        const auto initialAssignments = assignConnectedRegions(graph, seeds, patchCount);
        const auto refinedAssignments = refineConnectedRegions(
            graph, initialAssignments, patchCount);
        const auto tryParameterization = [&](const std::vector<int>& assignments) {
            layout.trianglePatches = assignments;
            layout.patches.assign(static_cast<std::size_t>(patchCount), {});
            report.maximumDistortion = 0.0;
            report.flippedTriangleCount = 0;
            report.matrixRows = 0;
            report.matrixNonZeros = 0;
            report.rejectionReason.clear();
            report.topologyPassed =
                std::find(assignments.begin(), assignments.end(), -1) == assignments.end();
            if (!report.topologyPassed) return false;
            for (int patch = 0; patch < patchCount; ++patch) {
                std::string failure;
                if (!parameterizePatch(
                        mesh,
                        layout.trianglePatches,
                        patch,
                        options.maximumParameterDistortion,
                        allowDiagnosticCandidates,
                        deadline,
                        layout.patches[static_cast<std::size_t>(patch)],
                        failure)) {
                    const auto& failedPatch = layout.patches[static_cast<std::size_t>(patch)];
                    report.maximumDistortion = failedPatch.maximumDistortion;
                    report.flippedTriangleCount = failedPatch.flippedTriangleCount;
                    report.matrixRows = failedPatch.matrixRows;
                    report.matrixNonZeros = failedPatch.matrixNonZeros;
                    report.topologyPassed = failure.find("not a single-boundary disk") == std::string::npos &&
                        failure.find("disconnected") == std::string::npos &&
                        failure.find("non-manifold") == std::string::npos;
                    report.rejectionReason = std::move(failure);
                    return false;
                }
            }
            return true;
        };
        auto parameterized = tryParameterization(refinedAssignments);
        if (!parameterized && refinedAssignments != initialAssignments && !expired()) {
            parameterized = tryParameterization(initialAssignments);
        }
        layout.proxyError = proxyError(graph, layout.trianglePatches, patchCount);
        report.proxyError = layout.proxyError;
        report.topologyPassed = report.topologyPassed && std::isfinite(layout.proxyError);
        if (expired()) {
            result.candidates.push_back(std::move(report));
            deadlineReached = true;
            break;
        }
        if (parameterized && report.topologyPassed) {
            report.constructibleParameterization = true;
            for (const auto& patch : layout.patches) {
                report.maximumDistortion = std::max(report.maximumDistortion, patch.maximumDistortion);
                report.flippedTriangleCount += patch.flippedTriangleCount;
                report.boundaryChainCount += patch.boundaryChainCount;
                report.trimCornerCount += patch.trimCornerCount;
                report.minimumSignedUvArea = report.minimumSignedUvArea == 0.0
                    ? patch.minimumSignedUvArea
                    : std::min(report.minimumSignedUvArea, patch.minimumSignedUvArea);
                if (report.domainType.empty()) report.domainType = patch.domainType;
                report.matrixRows += patch.matrixRows;
                report.matrixNonZeros += patch.matrixNonZeros;
            }
            report.parameterizationPassed =
                report.flippedTriangleCount == 0 &&
                report.maximumDistortion <= options.maximumParameterDistortion;
            if (!report.parameterizationPassed) {
                std::ostringstream stream;
                stream << "Trimmed-disk parameterization distortion "
                       << report.maximumDistortion << " exceeds the product limit "
                       << options.maximumParameterDistortion << '.';
                report.rejectionReason = stream.str();
            }
            buildSeams(graph, layout);
            std::size_t fitRows = 0;
            std::size_t fitNonZeros = 0;
            layout.estimatedSolverBytes = estimateSolverBytes(layout, mesh, fitRows, fitNonZeros);
            layout.estimatedOutputEntities = static_cast<std::uint64_t>(patchCount) * 64ull +
                static_cast<std::uint64_t>(layout.seams.size()) * 24ull;
            report.estimatedOutputEntities = layout.estimatedOutputEntities;
            report.matrixRows += fitRows;
            report.matrixNonZeros += fitNonZeros;
            report.estimatedSolverBytes = layout.estimatedSolverBytes;
            const auto solverLimit = static_cast<std::uint64_t>(options.solverMemoryPreflightMiB) * 1024ull * 1024ull;
            report.coarseFitPassed = layout.estimatedSolverBytes <= solverLimit;
            if (!report.coarseFitPassed) {
                report.rejectionReason = "Estimated joint-fit memory exceeds the preflight limit.";
            } else {
                layout.hash = layoutHash(layout);
                report.layoutHash = layout.hash;
                constructible.push_back(layout);
                result.constructibleCandidates.push_back(layout);
                if (report.parameterizationPassed) accepted.push_back(std::move(layout));
            }
        } else if (report.rejectionReason.empty()) {
            report.rejectionReason = "Layout did not cover the reference mesh.";
        }
        report.wallMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - candidateStarted).count();
        result.candidates.push_back(std::move(report));
    }
    if (accepted.empty() && (!allowDiagnosticCandidates || constructible.empty())) {
        result.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.message = "No deterministic layout passed topology, parameterization, and memory preflight.";
        return result;
    }
    if (allowDiagnosticCandidates) {
        std::sort(constructible.begin(), constructible.end(), [&](const auto& lhs, const auto& rhs) {
            const auto reportFor = [&](const int patchCount) -> const RegionLayoutCandidateReport& {
                return *std::find_if(result.candidates.begin(), result.candidates.end(), [&](const auto& candidate) {
                    return candidate.patchCount == patchCount;
                });
            };
            const auto& lhsReport = reportFor(lhs.patchCount);
            const auto& rhsReport = reportFor(rhs.patchCount);
            const auto hardGateCount = [](const auto& report) {
                return static_cast<int>(report.topologyPassed) +
                    static_cast<int>(report.constructibleParameterization) +
                    static_cast<int>(report.coarseFitPassed);
            };
            const auto lhsViolation = lhsReport.maximumDistortion /
                std::max(options.maximumParameterDistortion, 1.0e-12);
            const auto rhsViolation = rhsReport.maximumDistortion /
                std::max(options.maximumParameterDistortion, 1.0e-12);
            return std::tuple {
                -hardGateCount(lhsReport),
                lhsViolation,
                lhs.patchCount,
                lhs.proxyError,
                lhs.hash} <
                std::tuple {
                -hardGateCount(rhsReport),
                rhsViolation,
                rhs.patchCount,
                rhs.proxyError,
                rhs.hash};
        });
        const auto selectedCount = mode == RegionRefitLayoutMode::Adaptive
            ? std::min<std::size_t>(
                constructible.size(),
                static_cast<std::size_t>(options.maximumFullFitCount))
            : std::size_t {1};
        result.selectedCandidates.assign(
            constructible.begin(),
            constructible.begin() + static_cast<std::ptrdiff_t>(selectedCount));
    } else if (!accepted.empty()) {
        const auto selection = mode != RegionRefitLayoutMode::Adaptive
            ? std::vector<std::size_t> {0}
            : paretoSelection(accepted);
        for (const auto index : selection) {
            result.selectedCandidates.push_back(accepted[index]);
        }
    }
    for (const auto& selected : result.selectedCandidates) {
        const auto patchCount = selected.patchCount;
        const auto found = std::find_if(result.candidates.begin(), result.candidates.end(), [&](const auto& report) {
            return report.patchCount == patchCount;
        });
        if (found != result.candidates.end()) {
            found->selectedForFullFit = true;
        }
    }
    if (mode == RegionRefitLayoutMode::Adaptive) {
        std::sort(result.selectedCandidates.begin(), result.selectedCandidates.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.patchCount < rhs.patchCount;
        });
    }
    result.status = RegionRefitStatus::Success;
    result.message = deadlineReached
        ? "Layout deadline reached after retaining the coarse-screened candidate prefix."
        : (accepted.empty()
            ? "Retained constructible trimmed-disk layouts for forced diagnostic fitting."
            : "Deterministic layouts passed coarse screening.");
    return result;
}

}
