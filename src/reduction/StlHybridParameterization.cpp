#include "reduction/StlHybridParameterization.h"

#include "stl/StlCutChainCutter.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopLoc_Location.hxx>

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <utility>

namespace spo {

struct StlHybridParameterization::Triangle {
    std::array<std::size_t, 3> vertices {};
};

namespace {

using Edge = std::pair<std::size_t, std::size_t>;

Edge orderedEdge(std::size_t first, std::size_t second) {
    return first < second ? Edge {first, second} : Edge {second, first};
}

std::array<double, 2> circularBoundaryParameter(double fraction) {
    constexpr double pi = 3.14159265358979323846;
    const double angle = 2.0 * pi * (fraction - std::floor(fraction));
    return {0.5 + 0.5 * std::cos(angle), 0.5 + 0.5 * std::sin(angle)};
}

double squaredDistance(const gp_Pnt& first, const gp_Pnt& second) {
    return first.SquareDistance(second);
}

struct ClosestPoint {
    bool valid = false;
    std::array<double, 3> barycentric {};
    gp_Pnt point;
    double squaredDistance = std::numeric_limits<double>::infinity();
};

ClosestPoint closestPointOnTriangle(
    const gp_Pnt& point,
    const gp_Pnt& first,
    const gp_Pnt& second,
    const gp_Pnt& third) {
    ClosestPoint result;
    const gp_Vec firstSecond(first, second);
    const gp_Vec firstThird(first, third);
    const gp_Vec firstPoint(first, point);
    const double d1 = firstSecond.Dot(firstPoint);
    const double d2 = firstThird.Dot(firstPoint);
    if (d1 <= 0.0 && d2 <= 0.0) {
        result.valid = true;
        result.barycentric = {1.0, 0.0, 0.0};
        result.point = first;
        result.squaredDistance = squaredDistance(point, first);
        return result;
    }

    const gp_Vec secondPoint(second, point);
    const double d3 = firstSecond.Dot(secondPoint);
    const double d4 = firstThird.Dot(secondPoint);
    if (d3 >= 0.0 && d4 <= d3) {
        result.valid = true;
        result.barycentric = {0.0, 1.0, 0.0};
        result.point = second;
        result.squaredDistance = squaredDistance(point, second);
        return result;
    }

    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double parameter = d1 / (d1 - d3);
        result.valid = true;
        result.barycentric = {1.0 - parameter, parameter, 0.0};
        result.point = first.Translated(firstSecond * parameter);
        result.squaredDistance = squaredDistance(point, result.point);
        return result;
    }

    const gp_Vec thirdPoint(third, point);
    const double d5 = firstSecond.Dot(thirdPoint);
    const double d6 = firstThird.Dot(thirdPoint);
    if (d6 >= 0.0 && d5 <= d6) {
        result.valid = true;
        result.barycentric = {0.0, 0.0, 1.0};
        result.point = third;
        result.squaredDistance = squaredDistance(point, third);
        return result;
    }

    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double parameter = d2 / (d2 - d6);
        result.valid = true;
        result.barycentric = {1.0 - parameter, 0.0, parameter};
        result.point = first.Translated(firstThird * parameter);
        result.squaredDistance = squaredDistance(point, result.point);
        return result;
    }

    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) {
        const double parameter = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        const gp_Vec secondThird(second, third);
        result.valid = true;
        result.barycentric = {0.0, 1.0 - parameter, parameter};
        result.point = second.Translated(secondThird * parameter);
        result.squaredDistance = squaredDistance(point, result.point);
        return result;
    }

    const double denominator = va + vb + vc;
    if (std::abs(denominator) <= Precision::Confusion()) {
        return result;
    }
    const double inverse = 1.0 / denominator;
    const double secondWeight = vb * inverse;
    const double thirdWeight = vc * inverse;
    result.valid = true;
    result.barycentric = {1.0 - secondWeight - thirdWeight, secondWeight, thirdWeight};
    result.point = first.Translated(firstSecond * secondWeight + firstThird * thirdWeight);
    result.squaredDistance = squaredDistance(point, result.point);
    return result;
}

double normalizedJacobian(
    const std::array<double, 2>& first,
    const std::array<double, 2>& second,
    const std::array<double, 2>& third) {
    const double signedDoubleArea =
        (second[0] - first[0]) * (third[1] - first[1]) -
        (second[1] - first[1]) * (third[0] - first[0]);
    const auto squaredLength = [](const auto& a, const auto& b) {
        const double du = a[0] - b[0];
        const double dv = a[1] - b[1];
        return du * du + dv * dv;
    };
    const double denominator = squaredLength(first, second) +
        squaredLength(second, third) + squaredLength(third, first);
    return denominator > 1.0e-20
        ? 2.0 * std::sqrt(3.0) * signedDoubleArea / denominator
        : 0.0;
}

}

