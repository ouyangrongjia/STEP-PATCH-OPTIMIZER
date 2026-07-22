#include "reduction/region_refit/QualityBalancedRegionRemesher.h"

#include <BRepTools.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace spo {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct MeshEdge {
    std::size_t first = 0;
    std::size_t second = 0;

    MeshEdge() = default;
    MeshEdge(const std::size_t a, const std::size_t b)
        : first(std::min(a, b)), second(std::max(a, b)) {}

    bool operator<(const MeshEdge& other) const {
        return std::tie(first, second) < std::tie(other.first, other.second);
    }
};

using EdgeTriangles = std::map<MeshEdge, std::vector<std::size_t>>;

EdgeTriangles buildEdgeTriangles(const RegionReferenceMesh& mesh) {
    EdgeTriangles result;
    for (std::size_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
        const auto& triangle = mesh.triangles[triangleIndex];
        for (int edge = 0; edge < 3; ++edge) {
            result[MeshEdge(
                triangle.vertices[static_cast<std::size_t>(edge)],
                triangle.vertices[static_cast<std::size_t>((edge + 1) % 3)])].push_back(triangleIndex);
        }
    }
    return result;
}

std::vector<std::set<std::size_t>> buildNeighbors(const RegionReferenceMesh& mesh) {
    std::vector<std::set<std::size_t>> result(mesh.vertices.size());
    for (const auto& triangle : mesh.triangles) {
        for (int edge = 0; edge < 3; ++edge) {
            const auto first = triangle.vertices[static_cast<std::size_t>(edge)];
            const auto second = triangle.vertices[static_cast<std::size_t>((edge + 1) % 3)];
            if (first < result.size() && second < result.size()) {
                result[first].insert(second);
                result[second].insert(first);
            }
        }
    }
    return result;
}

std::vector<std::vector<std::size_t>> buildIncidentTriangles(const RegionReferenceMesh& mesh) {
    std::vector<std::vector<std::size_t>> result(mesh.vertices.size());
    for (std::size_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
        for (const auto vertex : mesh.triangles[triangleIndex].vertices) {
            if (vertex < result.size()) result[vertex].push_back(triangleIndex);
        }
    }
    return result;
}

double triangleArea(const RegionReferenceMesh& mesh, const RegionReferenceTriangle& triangle) {
    const auto& a = mesh.vertices[triangle.vertices[0]].point;
    const auto& b = mesh.vertices[triangle.vertices[1]].point;
    const auto& c = mesh.vertices[triangle.vertices[2]].point;
    return gp_Vec(a, b).Crossed(gp_Vec(a, c)).Magnitude() * 0.5;
}

double angleDegrees(const gp_Vec& first, const gp_Vec& second) {
    if (first.SquareMagnitude() <= 1.0e-24 || second.SquareMagnitude() <= 1.0e-24) return 0.0;
    const auto cosine = std::clamp(first.Dot(second) /
        std::sqrt(first.SquareMagnitude() * second.SquareMagnitude()), -1.0, 1.0);
    return std::acos(cosine) * 180.0 / kPi;
}

double minimumAngleDegrees(const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& c) {
    return std::min({
        angleDegrees(gp_Vec(a, b), gp_Vec(a, c)),
        angleDegrees(gp_Vec(b, a), gp_Vec(b, c)),
        angleDegrees(gp_Vec(c, a), gp_Vec(c, b))});
}

