#include "merge/MergeRegionGrower.h"

#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopAbs_Orientation.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <vector>

namespace spo {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct FaceSample {
    gp_Pnt center;
    gp_Dir normal;
    double area = 0.0;
};

double clampCosine(double value) {
    return std::max(-1.0, std::min(1.0, value));
}

std::optional<FaceSample> sampleFace(const TopoDS_Face& face) {
    if (face.IsNull()) {
        return std::nullopt;
    }

    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    if (!std::isfinite(uMin) || !std::isfinite(uMax) || !std::isfinite(vMin) || !std::isfinite(vMax)) {
        return std::nullopt;
    }

    BRepAdaptor_Surface surface(face);
    BRepLProp_SLProps props(surface, (uMin + uMax) * 0.5, (vMin + vMax) * 0.5, 1, Precision::Confusion());
    if (!props.IsNormalDefined()) {
        return std::nullopt;
    }

    FaceSample sample;
    sample.center = props.Value();
    sample.normal = props.Normal();
    if (face.Orientation() == TopAbs_REVERSED) {
        sample.normal.Reverse();
    }

    GProp_GProps areaProps;
    BRepGProp::SurfaceProperties(face, areaProps);
    sample.area = areaProps.Mass();
    return sample;
}

double distancePointToPlane(const gp_Pnt& point, const gp_Pnt& planePoint, const gp_Dir& planeNormal) {
    return std::abs(gp_Vec(planePoint, point).Dot(gp_Vec(planeNormal)));
}

double angleBetweenNormalsDeg(const gp_Dir& first, const gp_Dir& second) {
    const auto dot = std::abs(clampCosine(first.Dot(second)));
    return std::acos(dot) * 180.0 / kPi;
}

std::optional<EdgeId> sharedEdgeBetweenFaces(const TopologyGraph& topology, FaceId first, FaceId second) {
    const auto firstEdges = topology.edgesForFace(first);
    for (const auto edge : firstEdges) {
        const auto* adjacency = topology.adjacencyForEdge(edge);
        if (adjacency == nullptr) {
            continue;
        }
        if (std::find(adjacency->faces.begin(), adjacency->faces.end(), second) != adjacency->faces.end()) {
            return edge;
        }
    }
    return std::nullopt;
}

bool containsFace(const std::set<FaceId>& faces, const EdgeAdjacency& adjacency) {
    if (adjacency.faces.empty()) {
        return false;
    }

    for (const auto face : adjacency.faces) {
        if (!faces.contains(face)) {
            return false;
        }
    }
    return true;
}

struct AnalyticSurfaceSample {
    GeomAbs_SurfaceType surface_type = GeomAbs_OtherSurface;
    gp_Dir axis_direction = gp_Dir(0.0, 0.0, 1.0);
    gp_Pnt axis_location;
    gp_Pnt center;
    gp_Pnt apex;
    double radius = 0.0;
    double semi_angle_degrees = 0.0;
    double fit_error = 0.0;
};

bool solveLinear4x4(std::array<std::array<double, 4>, 4> matrix, std::array<double, 4> rhs, std::array<double, 4>& solution) {
    for (int pivot = 0; pivot < 4; ++pivot) {
        int pivotRow = pivot;
        for (int row = pivot + 1; row < 4; ++row) {
            if (std::abs(matrix[row][pivot]) > std::abs(matrix[pivotRow][pivot])) {
                pivotRow = row;
            }
        }
        if (std::abs(matrix[pivotRow][pivot]) <= 1.0e-12) {
            return false;
        }
        if (pivotRow != pivot) {
            std::swap(matrix[pivot], matrix[pivotRow]);
            std::swap(rhs[pivot], rhs[pivotRow]);
        }

        const auto divisor = matrix[pivot][pivot];
        for (int col = pivot; col < 4; ++col) {
            matrix[pivot][col] /= divisor;
        }
        rhs[pivot] /= divisor;

        for (int row = 0; row < 4; ++row) {
            if (row == pivot) {
                continue;
            }
            const auto factor = matrix[row][pivot];
            for (int col = pivot; col < 4; ++col) {
                matrix[row][col] -= factor * matrix[pivot][col];
            }
            rhs[row] -= factor * rhs[pivot];
        }
    }

    solution = rhs;
    return true;
}

bool solveLinear3x3(std::array<std::array<double, 3>, 3> matrix, std::array<double, 3> rhs, std::array<double, 3>& solution) {
    for (int pivot = 0; pivot < 3; ++pivot) {
        int pivotRow = pivot;
        for (int row = pivot + 1; row < 3; ++row) {
            if (std::abs(matrix[row][pivot]) > std::abs(matrix[pivotRow][pivot])) {
                pivotRow = row;
            }
        }
        if (std::abs(matrix[pivotRow][pivot]) <= 1.0e-12) {
            return false;
        }
        if (pivotRow != pivot) {
            std::swap(matrix[pivot], matrix[pivotRow]);
            std::swap(rhs[pivot], rhs[pivotRow]);
        }

        const auto divisor = matrix[pivot][pivot];
        for (int col = pivot; col < 3; ++col) {
            matrix[pivot][col] /= divisor;
        }
        rhs[pivot] /= divisor;

        for (int row = 0; row < 3; ++row) {
            if (row == pivot) {
                continue;
            }
            const auto factor = matrix[row][pivot];
            for (int col = pivot; col < 3; ++col) {
                matrix[row][col] -= factor * matrix[pivot][col];
            }
            rhs[row] -= factor * rhs[pivot];
        }
    }

    solution = rhs;
    return true;
}

std::optional<AnalyticSurfaceSample> fitSphereLikeSurface(const TopoDS_Face& face);

std::optional<AnalyticSurfaceSample> fitCylinderLikeSurface(
    const TopoDS_Face& face,
    const MergePlannerOptions& options) {
    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    if (!std::isfinite(uMin) || !std::isfinite(uMax) || !std::isfinite(vMin) || !std::isfinite(vMax)) {
        return std::nullopt;
    }

    if (fitSphereLikeSurface(face).has_value()) {
        return std::nullopt;
    }

    BRepAdaptor_Surface surface(face);
    std::vector<gp_Pnt> points;
    std::vector<gp_Dir> normals;
    points.reserve(25);
    normals.reserve(25);
    for (const auto uFactor : {0.15, 0.325, 0.5, 0.675, 0.85}) {
        for (const auto vFactor : {0.15, 0.325, 0.5, 0.675, 0.85}) {
            BRepLProp_SLProps props(surface, uMin + (uMax - uMin) * uFactor, vMin + (vMax - vMin) * vFactor, 1, Precision::Confusion());
            if (!props.IsNormalDefined()) {
                return std::nullopt;
            }
            auto normal = props.Normal();
            if (face.Orientation() == TopAbs_REVERSED) {
                normal.Reverse();
            }
            points.push_back(props.Value());
            normals.push_back(normal);
        }
    }
    if (points.size() < 9) {
        return std::nullopt;
    }

    gp_Vec bestAxis;
    double bestMagnitude = 0.0;
    for (std::size_t i = 0; i < normals.size(); ++i) {
        for (std::size_t j = i + 1; j < normals.size(); ++j) {
            const auto candidate = gp_Vec(normals[i]).Crossed(gp_Vec(normals[j]));
            const auto magnitude = candidate.Magnitude();
            if (magnitude > bestMagnitude) {
                bestMagnitude = magnitude;
                bestAxis = candidate;
            }
        }
    }
    if (bestMagnitude <= std::sin(5.0 * kPi / 180.0)) {
        return std::nullopt;
    }
    const gp_Dir axisDirection(bestAxis);
    const gp_Vec axisVector(axisDirection);

    double maxAxisNormalDot = 0.0;
    for (const auto& normal : normals) {
        maxAxisNormalDot = std::max(maxAxisNormalDot, std::abs(gp_Vec(normal).Dot(axisVector)));
    }
    constexpr double kMaxAxisNormalAngleDegrees = 8.0;
    if (maxAxisNormalDot > std::sin(kMaxAxisNormalAngleDegrees * kPi / 180.0)) {
        return std::nullopt;
    }

    const gp_Dir reference = std::abs(axisDirection.Z()) < 0.9 ? gp_Dir(0.0, 0.0, 1.0) : gp_Dir(1.0, 0.0, 0.0);
    auto uVector = axisVector.Crossed(gp_Vec(reference));
    if (uVector.Magnitude() <= Precision::Confusion()) {
        return std::nullopt;
    }
    uVector.Normalize();
    auto vVector = axisVector.Crossed(uVector);
    vVector.Normalize();

    std::array<std::array<double, 3>, 3> normalMatrix {};
    std::array<double, 3> normalRhs {};
    std::vector<std::array<double, 3>> coordinates;
    coordinates.reserve(points.size());
    for (const auto& point : points) {
        const gp_Vec vector(gp_Pnt(0.0, 0.0, 0.0), point);
        const auto x = vector.Dot(uVector);
        const auto y = vector.Dot(vVector);
        const auto t = vector.Dot(axisVector);
        coordinates.push_back({x, y, t});

        const std::array<double, 3> row {x, y, 1.0};
        const auto rhs = -(x * x + y * y);
        for (int i = 0; i < 3; ++i) {
            normalRhs[i] += row[i] * rhs;
            for (int j = 0; j < 3; ++j) {
                normalMatrix[i][j] += row[i] * row[j];
            }
        }
    }

    std::array<double, 3> coefficients {};
    if (!solveLinear3x3(normalMatrix, normalRhs, coefficients)) {
        return std::nullopt;
    }
    const auto centerX = -0.5 * coefficients[0];
    const auto centerY = -0.5 * coefficients[1];
    const auto radiusSquared = centerX * centerX + centerY * centerY - coefficients[2];
    if (radiusSquared <= Precision::Confusion()) {
        return std::nullopt;
    }
    const auto radius = std::sqrt(radiusSquared);
    const auto radiusTolerance = std::max(options.max_cylinder_fit_error, radius * 0.015);

    double meanT = 0.0;
    double maxRadiusError = 0.0;
    for (const auto& coordinate : coordinates) {
        const auto radiusAtPoint = std::hypot(coordinate[0] - centerX, coordinate[1] - centerY);
        maxRadiusError = std::max(maxRadiusError, std::abs(radiusAtPoint - radius));
        meanT += coordinate[2];
    }
    meanT /= static_cast<double>(coordinates.size());
    if (maxRadiusError > radiusTolerance) {
        return std::nullopt;
    }

    gp_Vec axisLocationVector = uVector.Multiplied(centerX) + vVector.Multiplied(centerY) + axisVector.Multiplied(meanT);
    const gp_Pnt axisLocation(axisLocationVector.X(), axisLocationVector.Y(), axisLocationVector.Z());

    double maxNormalRadialAngle = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const gp_Vec fromAxis(axisLocation, points[i]);
        const auto axialOffset = fromAxis.Dot(axisVector);
        auto radial = fromAxis - axisVector.Multiplied(axialOffset);
        if (radial.Magnitude() <= Precision::Confusion()) {
            return std::nullopt;
        }
        radial.Normalize();
        const auto dot = std::abs(clampCosine(radial.Dot(gp_Vec(normals[i]))));
        maxNormalRadialAngle = std::max(maxNormalRadialAngle, std::acos(dot) * 180.0 / kPi);
    }
    constexpr double kMaxNormalRadialAngleDegrees = 12.0;
    if (maxNormalRadialAngle > kMaxNormalRadialAngleDegrees) {
        return std::nullopt;
    }