const char* toString(SurfaceParameterizationSource source) {
    switch (source) {
    case SurfaceParameterizationSource::None:
        return "None";
    case SurfaceParameterizationSource::StepHarmonic:
        return "StepHarmonic";
    case SurfaceParameterizationSource::SupportSurfaceProjection:
        return "SupportSurfaceProjection";
    }
    return "None";
}

const char* toString(ScanReferenceCoverage coverage) {
    switch (coverage) {
    case ScanReferenceCoverage::None:
        return "None";
    case ScanReferenceCoverage::WholeModel:
        return "WholeModel";
    case ScanReferenceCoverage::SelectionCrop:
        return "SelectionCrop";
    }
    return "None";
}

std::shared_ptr<const StlHybridParameterization> StlHybridParameterization::buildFromStepSelection(
    const ShapeDocument& document,
    const std::vector<FaceId>& selectedFaces,
    double tolerance3d,
    StlHybridParameterizationReport& report) {
    report = {};
    if (!document.hasShape() || selectedFaces.size() < 2) {
        report.message = "STEP 参数图需要至少两个有效原始面。";
        return {};
    }
    if (std::any_of(selectedFaces.begin(), selectedFaces.end(), [&](FaceId faceId) {
            return faceId >= document.topology().faceCount();
        })) {
        report.message = "STEP 参数图引用了不存在的面。";
        return {};
    }

    StlMesh stepMesh;
    try {
        const double deflection = std::max(tolerance3d * 2.0, 0.01);
        for (const auto faceId : selectedFaces) {
            const auto& face = document.topology().face(faceId);
            BRepMesh_IncrementalMesh mesh(
                face,
                deflection,
                Standard_False,
                0.2,
                Standard_True);
            TopLoc_Location location;
            const auto triangulation = BRep_Tool::Triangulation(face, location);
            if (triangulation.IsNull()) {
                continue;
            }
            for (int triangleIndex = 1; triangleIndex <= triangulation->NbTriangles(); ++triangleIndex) {
                int first = 0;
                int second = 0;
                int third = 0;
                triangulation->Triangle(triangleIndex).Get(first, second, third);
                if (face.Orientation() == TopAbs_REVERSED) {
                    std::swap(second, third);
                }
                const gp_Pnt points[3] = {
                    triangulation->Node(first).Transformed(location.Transformation()),
                    triangulation->Node(second).Transformed(location.Transformation()),
                    triangulation->Node(third).Transformed(location.Transformation())};
                gp_Vec normal(points[0], points[1]);
                normal.Cross(gp_Vec(points[0], points[2]));
                if (normal.SquareMagnitude() <= Precision::SquareConfusion()) {
                    continue;
                }
                normal.Normalize();
                stepMesh.addTriangle({
                    {normal.X(), normal.Y(), normal.Z()},
                    {points[0].X(), points[0].Y(), points[0].Z()},
                    {points[1].X(), points[1].Y(), points[1].Z()},
                    {points[2].X(), points[2].Y(), points[2].Z()}});
            }
        }
    } catch (...) {
        report.message = "从 STEP 选区建立参数三角网格失败。";
        return {};
    }
    if (stepMesh.empty()) {
        report.message = "STEP 选区没有可用于参数化的三角网格。";
        return {};
    }

    report.sourceTriangleCount = static_cast<int>(stepMesh.triangleCount());
    report.coarseTriangleCount = report.sourceTriangleCount;
    auto parameterization = buildFromPatchMesh(stepMesh, report);
    if (parameterization) {
        report.source = SurfaceParameterizationSource::StepHarmonic;
        report.message = "STEP 选区单图调和参数化已建立。";
    }
    return parameterization;
}