double percentile(std::vector<double> values, const double fraction) {
    if (values.empty()) return 0.0;
    const auto index = std::min(
        values.size() - 1,
        static_cast<std::size_t>(std::floor(fraction * static_cast<double>(values.size() - 1))));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

RegionMeshQualityReport measureQualityImpl(const RegionReferenceMesh& mesh) {
    RegionMeshQualityReport report;
    const auto edges = buildEdgeTriangles(mesh);
    std::vector<double> lengths;
    lengths.reserve(edges.size());
    for (const auto& [edge, triangles] : edges) {
        if (triangles.size() > 2) ++report.nonManifoldEdgeCount;
        lengths.push_back(mesh.vertices[edge.first].point.Distance(mesh.vertices[edge.second].point));
    }
    std::vector<double> minimumAngles;
    std::vector<double> aspects;
    std::vector<double> qualities;
    minimumAngles.reserve(mesh.triangles.size());
    aspects.reserve(mesh.triangles.size());
    qualities.reserve(mesh.triangles.size());
    for (const auto& triangle : mesh.triangles) {
        const auto& a = mesh.vertices[triangle.vertices[0]].point;
        const auto& b = mesh.vertices[triangle.vertices[1]].point;
        const auto& c = mesh.vertices[triangle.vertices[2]].point;
        const auto ab2 = a.SquareDistance(b);
        const auto bc2 = b.SquareDistance(c);
        const auto ca2 = c.SquareDistance(a);
        const auto area = triangleArea(mesh, triangle);
        if (area <= 1.0e-20) {
            ++report.zeroAreaTriangleCount;
            continue;
        }
        minimumAngles.push_back(minimumAngleDegrees(a, b, c));
        aspects.push_back(std::max({ab2, bc2, ca2}) / (2.0 * area));
        qualities.push_back(4.0 * std::sqrt(3.0) * area / (ab2 + bc2 + ca2));
    }
    report.edgeLengthP01 = percentile(lengths, 0.01);
    report.edgeLengthP50 = percentile(lengths, 0.50);
    report.edgeLengthP99 = percentile(lengths, 0.99);
    report.edgeLengthP99P01Ratio = report.edgeLengthP01 > 0.0
        ? report.edgeLengthP99 / report.edgeLengthP01
        : std::numeric_limits<double>::infinity();
    report.minimumAngleP05Degrees = percentile(minimumAngles, 0.05);
    report.minimumAngleP50Degrees = percentile(minimumAngles, 0.50);
    report.aspectRatioP50 = percentile(aspects, 0.50);
    report.aspectRatioP95 = percentile(aspects, 0.95);
    report.aspectRatioP99 = percentile(aspects, 0.99);
    report.normalizedQualityP05 = percentile(qualities, 0.05);
    report.normalizedQualityP50 = percentile(qualities, 0.50);
    return report;
}

bool lockedVertex(const RegionReferenceVertex& vertex) {
    return vertex.boundary || vertex.hardBoundary || vertex.hardCorner || vertex.normalDiscontinuity;
}

bool constrainedEdge(const RegionReferenceMesh& mesh, const MeshEdge& edge) {
    const auto& first = mesh.vertices[edge.first];
    const auto& second = mesh.vertices[edge.second];
    if (first.normalDiscontinuity || second.normalDiscontinuity) return true;
    if (!first.boundary || !second.boundary) return false;
    for (const auto firstEdge : first.boundaryEdgeIds) {
        if (std::find(second.boundaryEdgeIds.begin(), second.boundaryEdgeIds.end(), firstEdge) !=
            second.boundaryEdgeIds.end()) {
            return true;
        }
    }
    return first.hardBoundary || second.hardBoundary;
}

const RegionReferenceSourceSample* sharedSource(
    const RegionReferenceVertex& first,
    const RegionReferenceVertex& second) {
    for (const auto& source : first.sources) {
        const auto found = std::find_if(second.sources.begin(), second.sources.end(), [&](const auto& candidate) {
            return candidate.faceId == source.faceId;
        });
        if (found != second.sources.end()) return &source;
    }
    return nullptr;
}

std::optional<RegionReferenceSourceSample> evaluateSource(
    const ShapeDocument& document,
    const FaceId faceId,
    const gp_Pnt2d& uv) {
    try {
        const auto& face = document.topology().face(faceId);
        const auto surface = BRep_Tool::Surface(face);
        if (surface.IsNull()) return std::nullopt;
        GeomLProp_SLProps properties(surface, uv.X(), uv.Y(), 2, 1.0e-9);
        RegionReferenceSourceSample result;
        result.faceId = faceId;
        result.uv = uv;
        if (properties.IsNormalDefined()) {
            result.normal = properties.Normal();
            if (face.Orientation() == TopAbs_REVERSED) result.normal.Reverse();
            result.normalDefined = true;
        }
        if (properties.IsCurvatureDefined()) {
            result.minimumCurvature = properties.MinCurvature();
            result.maximumCurvature = properties.MaxCurvature();
            result.curvatureDefined = true;
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<RegionReferenceVertex> midpointVertex(
    const ShapeDocument& document,
    const RegionReferenceVertex& first,
    const RegionReferenceVertex& second) {
    RegionReferenceVertex vertex;
    gp_XYZ pointSum;
    gp_XYZ normalSum;
    for (const auto& firstSource : first.sources) {
        const auto secondSource = std::find_if(
            second.sources.begin(),
            second.sources.end(),
            [&](const auto& source) { return source.faceId == firstSource.faceId; });
        if (secondSource == second.sources.end()) continue;
        const gp_Pnt2d uv(
            (firstSource.uv.X() + secondSource->uv.X()) * 0.5,
            (firstSource.uv.Y() + secondSource->uv.Y()) * 0.5);
        const auto source = evaluateSource(document, firstSource.faceId, uv);
        if (!source.has_value()) continue;
        const auto surface = BRep_Tool::Surface(document.topology().face(source->faceId));
        if (surface.IsNull()) continue;
        pointSum += surface->Value(uv.X(), uv.Y()).XYZ();
        if (source->normalDefined) normalSum += source->normal.XYZ();
        vertex.sources.push_back(*source);
    }
    if (vertex.sources.empty()) return std::nullopt;
    pointSum /= static_cast<double>(vertex.sources.size());
    vertex.point = gp_Pnt(pointSum);
    if (normalSum.SquareModulus() > 1.0e-24) {
        vertex.normal = gp_Dir(normalSum);
    } else {
        vertex.normal = vertex.sources.front().normal;
    }
    vertex.internalSeam = first.internalSeam && second.internalSeam;
    return vertex;
}

std::optional<RegionReferenceVertex> projectedVertex(
    const ShapeDocument& document,
    const RegionReferenceVertex& original,
    const gp_Pnt& target) {
    if (original.sources.size() != 1) return std::nullopt;
    const auto faceId = original.sources.front().faceId;
    try {
        const auto& face = document.topology().face(faceId);
        const auto surface = BRep_Tool::Surface(face);
        if (surface.IsNull()) return std::nullopt;
        double uMin = 0.0;
        double uMax = 0.0;
        double vMin = 0.0;
        double vMax = 0.0;
        BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
        GeomAPI_ProjectPointOnSurf projection(target, surface, uMin, uMax, vMin, vMax, 1.0e-9);
        if (projection.NbPoints() <= 0) return std::nullopt;
        double u = 0.0;
        double v = 0.0;
        projection.LowerDistanceParameters(u, v);
        BRepClass_FaceClassifier classifier(face, gp_Pnt2d(u, v), 1.0e-8);
        if (classifier.State() == TopAbs_OUT) return std::nullopt;
        const auto source = evaluateSource(document, faceId, gp_Pnt2d(u, v));
        if (!source.has_value()) return std::nullopt;
        auto result = original;
        result.point = surface->Value(u, v);
        result.normal = source->normal;
        result.sources = {*source};
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<double> targetLengths(
    const RegionReferenceMesh& mesh,
    const EdgeTriangles& edges,
    const double globalTarget,
    const double chordTolerance,
    const double normalToleranceRadians,
    const double maximumRatio) {
    std::vector<double> result(mesh.vertices.size(), globalTarget);
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
        double curvature = 0.0;
        for (const auto& source : mesh.vertices[index].sources) {
            if (!source.curvatureDefined) continue;
            curvature = std::max(curvature, std::max(
                std::abs(source.minimumCurvature), std::abs(source.maximumCurvature)));
        }
        if (curvature > 1.0e-12) {
            result[index] = std::min({
                result[index],
                std::sqrt(8.0 * chordTolerance / curvature),
                normalToleranceRadians / curvature});
        }
        result[index] = std::max(result[index], globalTarget * 0.1);
    }
    for (const auto& [edge, triangles] : edges) {
        (void)triangles;
        if (!constrainedEdge(mesh, edge)) continue;
        const auto length = mesh.vertices[edge.first].point.Distance(mesh.vertices[edge.second].point);
        const auto boundaryTarget = std::max(length, globalTarget / 3.0);
        result[edge.first] = std::min(result[edge.first], boundaryTarget);
        result[edge.second] = std::min(result[edge.second], boundaryTarget);
    }
    for (int iteration = 0; iteration < 8; ++iteration) {
        bool changed = false;
        for (const auto& [edge, triangles] : edges) {
            (void)triangles;
            const auto firstLimit = result[edge.second] * maximumRatio;
            const auto secondLimit = result[edge.first] * maximumRatio;
            if (result[edge.first] > firstLimit) {
                result[edge.first] = firstLimit;
                changed = true;
            }
            if (result[edge.second] > secondLimit) {
                result[edge.second] = secondLimit;
                changed = true;
            }
        }
        if (!changed) break;
    }
    return result;
}

std::size_t splitLongEdges(
    const ShapeDocument& document,
    const std::vector<double>& targets,
    RegionReferenceMesh& mesh,
    const std::chrono::steady_clock::time_point deadline,
    bool& canceled) {
    const auto edges = buildEdgeTriangles(mesh);
    struct Candidate { double ratio = 0.0; MeshEdge edge; };
    std::vector<Candidate> candidates;
    for (const auto& [edge, triangles] : edges) {
        if (triangles.empty() || triangles.size() > 2 || constrainedEdge(mesh, edge)) continue;
        const auto target = (targets[edge.first] + targets[edge.second]) * 0.5;
        const auto length = mesh.vertices[edge.first].point.Distance(mesh.vertices[edge.second].point);
        if (target > 0.0 && length > target * 4.0 / 3.0 &&
            sharedSource(mesh.vertices[edge.first], mesh.vertices[edge.second]) != nullptr) {
            candidates.push_back({length / target, edge});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.ratio != rhs.ratio) return lhs.ratio > rhs.ratio;
        return lhs.edge < rhs.edge;
    });
    std::set<std::size_t> used;
    std::map<MeshEdge, std::size_t> midpointByEdge;
    std::size_t candidateIndex = 0;
    for (const auto& candidate : candidates) {
        if ((candidateIndex++ & 0xffu) == 0u && std::chrono::steady_clock::now() >= deadline) {
            canceled = true;
            break;
        }
        if (used.count(candidate.edge.first) || used.count(candidate.edge.second)) continue;
        const auto vertex = midpointVertex(
            document, mesh.vertices[candidate.edge.first], mesh.vertices[candidate.edge.second]);
        if (!vertex.has_value()) continue;
        const auto index = mesh.vertices.size();
        mesh.vertices.push_back(*vertex);
        midpointByEdge.emplace(candidate.edge, index);
        used.insert(candidate.edge.first);
        used.insert(candidate.edge.second);
    }
    if (midpointByEdge.empty()) return 0;
    std::vector<RegionReferenceTriangle> rebuilt;
    rebuilt.reserve(mesh.triangles.size() + midpointByEdge.size() * 2);
    for (const auto& triangle : mesh.triangles) {
        const auto a = triangle.vertices[0];
        const auto b = triangle.vertices[1];
        const auto c = triangle.vertices[2];
        const auto ab = midpointByEdge.find(MeshEdge(a, b));
        const auto bc = midpointByEdge.find(MeshEdge(b, c));
        const auto ca = midpointByEdge.find(MeshEdge(c, a));
        if (ab != midpointByEdge.end()) {
            rebuilt.push_back({{a, ab->second, c}, triangle.faceId});
            rebuilt.push_back({{ab->second, b, c}, triangle.faceId});
        } else if (bc != midpointByEdge.end()) {
            rebuilt.push_back({{b, bc->second, a}, triangle.faceId});
            rebuilt.push_back({{bc->second, c, a}, triangle.faceId});
        } else if (ca != midpointByEdge.end()) {
            rebuilt.push_back({{c, ca->second, b}, triangle.faceId});
            rebuilt.push_back({{ca->second, a, b}, triangle.faceId});
        } else {
            rebuilt.push_back(triangle);
        }
    }
    mesh.triangles = std::move(rebuilt);
    return midpointByEdge.size();
}

bool collapsePreservesOrientation(
    const RegionReferenceMesh& mesh,
    const MeshEdge& edge,
    const gp_Pnt& replacement,
    const std::vector<std::vector<std::size_t>>& incidentTriangles) {
    std::vector<std::size_t> localTriangles = incidentTriangles[edge.first];
    localTriangles.insert(
        localTriangles.end(),
        incidentTriangles[edge.second].begin(),
        incidentTriangles[edge.second].end());
    std::sort(localTriangles.begin(), localTriangles.end());
    localTriangles.erase(std::unique(localTriangles.begin(), localTriangles.end()), localTriangles.end());
    for (const auto triangleIndex : localTriangles) {
        const auto& triangle = mesh.triangles[triangleIndex];
        const bool containsFirst = std::find(triangle.vertices.begin(), triangle.vertices.end(), edge.first) !=
            triangle.vertices.end();
        const bool containsSecond = std::find(triangle.vertices.begin(), triangle.vertices.end(), edge.second) !=
            triangle.vertices.end();
        if (!containsFirst && !containsSecond) continue;
        if (containsFirst && containsSecond) continue;
        std::array<gp_Pnt, 3> before;
        std::array<gp_Pnt, 3> after;
        for (int corner = 0; corner < 3; ++corner) {
            const auto index = triangle.vertices[static_cast<std::size_t>(corner)];
            before[static_cast<std::size_t>(corner)] = mesh.vertices[index].point;
            after[static_cast<std::size_t>(corner)] = index == edge.first || index == edge.second
                ? replacement : before[static_cast<std::size_t>(corner)];
        }
        const auto oldNormal = gp_Vec(before[0], before[1]).Crossed(gp_Vec(before[0], before[2]));
        const auto newNormal = gp_Vec(after[0], after[1]).Crossed(gp_Vec(after[0], after[2]));
        if (newNormal.SquareMagnitude() <= 1.0e-24 || oldNormal.Dot(newNormal) <= 0.0) return false;
    }
    return true;
}

void compactMesh(RegionReferenceMesh& mesh) {
    std::vector<bool> used(mesh.vertices.size(), false);
    for (const auto& triangle : mesh.triangles) {
        for (const auto vertex : triangle.vertices) if (vertex < used.size()) used[vertex] = true;
    }
    for (const auto& sample : mesh.boundarySamples) if (sample.vertexIndex < used.size()) used[sample.vertexIndex] = true;
    std::vector<std::size_t> mapping(mesh.vertices.size(), std::numeric_limits<std::size_t>::max());
    std::vector<RegionReferenceVertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
        if (!used[index]) continue;
        mapping[index] = vertices.size();
        vertices.push_back(std::move(mesh.vertices[index]));
    }
    for (auto& triangle : mesh.triangles) {
        for (auto& vertex : triangle.vertices) vertex = mapping[vertex];
    }
    for (auto& sample : mesh.boundarySamples) sample.vertexIndex = mapping[sample.vertexIndex];
    mesh.vertices = std::move(vertices);
}

std::size_t collapseShortEdges(
    const ShapeDocument& document,
    const std::vector<double>& targets,
    RegionReferenceMesh& mesh,
    const std::chrono::steady_clock::time_point deadline,
    bool& canceled) {
    const auto edges = buildEdgeTriangles(mesh);
    const auto neighbors = buildNeighbors(mesh);
    const auto incidentTriangles = buildIncidentTriangles(mesh);
    struct Candidate { double ratio = 0.0; MeshEdge edge; };
    std::vector<Candidate> candidates;
    for (const auto& [edge, triangles] : edges) {
        if (triangles.size() != 2 || constrainedEdge(mesh, edge) ||
            lockedVertex(mesh.vertices[edge.first]) || lockedVertex(mesh.vertices[edge.second])) {
            continue;
        }
        std::vector<std::size_t> common;
        std::set_intersection(
            neighbors[edge.first].begin(), neighbors[edge.first].end(),
            neighbors[edge.second].begin(), neighbors[edge.second].end(),
            std::back_inserter(common));
        if (common.size() != 2 || sharedSource(mesh.vertices[edge.first], mesh.vertices[edge.second]) == nullptr) continue;
        const auto target = (targets[edge.first] + targets[edge.second]) * 0.5;
        const auto length = mesh.vertices[edge.first].point.Distance(mesh.vertices[edge.second].point);
        if (target > 0.0 && length < target * 0.8) candidates.push_back({length / target, edge});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.ratio != rhs.ratio) return lhs.ratio < rhs.ratio;
        return lhs.edge < rhs.edge;
    });
    std::set<std::size_t> used;
    std::map<std::size_t, std::size_t> replacement;
    std::map<std::size_t, RegionReferenceVertex> moved;
    std::size_t candidateIndex = 0;
    for (const auto& candidate : candidates) {
        if ((candidateIndex++ & 0xffu) == 0u && std::chrono::steady_clock::now() >= deadline) {
            canceled = true;
            break;
        }
        const auto keep = candidate.edge.first;
        const auto remove = candidate.edge.second;
        if (used.count(keep) || used.count(remove)) continue;
        const auto midpoint = midpointVertex(document, mesh.vertices[keep], mesh.vertices[remove]);
        if (!midpoint.has_value() ||
            !collapsePreservesOrientation(mesh, candidate.edge, midpoint->point, incidentTriangles)) continue;
        replacement.emplace(remove, keep);
        moved.emplace(keep, *midpoint);
        used.insert(keep);
        used.insert(remove);
        used.insert(neighbors[keep].begin(), neighbors[keep].end());
        used.insert(neighbors[remove].begin(), neighbors[remove].end());
    }
    if (replacement.empty()) return 0;
    for (const auto& [index, vertex] : moved) mesh.vertices[index] = vertex;
    std::vector<RegionReferenceTriangle> triangles;
    triangles.reserve(mesh.triangles.size());
    std::set<std::array<std::size_t, 3>> unique;
    for (auto triangle : mesh.triangles) {
        for (auto& vertex : triangle.vertices) {
            const auto found = replacement.find(vertex);
            if (found != replacement.end()) vertex = found->second;
        }
        if (triangle.vertices[0] == triangle.vertices[1] || triangle.vertices[1] == triangle.vertices[2] ||
            triangle.vertices[2] == triangle.vertices[0]) continue;
        auto key = triangle.vertices;
        std::sort(key.begin(), key.end());
        if (!unique.insert(key).second) continue;
        triangles.push_back(triangle);
    }
    mesh.triangles = std::move(triangles);
    compactMesh(mesh);
    return replacement.size();
}

std::size_t oppositeVertex(const RegionReferenceTriangle& triangle, const MeshEdge& edge) {
    for (const auto vertex : triangle.vertices) {
        if (vertex != edge.first && vertex != edge.second) return vertex;
    }
    return std::numeric_limits<std::size_t>::max();
}

RegionReferenceTriangle orientedTriangle(
    const RegionReferenceMesh& mesh,
    const std::array<std::size_t, 3>& vertices,
    const gp_Vec& reference,
    const FaceId faceId) {
    RegionReferenceTriangle result {vertices, faceId};
    const auto normal = gp_Vec(
        mesh.vertices[vertices[0]].point,
        mesh.vertices[vertices[1]].point).Crossed(gp_Vec(
            mesh.vertices[vertices[0]].point,
            mesh.vertices[vertices[2]].point));
    if (normal.Dot(reference) < 0.0) std::swap(result.vertices[1], result.vertices[2]);
    return result;
}

std::size_t flipEdges(
    RegionReferenceMesh& mesh,
    const std::chrono::steady_clock::time_point deadline,
    bool& canceled) {
    const auto edges = buildEdgeTriangles(mesh);
    std::set<std::size_t> usedTriangles;
    struct Flip { std::size_t firstTriangle; std::size_t secondTriangle; RegionReferenceTriangle first; RegionReferenceTriangle second; };
    std::vector<Flip> flips;
    std::size_t edgeIndex = 0;
    for (const auto& [edge, incident] : edges) {
        if ((edgeIndex++ & 0x3ffu) == 0u && std::chrono::steady_clock::now() >= deadline) {
            canceled = true;
            break;
        }
        if (incident.size() != 2 || constrainedEdge(mesh, edge) ||
            usedTriangles.count(incident[0]) || usedTriangles.count(incident[1])) continue;
        const auto& firstTriangle = mesh.triangles[incident[0]];
        const auto& secondTriangle = mesh.triangles[incident[1]];
        if (firstTriangle.faceId != secondTriangle.faceId) continue;
        const auto c = oppositeVertex(firstTriangle, edge);
        const auto d = oppositeVertex(secondTriangle, edge);
        if (c >= mesh.vertices.size() || d >= mesh.vertices.size() || c == d || edges.count(MeshEdge(c, d))) continue;
        const auto& aPoint = mesh.vertices[edge.first].point;
        const auto& bPoint = mesh.vertices[edge.second].point;
        const auto& cPoint = mesh.vertices[c].point;
        const auto& dPoint = mesh.vertices[d].point;
        const auto oldMinimum = std::min(
            minimumAngleDegrees(aPoint, bPoint, cPoint),
            minimumAngleDegrees(bPoint, aPoint, dPoint));
        const auto newMinimum = std::min(
            minimumAngleDegrees(cPoint, dPoint, aPoint),
            minimumAngleDegrees(dPoint, cPoint, bPoint));
        if (newMinimum < oldMinimum + 0.5) continue;
        const auto firstNormal = gp_Vec(aPoint, bPoint).Crossed(gp_Vec(aPoint, cPoint));
        const auto secondNormal = gp_Vec(bPoint, aPoint).Crossed(gp_Vec(bPoint, dPoint));
        auto first = orientedTriangle(mesh, {c, d, edge.first}, firstNormal, firstTriangle.faceId);
        auto second = orientedTriangle(mesh, {d, c, edge.second}, secondNormal, secondTriangle.faceId);
        if (triangleArea(mesh, first) <= 1.0e-20 || triangleArea(mesh, second) <= 1.0e-20) continue;
        flips.push_back({incident[0], incident[1], first, second});
        usedTriangles.insert(incident[0]);
        usedTriangles.insert(incident[1]);
    }
    for (const auto& flip : flips) {
        mesh.triangles[flip.firstTriangle] = flip.first;
        mesh.triangles[flip.secondTriangle] = flip.second;
    }
    return flips.size();
}

bool relocationPreservesOrientation(
    const RegionReferenceMesh& mesh,
    const std::size_t vertexIndex,
    const gp_Pnt& replacement,
    const std::vector<std::size_t>& incidentTriangles) {
    for (const auto triangleIndex : incidentTriangles) {
        const auto& triangle = mesh.triangles[triangleIndex];
        std::array<gp_Pnt, 3> before;
        std::array<gp_Pnt, 3> after;
        for (int corner = 0; corner < 3; ++corner) {
            const auto index = triangle.vertices[static_cast<std::size_t>(corner)];
            before[static_cast<std::size_t>(corner)] = mesh.vertices[index].point;
            after[static_cast<std::size_t>(corner)] = index == vertexIndex ? replacement : before[static_cast<std::size_t>(corner)];
        }
        const auto oldNormal = gp_Vec(before[0], before[1]).Crossed(gp_Vec(before[0], before[2]));
        const auto newNormal = gp_Vec(after[0], after[1]).Crossed(gp_Vec(after[0], after[2]));
        if (newNormal.SquareMagnitude() <= 1.0e-24 || oldNormal.Dot(newNormal) <= 0.0) return false;
    }
    return true;
}

std::size_t relocateVertices(
    const ShapeDocument& document,
    const std::vector<double>& targets,
    RegionReferenceMesh& mesh,
    const std::chrono::steady_clock::time_point deadline,
    bool& canceled) {
    const auto neighbors = buildNeighbors(mesh);
    const auto incidentTriangles = buildIncidentTriangles(mesh);
    const auto original = mesh.vertices;
    std::size_t count = 0;
    for (std::size_t index = 0; index < original.size(); ++index) {
        if ((index & 0x3fu) == 0u && std::chrono::steady_clock::now() >= deadline) {
            canceled = true;
            break;
        }
        const auto& vertex = original[index];
        if (lockedVertex(vertex) || vertex.sources.size() != 1 || neighbors[index].size() < 3) continue;
        gp_Vec average;
        double shortestEdge = std::numeric_limits<double>::infinity();
        double longestEdge = 0.0;
        for (const auto neighbor : neighbors[index]) {
            average += original[neighbor].point.XYZ();
            const auto length = vertex.point.Distance(original[neighbor].point);
            shortestEdge = std::min(shortestEdge, length);
            longestEdge = std::max(longestEdge, length);
        }
        average /= static_cast<double>(neighbors[index].size());
        const gp_Pnt target(
            (vertex.point.X() + average.X()) * 0.5,
            (vertex.point.Y() + average.Y()) * 0.5,
            (vertex.point.Z() + average.Z()) * 0.5);
        const auto localTarget = index < targets.size() ? targets[index] : 0.0;
        if (target.Distance(vertex.point) <= std::max(1.0e-10, localTarget * 0.02)) continue;
        if (shortestEdge > 0.0 && longestEdge / shortestEdge < 1.25 &&
            neighbors[index].size() >= 5 && neighbors[index].size() <= 7) {
            continue;
        }
        const auto projected = projectedVertex(document, vertex, target);
        if (!projected.has_value() ||
            !relocationPreservesOrientation(mesh, index, projected->point, incidentTriangles[index])) continue;
        mesh.vertices[index] = *projected;
        ++count;
    }
    return count;
}

}