    AnalyticSurfaceSample sample;
    sample.surface_type = GeomAbs_Cylinder;
    sample.axis_direction = axisDirection;
    sample.axis_location = axisLocation;
    sample.radius = radius;
    sample.fit_error = std::max(maxRadiusError / radiusTolerance, maxNormalRadialAngle / kMaxNormalRadialAngleDegrees);
    return sample;
}

std::optional<AnalyticSurfaceSample> fitSphereLikeSurface(const TopoDS_Face& face) {
    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    if (!std::isfinite(uMin) || !std::isfinite(uMax) || !std::isfinite(vMin) || !std::isfinite(vMax)) {
        return std::nullopt;
    }

    BRepAdaptor_Surface surface(face);
    std::vector<gp_Pnt> points;
    points.reserve(9);
    for (const auto uFactor : {0.25, 0.5, 0.75}) {
        for (const auto vFactor : {0.25, 0.5, 0.75}) {
            BRepLProp_SLProps props(surface, uMin + (uMax - uMin) * uFactor, vMin + (vMax - vMin) * vFactor, 0, Precision::Confusion());
            points.push_back(props.Value());
        }
    }
    if (points.size() < 4) {
        return std::nullopt;
    }

    std::array<std::array<double, 4>, 4> normalMatrix {};
    std::array<double, 4> normalRhs {};
    for (const auto& point : points) {
        const std::array<double, 4> row {point.X(), point.Y(), point.Z(), 1.0};
        const auto rhs = -(point.X() * point.X() + point.Y() * point.Y() + point.Z() * point.Z());
        for (int i = 0; i < 4; ++i) {
            normalRhs[i] += row[i] * rhs;
            for (int j = 0; j < 4; ++j) {
                normalMatrix[i][j] += row[i] * row[j];
            }
        }
    }

    std::array<double, 4> coefficients {};
    if (!solveLinear4x4(normalMatrix, normalRhs, coefficients)) {
        return std::nullopt;
    }

    const gp_Pnt center(-0.5 * coefficients[0], -0.5 * coefficients[1], -0.5 * coefficients[2]);
    const auto radiusSquared = center.X() * center.X() + center.Y() * center.Y() + center.Z() * center.Z() - coefficients[3];
    if (radiusSquared <= Precision::Confusion()) {
        return std::nullopt;
    }
    const auto radius = std::sqrt(radiusSquared);

    double maxResidual = 0.0;
    for (const auto& point : points) {
        maxResidual = std::max(maxResidual, std::abs(center.Distance(point) - radius));
    }
    if (maxResidual > std::max(0.20, radius * 0.06)) {
        return std::nullopt;
    }

    AnalyticSurfaceSample sample;
    sample.surface_type = GeomAbs_Sphere;
    sample.center = center;
    sample.radius = radius;
    return sample;
}