std::shared_ptr<const StlHybridParameterization> StlHybridParameterization::buildFromPatchMesh(
    const StlMesh& patchMesh,
    StlHybridParameterizationReport& report) {
    const int sourceCount = report.sourceTriangleCount;
    const int coarseCount = report.coarseTriangleCount;
    report = {};
    report.sourceTriangleCount = sourceCount;
    report.coarseTriangleCount = coarseCount;
    report.croppedTriangleCount = static_cast<int>(patchMesh.triangleCount());
    if (patchMesh.empty()) {
        report.message = "裁剪 STL 为空。";
        return {};
    }

    const auto topology = StlCutChainCutter::buildTopology(patchMesh);
    report.vertexCount = static_cast<int>(topology.vertices.size());
    if (topology.vertices.size() < 3 || topology.faces.empty()) {
        report.message = "裁剪 STL 没有可参数化的三角网格。";
        return {};
    }

    std::vector<std::set<std::size_t>> neighbors(topology.vertices.size());
    std::map<Edge, int> edgeUseCount;
    for (const auto& face : topology.faces) {
        std::array<std::size_t, 3> vertices {
            static_cast<std::size_t>(face[0]),
            static_cast<std::size_t>(face[1]),
            static_cast<std::size_t>(face[2])};
        if (std::any_of(vertices.begin(), vertices.end(), [&](std::size_t value) {
                return value >= topology.vertices.size();
            })) {
            report.message = "裁剪 STL 包含无效三角形索引。";
            return {};
        }
        for (int edge = 0; edge < 3; ++edge) {
            const auto first = vertices[static_cast<std::size_t>(edge)];
            const auto second = vertices[static_cast<std::size_t>((edge + 1) % 3)];
            if (first == second) {
                report.message = "裁剪 STL 包含退化拓扑边。";
                return {};
            }
            neighbors[first].insert(second);
            neighbors[second].insert(first);
            ++edgeUseCount[orderedEdge(first, second)];
        }
    }

    std::map<std::size_t, std::vector<std::size_t>> boundaryNeighbors;
    for (const auto& [edge, count] : edgeUseCount) {
        if (count > 2) {
            report.message = "裁剪 STL 包含非流形边。";
            return {};
        }
        if (count == 1) {
            boundaryNeighbors[edge.first].push_back(edge.second);
            boundaryNeighbors[edge.second].push_back(edge.first);
        }
    }
    if (boundaryNeighbors.empty() || std::any_of(
            boundaryNeighbors.begin(), boundaryNeighbors.end(),
            [](const auto& value) { return value.second.size() != 2; })) {
        report.message = "裁剪 STL 边界不是一个闭合二流形环。";
        return {};
    }

    std::vector<std::size_t> boundaryLoop;
    boundaryLoop.reserve(boundaryNeighbors.size());
    const std::size_t start = boundaryNeighbors.begin()->first;
    std::size_t previous = std::numeric_limits<std::size_t>::max();
    std::size_t current = start;
    do {
        boundaryLoop.push_back(current);
        const auto& adjacent = boundaryNeighbors[current];
        const std::size_t next = adjacent[0] == previous ? adjacent[1] : adjacent[0];
        previous = current;
        current = next;
        if (boundaryLoop.size() > boundaryNeighbors.size()) {
            report.message = "裁剪 STL 边界环遍历失败。";
            return {};
        }
    } while (current != start);
    if (boundaryLoop.size() != boundaryNeighbors.size()) {
        report.message = "裁剪 STL 包含多个边界环。";
        return {};
    }
    report.boundaryVertexCount = static_cast<int>(boundaryLoop.size());

    auto result = std::shared_ptr<StlHybridParameterization>(new StlHybridParameterization());
    result->vertices_.reserve(topology.vertices.size());
    for (const auto& vertex : topology.vertices) {
        result->vertices_.emplace_back(vertex[0], vertex[1], vertex[2]);
    }
    result->parameters_.resize(topology.vertices.size());
    result->triangles_.reserve(topology.faces.size());
    for (const auto& face : topology.faces) {
        result->triangles_.push_back({{
            static_cast<std::size_t>(face[0]),
            static_cast<std::size_t>(face[1]),
            static_cast<std::size_t>(face[2])}});
    }

    std::vector<double> cumulative(boundaryLoop.size() + 1, 0.0);
    for (std::size_t index = 0; index < boundaryLoop.size(); ++index) {
        const auto first = boundaryLoop[index];
        const auto second = boundaryLoop[(index + 1) % boundaryLoop.size()];
        cumulative[index + 1] = cumulative[index] +
            result->vertices_[first].Distance(result->vertices_[second]);
    }
    if (cumulative.back() <= Precision::Confusion()) {
        report.message = "裁剪 STL 边界长度退化。";
        return {};
    }
    std::set<std::size_t> boundarySet(boundaryLoop.begin(), boundaryLoop.end());
    for (std::size_t index = 0; index < boundaryLoop.size(); ++index) {
        result->parameters_[boundaryLoop[index]] = circularBoundaryParameter(
            cumulative[index] / cumulative.back());
    }

    std::vector<std::size_t> interior;
    std::vector<int> interiorIndex(topology.vertices.size(), -1);
    for (std::size_t vertex = 0; vertex < topology.vertices.size(); ++vertex) {
        if (!boundarySet.contains(vertex)) {
            interiorIndex[vertex] = static_cast<int>(interior.size());
            interior.push_back(vertex);
        }
    }
    if (!interior.empty()) {
        std::vector<Eigen::Triplet<double>> coefficients;
        Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(static_cast<int>(interior.size()), 2);
        for (std::size_t rowIndex = 0; rowIndex < interior.size(); ++rowIndex) {
            const auto vertex = interior[rowIndex];
            const double degree = static_cast<double>(neighbors[vertex].size());
            if (degree < 1.0) {
                report.message = "裁剪 STL 包含孤立内部顶点。";
                return {};
            }
            coefficients.emplace_back(
                static_cast<int>(rowIndex), static_cast<int>(rowIndex), degree);
            for (const auto neighbor : neighbors[vertex]) {
                if (boundarySet.contains(neighbor)) {
                    rhs(static_cast<int>(rowIndex), 0) += result->parameters_[neighbor][0];
                    rhs(static_cast<int>(rowIndex), 1) += result->parameters_[neighbor][1];
                } else {
                    coefficients.emplace_back(
                        static_cast<int>(rowIndex), interiorIndex[neighbor], -1.0);
                }
            }
        }
        Eigen::SparseMatrix<double> matrix(
            static_cast<int>(interior.size()), static_cast<int>(interior.size()));
        matrix.setFromTriplets(coefficients.begin(), coefficients.end());
        Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
        solver.compute(matrix);
        if (solver.info() != Eigen::Success) {
            report.message = "裁剪 STL 调和参数化矩阵分解失败。";
            return {};
        }
        const Eigen::MatrixXd solution = solver.solve(rhs);
        if (solver.info() != Eigen::Success || !solution.allFinite()) {
            report.message = "裁剪 STL 调和参数化求解失败。";
            return {};
        }
        for (std::size_t index = 0; index < interior.size(); ++index) {
            result->parameters_[interior[index]] = {
                solution(static_cast<int>(index), 0),
                solution(static_cast<int>(index), 1)};
        }
    }

    struct FaceEdgeUse {
        std::size_t face = 0;
        int direction = 0;
    };
    std::map<Edge, std::vector<FaceEdgeUse>> faceEdgeUses;
    for (std::size_t faceIndex = 0; faceIndex < result->triangles_.size(); ++faceIndex) {
        const auto& triangle = result->triangles_[faceIndex];
        for (int edgeIndex = 0; edgeIndex < 3; ++edgeIndex) {
            const auto first = triangle.vertices[static_cast<std::size_t>(edgeIndex)];
            const auto second = triangle.vertices[static_cast<std::size_t>((edgeIndex + 1) % 3)];
            faceEdgeUses[orderedEdge(first, second)].push_back({
                faceIndex,
                first < second ? 1 : -1});
        }
    }
    std::vector<int> faceOrientation(result->triangles_.size(), 0);
    for (std::size_t seed = 0; seed < result->triangles_.size(); ++seed) {
        if (faceOrientation[seed] != 0) {
            continue;
        }
        faceOrientation[seed] = 1;
        std::queue<std::size_t> queue;
        queue.push(seed);
        while (!queue.empty()) {
            const auto faceIndex = queue.front();
            queue.pop();
            const auto& triangle = result->triangles_[faceIndex];
            for (int edgeIndex = 0; edgeIndex < 3; ++edgeIndex) {
                const auto first = triangle.vertices[static_cast<std::size_t>(edgeIndex)];
                const auto second = triangle.vertices[static_cast<std::size_t>((edgeIndex + 1) % 3)];
                const auto& uses = faceEdgeUses[orderedEdge(first, second)];
                if (uses.size() != 2) {
                    continue;
                }
                const auto current = std::find_if(
                    uses.begin(), uses.end(),
                    [&](const auto& use) { return use.face == faceIndex; });
                const auto other = current == uses.begin() ? uses[1] : uses[0];
                const int expected = -current->direction *
                    faceOrientation[faceIndex] * other.direction;
                if (faceOrientation[other.face] == 0) {
                    faceOrientation[other.face] = expected;
                    queue.push(other.face);
                } else if (faceOrientation[other.face] != expected) {
                    report.message = "裁剪 STL 三角网格不可定向。";
                    return {};
                }
            }
        }
    }
    for (std::size_t faceIndex = 0; faceIndex < result->triangles_.size(); ++faceIndex) {
        if (faceOrientation[faceIndex] < 0) {
            std::swap(
                result->triangles_[faceIndex].vertices[1],
                result->triangles_[faceIndex].vertices[2]);
        }
    }

    int positive = 0;
    int negative = 0;
    for (const auto& triangle : result->triangles_) {
        const double jacobian = normalizedJacobian(
            result->parameters_[triangle.vertices[0]],
            result->parameters_[triangle.vertices[1]],
            result->parameters_[triangle.vertices[2]]);
        positive += jacobian > 1.0e-12 ? 1 : 0;
        negative += jacobian < -1.0e-12 ? 1 : 0;
    }
    if (negative > positive) {
        for (auto& parameter : result->parameters_) {
            parameter[1] = 1.0 - parameter[1];
        }
    }

    report.minimumNormalizedJacobian = std::numeric_limits<double>::infinity();
    for (const auto& triangle : result->triangles_) {
        const double jacobian = normalizedJacobian(
            result->parameters_[triangle.vertices[0]],
            result->parameters_[triangle.vertices[1]],
            result->parameters_[triangle.vertices[2]]);
        report.minimumNormalizedJacobian = std::min(
            report.minimumNormalizedJacobian, jacobian);
        if (jacobian <= 1.0e-12) {
            ++report.flippedTriangleCount;
        }
    }
    if (report.flippedTriangleCount > 0) {
        report.message = "裁剪 STL 调和参数化包含翻转或近奇异三角形。";
        return {};
    }

    report.success = true;
    report.message = "裁剪 STL 单图调和参数化已建立。";
    return result;
}