RegionMeshQualityReport measureRegionMeshQuality(const RegionReferenceMesh& mesh) {
    return measureQualityImpl(mesh);
}

QualityBalancedRegionRemeshResult QualityBalancedRegionRemesher::remesh(
    const ShapeDocument& document,
    const RegionTopologyConstraints& constraints,
    const RegionRefitOptions& options,
    const RegionReferenceMesh& input,
    const std::chrono::steady_clock::time_point deadline) const {
    (void)constraints;
    QualityBalancedRegionRemeshResult result;
    result.mesh = input;
    if (input.empty() || options.targetSampleTriangles == 0 || options.maximumRemeshSweeps <= 0 ||
        options.maximumLocalSizeRatio <= 1.0) {
        result.report.message = "Quality-balanced remeshing received invalid geometry or options.";
        return result;
    }
    double area = 0.0;
    for (const auto& triangle : input.triangles) area += triangleArea(input, triangle);
    if (area <= 1.0e-20) {
        result.report.message = "Quality-balanced remeshing received a zero-area reference mesh.";
        return result;
    }
    result.report.targetEdgeLength = std::sqrt(
        4.0 * area / (std::sqrt(3.0) * static_cast<double>(options.targetSampleTriangles)));
    const auto chordTolerance = std::min(options.tolerance3d / 4.0, 0.01);
    const auto normalTolerance = std::min(options.normalToleranceDegrees / 3.0, 5.0) * kPi / 180.0;
    for (int sweep = 0; sweep < options.maximumRemeshSweeps; ++sweep) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.report.timeBudgetExceeded = true;
            break;
        }
        const auto beforeEdges = buildEdgeTriangles(result.mesh);
        const auto targets = targetLengths(
            result.mesh,
            beforeEdges,
            result.report.targetEdgeLength,
            chordTolerance,
            normalTolerance,
            options.maximumLocalSizeRatio);
        bool canceled = false;
        const auto split = splitLongEdges(document, targets, result.mesh, deadline, canceled);
        if (result.mesh.triangles.size() > options.maximumSampleTriangles) {
            result.report.message = "Quality-balanced remeshing exceeded the triangle budget.";
            return result;
        }
        if (canceled) {
            result.report.splitCount += split;
            result.report.timeBudgetExceeded = true;
            break;
        }
        const auto afterSplitEdges = buildEdgeTriangles(result.mesh);
        const auto afterSplitTargets = targetLengths(
            result.mesh,
            afterSplitEdges,
            result.report.targetEdgeLength,
            chordTolerance,
            normalTolerance,
            options.maximumLocalSizeRatio);
        const auto collapse = collapseShortEdges(
            document, afterSplitTargets, result.mesh, deadline, canceled);
        const auto intermediateQuality = measureRegionMeshQuality(result.mesh);
        const auto minimumAcceptedTriangles = std::max<std::size_t>(
            1, options.targetSampleTriangles * 4 / 5);
        const auto maximumAcceptedTriangles = options.targetSampleTriangles * 6 / 5;
        const auto targetReached =
            result.mesh.triangles.size() >= minimumAcceptedTriangles &&
            result.mesh.triangles.size() <= maximumAcceptedTriangles &&
            intermediateQuality.zeroAreaTriangleCount == 0 &&
            intermediateQuality.nonManifoldEdgeCount == 0 &&
            intermediateQuality.minimumAngleP05Degrees >= 5.0 &&
            intermediateQuality.aspectRatioP95 <= 20.0;
        if (targetReached) {
            result.report.splitCount += split;
            result.report.collapseCount += collapse;
            break;
        }
        if (canceled) {
            result.report.splitCount += split;
            result.report.collapseCount += collapse;
            result.report.timeBudgetExceeded = true;
            break;
        }
        const auto flip = flipEdges(result.mesh, deadline, canceled);
        if (canceled) {
            result.report.splitCount += split;
            result.report.collapseCount += collapse;
            result.report.flipCount += flip;
            result.report.timeBudgetExceeded = true;
            break;
        }
        const auto afterCollapseEdges = buildEdgeTriangles(result.mesh);
        const auto afterCollapseTargets = targetLengths(
            result.mesh,
            afterCollapseEdges,
            result.report.targetEdgeLength,
            chordTolerance,
            normalTolerance,
            options.maximumLocalSizeRatio);
        const auto relocation = relocateVertices(
            document, afterCollapseTargets, result.mesh, deadline, canceled);
        result.report.splitCount += split;
        result.report.collapseCount += collapse;
        result.report.flipCount += flip;
        result.report.relocationCount += relocation;
        if (canceled) {
            result.report.timeBudgetExceeded = true;
            break;
        }
        const auto operationCount = split + collapse + flip + relocation;
        const auto edgeCount = std::max<std::size_t>(1, beforeEdges.size());
        if (static_cast<double>(operationCount) < static_cast<double>(edgeCount) * 0.005) break;
    }
    result.mesh.updateIntegrationWeights();
    const auto quality = measureRegionMeshQuality(result.mesh);
    const auto targetEdgeLength = result.report.targetEdgeLength;
    const auto splitCount = result.report.splitCount;
    const auto collapseCount = result.report.collapseCount;
    const auto flipCount = result.report.flipCount;
    const auto relocationCount = result.report.relocationCount;
    const auto timeBudgetExceeded = result.report.timeBudgetExceeded;
    result.report = quality;
    result.report.targetEdgeLength = targetEdgeLength;
    result.report.splitCount = splitCount;
    result.report.collapseCount = collapseCount;
    result.report.flipCount = flipCount;
    result.report.relocationCount = relocationCount;
    result.report.timeBudgetExceeded = timeBudgetExceeded;
    result.report.success = !result.report.timeBudgetExceeded && !result.mesh.empty() &&
        result.report.zeroAreaTriangleCount == 0 &&
        result.report.nonManifoldEdgeCount == 0 && result.mesh.triangles.size() <= options.maximumSampleTriangles;
    if (result.report.success) {
        result.report.message = "Quality-balanced STEP remeshing produced a structurally valid reference mesh.";
    } else if (result.report.timeBudgetExceeded) {
        result.report.message = "Quality-balanced STEP remeshing reached the sampling deadline.";
    } else {
        std::ostringstream message;
        message << "Quality-balanced STEP remeshing produced a structurally invalid reference mesh: vertices="
                << result.mesh.vertices.size() << ", triangles=" << result.mesh.triangles.size()
                << ", zero_area_triangles=" << result.report.zeroAreaTriangleCount
                << ", non_manifold_edges=" << result.report.nonManifoldEdgeCount
                << ", triangle_budget=" << options.maximumSampleTriangles << '.';
        result.report.message = message.str();
    }
    return result;
}

}