std::optional<AnalyticSurfaceSample> sampleAnalyticSurface(
    const TopoDS_Face& face,
    const MergePlannerOptions& options,
    GeomAbs_SurfaceType expectedType) {
    if (face.IsNull()) {
        return std::nullopt;
    }

    BRepAdaptor_Surface surface(face);
    if (surface.GetType() != expectedType) {
        if (expectedType == GeomAbs_Cylinder &&
            (surface.GetType() == GeomAbs_BSplineSurface ||
                surface.GetType() == GeomAbs_BezierSurface ||
                surface.GetType() == GeomAbs_SurfaceOfRevolution)) {
            return fitCylinderLikeSurface(face, options);
        }
        if (expectedType == GeomAbs_Sphere &&
            (surface.GetType() == GeomAbs_BSplineSurface || surface.GetType() == GeomAbs_BezierSurface)) {
            return fitSphereLikeSurface(face);
        }
        return std::nullopt;
    }

    AnalyticSurfaceSample sample;
    sample.surface_type = expectedType;
    switch (expectedType) {
    case GeomAbs_Cylinder: {
        const auto cylinder = surface.Cylinder();
        sample.axis_direction = cylinder.Axis().Direction();
        sample.axis_location = cylinder.Axis().Location();
        sample.radius = cylinder.Radius();
        return sample;
    }
    case GeomAbs_Sphere: {
        const auto sphere = surface.Sphere();
        sample.center = sphere.Location();
        sample.radius = sphere.Radius();
        return sample;
    }
    case GeomAbs_Cone: {
        const auto cone = surface.Cone();
        sample.axis_direction = cone.Axis().Direction();
        sample.axis_location = cone.Axis().Location();
        sample.apex = cone.Apex();
        sample.radius = cone.RefRadius();
        sample.semi_angle_degrees = std::abs(cone.SemiAngle()) * 180.0 / kPi;
        return sample;
    }
    default:
        return std::nullopt;
    }
}