StlHybridProjection StlHybridParameterization::project(const gp_Pnt& point) const {
    StlHybridProjection result;
    double bestSquaredDistance = std::numeric_limits<double>::infinity();
    for (const auto& triangle : triangles_) {
        const auto closest = closestPointOnTriangle(
            point,
            vertices_[triangle.vertices[0]],
            vertices_[triangle.vertices[1]],
            vertices_[triangle.vertices[2]]);
        if (!closest.valid || closest.squaredDistance >= bestSquaredDistance) {
            continue;
        }
        gp_Vec normal(
            vertices_[triangle.vertices[0]],
            vertices_[triangle.vertices[1]]);
        normal.Cross(
            gp_Vec(vertices_[triangle.vertices[0]], vertices_[triangle.vertices[2]]));
        if (normal.SquareMagnitude() <= Precision::SquareConfusion()) {
            continue;
        }
        normal.Normalize();
        const auto& first = parameters_[triangle.vertices[0]];
        const auto& second = parameters_[triangle.vertices[1]];
        const auto& third = parameters_[triangle.vertices[2]];
        result.valid = true;
        result.u = closest.barycentric[0] * first[0] +
            closest.barycentric[1] * second[0] + closest.barycentric[2] * third[0];
        result.v = closest.barycentric[0] * first[1] +
            closest.barycentric[1] * second[1] + closest.barycentric[2] * third[1];
        result.referencePoint = closest.point;
        result.referenceNormal = normal;
        bestSquaredDistance = closest.squaredDistance;
    }
    result.distance = result.valid ? std::sqrt(bestSquaredDistance) : 0.0;
    return result;
}

bool StlHybridParameterization::contains(double u, double v, double tolerance) const {
    for (const auto& triangle : triangles_) {
        const auto& first = parameters_[triangle.vertices[0]];
        const auto& second = parameters_[triangle.vertices[1]];
        const auto& third = parameters_[triangle.vertices[2]];
        const double denominator =
            (second[1] - third[1]) * (first[0] - third[0]) +
            (third[0] - second[0]) * (first[1] - third[1]);
        if (std::abs(denominator) <= 1.0e-20) {
            continue;
        }
        const double firstWeight =
            ((second[1] - third[1]) * (u - third[0]) +
             (third[0] - second[0]) * (v - third[1])) / denominator;
        const double secondWeight =
            ((third[1] - first[1]) * (u - third[0]) +
             (first[0] - third[0]) * (v - third[1])) / denominator;
        const double thirdWeight = 1.0 - firstWeight - secondWeight;
        if (firstWeight >= -tolerance && secondWeight >= -tolerance &&
            thirdWeight >= -tolerance) {
            return true;
        }
    }
    return false;
}

}