double axisAngleDegrees(const gp_Dir& first, const gp_Dir& second) {
    const auto dot = std::abs(clampCosine(first.Dot(second)));
    return std::acos(dot) * 180.0 / kPi;
}

double axisPositionDistance(const AnalyticSurfaceSample& first, const AnalyticSurfaceSample& second) {
    return gp_Lin(first.axis_location, first.axis_direction).Distance(gp_Lin(second.axis_location, second.axis_direction));
}

struct AnalyticCompatibility {
    bool compatible = false;
    double fit_error = 0.0;
    double axis_angle_degrees = 0.0;
    double distance = 0.0;
};

AnalyticCompatibility compareAnalyticSamples(
    const AnalyticSurfaceSample& seed,
    const AnalyticSurfaceSample& next,
    MergeCandidateType candidateType,
    const MergePlannerOptions& options) {
    AnalyticCompatibility result;
    switch (candidateType) {
    case MergeCandidateType::CylinderLike: {
        result.axis_angle_degrees = axisAngleDegrees(seed.axis_direction, next.axis_direction);
        const auto axisDistance = axisPositionDistance(seed, next);
        const auto radiusDelta = std::abs(seed.radius - next.radius);
        result.distance = std::max(axisDistance, radiusDelta);
        result.fit_error = std::max({result.axis_angle_degrees / options.max_cylinder_axis_angle_degrees,
            axisDistance / options.max_cylinder_axis_position_delta,
            radiusDelta / options.max_cylinder_radius_delta});
        result.compatible =
            result.axis_angle_degrees <= options.max_cylinder_axis_angle_degrees &&
            axisDistance <= options.max_cylinder_axis_position_delta &&
            radiusDelta <= options.max_cylinder_radius_delta;
        return result;
    }
    case MergeCandidateType::SphereLike: {
        const auto centerDelta = seed.center.Distance(next.center);
        const auto radiusDelta = std::abs(seed.radius - next.radius);
        const auto centerTolerance = std::max(options.max_sphere_center_delta, seed.radius * 0.10);
        const auto radiusTolerance = std::max(options.max_sphere_radius_delta, seed.radius * 0.08);
        result.distance = std::max(centerDelta, radiusDelta);
        result.fit_error = std::max(centerDelta / centerTolerance, radiusDelta / radiusTolerance);
        result.compatible =
            centerDelta <= centerTolerance &&
            radiusDelta <= radiusTolerance;
        return result;
    }
    case MergeCandidateType::ConeLike: {
        result.axis_angle_degrees = axisAngleDegrees(seed.axis_direction, next.axis_direction);
        const auto semiAngleDelta = std::abs(seed.semi_angle_degrees - next.semi_angle_degrees);
        const auto apexDelta = seed.apex.Distance(next.apex);
        result.distance = std::max(apexDelta, semiAngleDelta);
        result.fit_error = std::max({result.axis_angle_degrees / options.max_cone_axis_angle_degrees,
            semiAngleDelta / options.max_cone_semi_angle_delta_degrees,
            apexDelta / options.max_cone_apex_delta});
        result.compatible =
            result.axis_angle_degrees <= options.max_cone_axis_angle_degrees &&
            semiAngleDelta <= options.max_cone_semi_angle_delta_degrees &&
            apexDelta <= options.max_cone_apex_delta &&
            result.fit_error <= options.max_cone_fit_error / std::max(options.max_cone_fit_error, 1.0e-12);
        return result;
    }
    default:
        return result;
    }
}

MergeRiskLevel riskFromFitError(double fitError) {
    if (fitError <= 0.5) {
        return MergeRiskLevel::Low;
    }
    if (fitError <= 1.0) {
        return MergeRiskLevel::Medium;
    }
    return MergeRiskLevel::High;
}

std::vector<MergeCandidate> growAnalyticRegions(
    const ShapeDocument& document,
    const std::set<EdgeId>& protectedEdges,
    const MergePlannerOptions& options,
    GeomAbs_SurfaceType surfaceType,
    MergeCandidateType candidateType,
    int* visitedFaces,
    int* rejectedRegions) {
    std::vector<MergeCandidate> candidates;
    const auto& topology = document.topology();
    std::vector<bool> visited(topology.faceCount(), false);
    int visitedCount = 0;
    int rejectedCount = 0;

    for (FaceId seed = 0; seed < topology.faceCount(); ++seed) {
        if (visited[seed]) {
            continue;
        }

        const auto seedSurface = sampleAnalyticSurface(topology.face(seed), options, surfaceType);
        if (!seedSurface.has_value()) {
            continue;
        }

        std::queue<FaceId> queue;
        std::set<FaceId> regionFaces;
        std::set<EdgeId> blockedEdges;
        std::vector<double> distances;
        std::vector<double> fitErrors;
        std::vector<double> axisAngles;

        visited[seed] = true;
        ++visitedCount;
        queue.push(seed);
        regionFaces.insert(seed);
        distances.push_back(0.0);
        fitErrors.push_back(seedSurface->fit_error);
        axisAngles.push_back(0.0);

        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();

            for (const auto neighbor : topology.adjacentFaces(current)) {
                const auto sharedEdge = sharedEdgeBetweenFaces(topology, current, neighbor);
                if (sharedEdge.has_value() && protectedEdges.contains(*sharedEdge)) {
                    blockedEdges.insert(*sharedEdge);
                    continue;
                }
                if (visited[neighbor]) {
                    continue;
                }

                const auto neighborSurface = sampleAnalyticSurface(topology.face(neighbor), options, surfaceType);
                if (!neighborSurface.has_value()) {
                    continue;
                }

                const auto compatibility = compareAnalyticSamples(*seedSurface, *neighborSurface, candidateType, options);
                if (!compatibility.compatible) {
                    continue;
                }

                visited[neighbor] = true;
                ++visitedCount;
                queue.push(neighbor);
                regionFaces.insert(neighbor);
                distances.push_back(compatibility.distance);
                fitErrors.push_back(std::max(compatibility.fit_error, neighborSurface->fit_error));
                axisAngles.push_back(compatibility.axis_angle_degrees);
            }
        }

        if (static_cast<int>(regionFaces.size()) < options.min_analytic_region_faces) {
            ++rejectedCount;
            continue;
        }

        MergeCandidate candidate;
        candidate.candidate_id = static_cast<int>(candidates.size());
        candidate.candidate_type = candidateType;
        candidate.faces.assign(regionFaces.begin(), regionFaces.end());
        candidate.face_count = static_cast<int>(candidate.faces.size());
        candidate.blocked_edges.assign(blockedEdges.begin(), blockedEdges.end());
        candidate.protected_edges = candidate.blocked_edges;

        std::set<EdgeId> internalEdges;
        std::set<EdgeId> boundaryEdges;
        for (const auto face : candidate.faces) {
            const auto faceSample = sampleFace(topology.face(face));
            if (faceSample.has_value()) {
                candidate.total_area += faceSample->area;
            }

            for (const auto edge : topology.edgesForFace(face)) {
                const auto* adjacency = topology.adjacencyForEdge(edge);
                if (adjacency != nullptr && adjacency->faces.size() == 2 && containsFace(regionFaces, *adjacency)) {
                    internalEdges.insert(edge);
                } else {
                    boundaryEdges.insert(edge);
                }
            }
        }

        candidate.internal_edges.assign(internalEdges.begin(), internalEdges.end());
        candidate.boundary_edges.assign(boundaryEdges.begin(), boundaryEdges.end());
        candidate.internal_edge_count = static_cast<int>(candidate.internal_edges.size());
        candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());
        candidate.max_distance = distances.empty() ? 0.0 : *std::max_element(distances.begin(), distances.end());
        candidate.mean_distance = distances.empty()
            ? 0.0
            : std::accumulate(distances.begin(), distances.end(), 0.0) / distances.size();
        candidate.max_normal_angle_deg = axisAngles.empty() ? 0.0 : *std::max_element(axisAngles.begin(), axisAngles.end());
        candidate.mean_normal_angle_deg = axisAngles.empty()
            ? 0.0
            : std::accumulate(axisAngles.begin(), axisAngles.end(), 0.0) / axisAngles.size();
        candidate.fit_error = fitErrors.empty() ? 0.0 : *std::max_element(fitErrors.begin(), fitErrors.end());
        candidate.risk_level = riskFromFitError(candidate.fit_error);
        candidates.push_back(std::move(candidate));
    }

    if (visitedFaces != nullptr) {
        *visitedFaces = visitedCount;
    }
    if (rejectedRegions != nullptr) {
        *rejectedRegions = rejectedCount;
    }
    return candidates;
}

}

std::vector<MergeCandidate> MergeRegionGrower::growPlaneLikeRegions(
    const ShapeDocument& document,
    const std::set<EdgeId>& protectedEdges,
    const MergePlannerOptions& options,
    int* visitedFaces,
    int* rejectedRegions) const {
    std::vector<MergeCandidate> candidates;
    const auto& topology = document.topology();
    std::vector<bool> visited(topology.faceCount(), false);
    int visitedCount = 0;
    int rejectedCount = 0;

    for (FaceId seed = 0; seed < topology.faceCount(); ++seed) {
        if (visited[seed]) {
            continue;
        }

        const auto seedSample = sampleFace(topology.face(seed));
        if (!seedSample.has_value()) {
            visited[seed] = true;
            ++visitedCount;
            ++rejectedCount;
            continue;
        }

        std::queue<FaceId> queue;
        std::set<FaceId> regionFaces;
        std::set<EdgeId> blockedEdges;
        std::vector<double> distances;
        std::vector<double> normalAngles;

        visited[seed] = true;
        ++visitedCount;
        queue.push(seed);
        regionFaces.insert(seed);
        distances.push_back(0.0);
        normalAngles.push_back(0.0);

        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();

            for (const auto neighbor : topology.adjacentFaces(current)) {
                const auto sharedEdge = sharedEdgeBetweenFaces(topology, current, neighbor);
                if (sharedEdge.has_value() && protectedEdges.contains(*sharedEdge)) {
                    blockedEdges.insert(*sharedEdge);
                    continue;
                }

                if (visited[neighbor]) {
                    continue;
                }

                const auto neighborSample = sampleFace(topology.face(neighbor));
                if (!neighborSample.has_value()) {
                    continue;
                }

                const auto normalAngle = angleBetweenNormalsDeg(seedSample->normal, neighborSample->normal);
                const auto distance = distancePointToPlane(neighborSample->center, seedSample->center, seedSample->normal);
                if (normalAngle > options.max_normal_angle_degrees || distance > options.max_plane_distance) {
                    continue;
                }

                visited[neighbor] = true;
                ++visitedCount;
                queue.push(neighbor);
                regionFaces.insert(neighbor);
                distances.push_back(distance);
                normalAngles.push_back(normalAngle);
            }
        }

        if (static_cast<int>(regionFaces.size()) < options.min_region_faces) {
            ++rejectedCount;
            continue;
        }

        MergeCandidate candidate;
        candidate.candidate_id = static_cast<int>(candidates.size());
        candidate.candidate_type = MergeCandidateType::PlaneLike;
        candidate.faces.assign(regionFaces.begin(), regionFaces.end());
        candidate.face_count = static_cast<int>(candidate.faces.size());
        candidate.blocked_edges.assign(blockedEdges.begin(), blockedEdges.end());
        candidate.protected_edges = candidate.blocked_edges;

        std::set<EdgeId> internalEdges;
        std::set<EdgeId> boundaryEdges;
        for (const auto face : candidate.faces) {
            const auto faceSample = sampleFace(topology.face(face));
            if (faceSample.has_value()) {
                candidate.total_area += faceSample->area;
            }

            for (const auto edge : topology.edgesForFace(face)) {
                const auto* adjacency = topology.adjacencyForEdge(edge);
                if (adjacency != nullptr && adjacency->faces.size() == 2 && containsFace(regionFaces, *adjacency)) {
                    internalEdges.insert(edge);
                } else {
                    boundaryEdges.insert(edge);
                }
            }
        }

        candidate.internal_edges.assign(internalEdges.begin(), internalEdges.end());
        candidate.boundary_edges.assign(boundaryEdges.begin(), boundaryEdges.end());
        candidate.internal_edge_count = static_cast<int>(candidate.internal_edges.size());
        candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());
        candidate.max_distance = distances.empty() ? 0.0 : *std::max_element(distances.begin(), distances.end());
        candidate.mean_distance = distances.empty()
            ? 0.0
            : std::accumulate(distances.begin(), distances.end(), 0.0) / distances.size();
        candidate.max_normal_angle_deg = normalAngles.empty()
            ? 0.0
            : *std::max_element(normalAngles.begin(), normalAngles.end());
        candidate.mean_normal_angle_deg = normalAngles.empty()
            ? 0.0
            : std::accumulate(normalAngles.begin(), normalAngles.end(), 0.0) / normalAngles.size();
        candidate.fit_error = candidate.max_distance;
        candidate.risk_level = candidate.max_normal_angle_deg <= options.max_normal_angle_degrees * 0.5
            ? MergeRiskLevel::Low
            : MergeRiskLevel::Medium;

        candidates.push_back(std::move(candidate));
    }

    if (visitedFaces != nullptr) {
        *visitedFaces = visitedCount;
    }
    if (rejectedRegions != nullptr) {
        *rejectedRegions = rejectedCount;
    }
    return candidates;
}

std::vector<MergeCandidate> MergeRegionGrower::growCylinderLikeRegions(
    const ShapeDocument& document,
    const std::set<EdgeId>& protectedEdges,
    const MergePlannerOptions& options,
    int* visitedFaces,
    int* rejectedRegions) const {
    return growAnalyticRegions(
        document,
        protectedEdges,
        options,
        GeomAbs_Cylinder,
        MergeCandidateType::CylinderLike,
        visitedFaces,
        rejectedRegions);
}

std::vector<MergeCandidate> MergeRegionGrower::growSphereLikeRegions(
    const ShapeDocument& document,
    const std::set<EdgeId>& protectedEdges,
    const MergePlannerOptions& options,
    int* visitedFaces,
    int* rejectedRegions) const {
    return growAnalyticRegions(
        document,
        protectedEdges,
        options,
        GeomAbs_Sphere,
        MergeCandidateType::SphereLike,
        visitedFaces,
        rejectedRegions);
}

std::vector<MergeCandidate> MergeRegionGrower::growConeLikeRegions(
    const ShapeDocument& document,
    const std::set<EdgeId>& protectedEdges,
    const MergePlannerOptions& options,
    int* visitedFaces,
    int* rejectedRegions) const {
    return growAnalyticRegions(
        document,
        protectedEdges,
        options,
        GeomAbs_Cone,
        MergeCandidateType::ConeLike,
        visitedFaces,
        rejectedRegions);
}

std::vector<MergeCandidate> MergeRegionGrower::growTorusLikeRegions(
    const ShapeDocument&,
    const std::set<EdgeId>&,
    const MergePlannerOptions&,
    int* visitedFaces,
    int* rejectedRegions) const {
    if (visitedFaces != nullptr) {
        *visitedFaces = 0;
    }
    if (rejectedRegions != nullptr) {
        *rejectedRegions = 0;
    }
    return {};
}

}
