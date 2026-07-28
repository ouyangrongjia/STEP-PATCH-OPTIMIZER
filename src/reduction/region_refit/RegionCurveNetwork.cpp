#include "reduction/region_refit/RegionCurveNetwork.h"

#include <Eigen/Dense>

#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Tool.hxx>
#include <Geom_BSplineCurve.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace spo {

namespace {

using MeshEdge = std::array<std::size_t, 2>;

MeshEdge orderedEdge(const std::size_t first, const std::size_t second) {
    return {std::min(first, second), std::max(first, second)};
}

bool finitePoint(const gp_Pnt& point) {
    return std::isfinite(point.X()) && std::isfinite(point.Y()) &&
        std::isfinite(point.Z());
}

std::vector<std::size_t> orderedChain(
    const std::vector<MeshEdge>& edges,
    bool& valid) {
    valid = false;
    std::map<std::size_t, std::vector<std::size_t>> adjacency;
    std::set<MeshEdge> remaining;
    for (const auto& edge : edges) {
        if (edge[0] == edge[1]) return {};
        adjacency[edge[0]].push_back(edge[1]);
        adjacency[edge[1]].push_back(edge[0]);
        remaining.insert(orderedEdge(edge[0], edge[1]));
    }
    std::vector<std::size_t> endpoints;
    for (auto& [vertex, neighbors] : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
        if (neighbors.size() > 2 || neighbors.empty()) return {};
        if (neighbors.size() == 1) endpoints.push_back(vertex);
    }
    if (endpoints.size() != 2) return {};
    std::sort(endpoints.begin(), endpoints.end());
    std::vector<std::size_t> result {endpoints.front()};
    std::optional<std::size_t> previous;
    auto current = endpoints.front();
    while (current != endpoints.back()) {
        std::optional<std::size_t> next;
        for (const auto neighbor : adjacency[current]) {
            const auto edge = orderedEdge(current, neighbor);
            if (remaining.contains(edge) &&
                (!previous.has_value() || neighbor != *previous)) {
                next = neighbor;
                break;
            }
        }
        if (!next.has_value()) return {};
        remaining.erase(orderedEdge(current, *next));
        previous = current;
        current = *next;
        result.push_back(current);
    }
    valid = remaining.empty();
    return valid ? result : std::vector<std::size_t> {};
}

const RegionBoundaryEdgeConstraint* constraintFor(
    const RegionTopologyConstraints& constraints,
    const EdgeId edgeId) {
    const auto found = std::find_if(
        constraints.boundaryEdges.begin(),
        constraints.boundaryEdges.end(),
        [&](const auto& constraint) { return constraint.edgeId == edgeId; });
    return found == constraints.boundaryEdges.end() ? nullptr : &*found;
}

std::optional<EdgeId> commonBoundaryEdgeId(
    const RegionReferenceVertex& first,
    const RegionReferenceVertex& second) {
    std::optional<EdgeId> result;
    for (const auto edgeId : first.boundaryEdgeIds) {
        if (std::find(
                second.boundaryEdgeIds.begin(),
                second.boundaryEdgeIds.end(),
                edgeId) != second.boundaryEdgeIds.end() &&
            (!result.has_value() || edgeId < *result)) {
            result = edgeId;
        }
    }
    return result;
}

bool projectToOriginalRegion(
    const gp_Pnt& point,
    const RegionTopologyConstraints& constraints,
    gp_Pnt& projected,
    double& distance,
    const double acceptablePreferredDistance,
    const std::optional<FaceId> preferredFaceId) {
    distance = std::numeric_limits<double>::infinity();
    const auto evaluate = [&](const TopoDS_Face& face) {
        if (face.IsNull()) return;
        try {
            BRepExtrema_DistShapeShape extrema(
                BRepBuilderAPI_MakeVertex(point).Vertex(), face);
            extrema.Perform();
            if (!extrema.IsDone() || extrema.NbSolution() <= 0) return;
            const auto candidateDistance = extrema.Value();
            if (candidateDistance < distance) {
                distance = candidateDistance;
                projected = extrema.PointOnShape2(1);
            }
        } catch (const Standard_Failure&) {
        }
    };
    if (preferredFaceId.has_value()) {
        const auto preferred = std::find_if(
            constraints.selectedFaceGeometry.begin(),
            constraints.selectedFaceGeometry.end(),
            [&](const auto& item) {
                return item.first == *preferredFaceId;
            });
        if (preferred != constraints.selectedFaceGeometry.end()) {
            evaluate(preferred->second);
            if (std::isfinite(distance) &&
                distance <= acceptablePreferredDistance) {
                return finitePoint(projected);
            }
        }
    }
    for (const auto& [faceId, face] : constraints.selectedFaceGeometry) {
        if (preferredFaceId.has_value() && faceId == *preferredFaceId) continue;
        evaluate(face);
    }
    return finitePoint(projected) && std::isfinite(distance);
}

std::optional<FaceId> preferredFace(
    const RegionReferenceVertex& vertex) {
    return vertex.sources.empty()
        ? std::nullopt
        : std::optional<FaceId>(vertex.sources.front().faceId);
}

RegionCurveContinuity internalContinuity(
    const RegionReferenceMesh& mesh,
    const std::vector<std::size_t>& vertices,
    const RegionRefitOptions& options) {
    for (const auto vertexIndex : vertices) {
        const auto& vertex = mesh.vertices[vertexIndex];
        if (vertex.normalDiscontinuity) {
            return RegionCurveContinuity::G0;
        }
        for (std::size_t first = 0; first < vertex.sources.size(); ++first) {
            if (!vertex.sources[first].normalDefined) continue;
            for (std::size_t second = first + 1;
                 second < vertex.sources.size(); ++second) {
                if (!vertex.sources[second].normalDefined) continue;
                const auto dot = std::clamp(
                    vertex.sources[first].normal.Dot(
                        vertex.sources[second].normal),
                    -1.0,
                    1.0);
                const auto degrees =
                    std::acos(dot) * 180.0 / 3.14159265358979323846;
                if (degrees > options.g1ToleranceDegrees) {
                    return RegionCurveContinuity::G0;
                }
            }
        }
    }
    if (!options.enableInternalSeamG2) {
        return RegionCurveContinuity::G1;
    }
    for (const auto vertexIndex : vertices) {
        const auto& sources = mesh.vertices[vertexIndex].sources;
        if (sources.empty()) {
            return RegionCurveContinuity::G1;
        }
        for (const auto& source : sources) {
            if (!source.curvatureDefined ||
                !std::isfinite(source.minimumCurvature) ||
                !std::isfinite(source.maximumCurvature)) {
                return RegionCurveContinuity::G1;
            }
        }
        for (std::size_t first = 0; first < sources.size(); ++first) {
            for (std::size_t second = first + 1;
                 second < sources.size(); ++second) {
                if (std::abs(
                        sources[first].minimumCurvature -
                        sources[second].minimumCurvature) >
                        options.g2CurvatureTolerance ||
                    std::abs(
                        sources[first].maximumCurvature -
                        sources[second].maximumCurvature) >
                        options.g2CurvatureTolerance) {
                    return RegionCurveContinuity::G1;
                }
            }
        }
    }
    return RegionCurveContinuity::G2;
}

bool curveParameter(
    const Handle(Geom_Curve)& curve,
    const gp_Pnt& point,
    const double first,
    const double last,
    double& parameter) {
    if (curve.IsNull()) return false;
    try {
        GeomAPI_ProjectPointOnCurve projection(
            point, curve, std::min(first, last), std::max(first, last));
        if (projection.NbPoints() <= 0) return false;
        parameter = projection.LowerDistanceParameter();
        return std::isfinite(parameter);
    } catch (const Standard_Failure&) {
        return false;
    }
}

std::size_t nodeFor(
    RegionCurveNetwork& network,
    std::map<std::size_t, std::size_t>& nodeByMeshVertex,
    const std::size_t meshVertex,
    const gp_Pnt& point,
    const bool originalBoundary) {
    const auto found = nodeByMeshVertex.find(meshVertex);
    if (found != nodeByMeshVertex.end()) {
        auto& node = network.nodes[found->second];
        if (originalBoundary && !node.originalBoundary) {
            node.point = point;
            node.originalBoundary = true;
        }
        return found->second;
    }
    const auto id = network.nodes.size();
    network.nodes.push_back({id, meshVertex, point, originalBoundary, {}});
    nodeByMeshVertex.emplace(meshVertex, id);
    return id;
}

std::vector<double> chordParameters(
    const std::vector<gp_Pnt>& points,
    const double first,
    const double last) {
    std::vector<double> cumulative(points.size(), 0.0);
    for (std::size_t index = 1; index < points.size(); ++index) {
        cumulative[index] = cumulative[index - 1] +
            std::max(points[index - 1].Distance(points[index]), 1.0e-12);
    }
    const auto length = cumulative.back();
    if (length <= 1.0e-14) return {};
    for (auto& parameter : cumulative) {
        parameter = first + (last - first) * parameter / length;
    }
    return cumulative;
}

constexpr int kInternalCurveDegree = 3;

std::vector<double> curveFullKnotVector(
    const std::vector<double>& uniqueKnots) {
    std::vector<double> full;
    full.reserve(
        uniqueKnots.size() +
        2 * static_cast<std::size_t>(kInternalCurveDegree));
    for (int repeat = 0; repeat <= kInternalCurveDegree; ++repeat) {
        full.push_back(uniqueKnots.front());
    }
    for (std::size_t index = 1; index + 1 < uniqueKnots.size(); ++index) {
        full.push_back(uniqueKnots[index]);
    }
    for (int repeat = 0; repeat <= kInternalCurveDegree; ++repeat) {
        full.push_back(uniqueKnots.back());
    }
    return full;
}

std::vector<double> curveBasisValues(
    const std::vector<double>& uniqueKnots,
    const double parameter) {
    const auto fullKnots = curveFullKnotVector(uniqueKnots);
    const auto controlCount =
        static_cast<int>(uniqueKnots.size()) + kInternalCurveDegree - 1;
    const auto clamped = std::clamp(
        parameter, uniqueKnots.front(), uniqueKnots.back());
    const auto lastControl = controlCount - 1;
    auto span = lastControl;
    if (clamped < uniqueKnots.back()) {
        const auto upper = std::upper_bound(
            fullKnots.begin() + kInternalCurveDegree,
            fullKnots.begin() + controlCount + 1,
            clamped);
        span = std::clamp(
            static_cast<int>(std::distance(fullKnots.begin(), upper)) - 1,
            kInternalCurveDegree,
            lastControl);
    }
    std::array<double, kInternalCurveDegree + 1> local {};
    std::array<double, kInternalCurveDegree + 1> left {};
    std::array<double, kInternalCurveDegree + 1> right {};
    local[0] = 1.0;
    for (int degree = 1; degree <= kInternalCurveDegree; ++degree) {
        left[static_cast<std::size_t>(degree)] =
            clamped - fullKnots[static_cast<std::size_t>(
                span + 1 - degree)];
        right[static_cast<std::size_t>(degree)] =
            fullKnots[static_cast<std::size_t>(span + degree)] - clamped;
        auto saved = 0.0;
        for (int index = 0; index < degree; ++index) {
            const auto denominator =
                right[static_cast<std::size_t>(index + 1)] +
                left[static_cast<std::size_t>(degree - index)];
            const auto term = denominator > 0.0
                ? local[static_cast<std::size_t>(index)] / denominator
                : 0.0;
            local[static_cast<std::size_t>(index)] =
                saved +
                right[static_cast<std::size_t>(index + 1)] * term;
            saved =
                left[static_cast<std::size_t>(degree - index)] * term;
        }
        local[static_cast<std::size_t>(degree)] = saved;
    }
    std::vector<double> values(
        static_cast<std::size_t>(controlCount), 0.0);
    for (int index = 0; index <= kInternalCurveDegree; ++index) {
        values[static_cast<std::size_t>(
            span - kInternalCurveDegree + index)] =
            local[static_cast<std::size_t>(index)];
    }
    return values;
}

std::vector<gp_Pnt> resampleByArcLength(
    const std::vector<gp_Pnt>& points,
    const std::size_t requestedCount) {
    if (points.size() < 2 || requestedCount < 2) return {};
    std::vector<double> cumulative(points.size(), 0.0);
    for (std::size_t index = 1; index < points.size(); ++index) {
        cumulative[index] = cumulative[index - 1] +
            points[index - 1].Distance(points[index]);
    }
    if (cumulative.back() <= 1.0e-14) return {};
    std::vector<gp_Pnt> result;
    result.reserve(requestedCount);
    for (std::size_t sample = 0; sample < requestedCount; ++sample) {
        const auto distance = cumulative.back() *
            static_cast<double>(sample) /
            static_cast<double>(requestedCount - 1);
        const auto upper = std::lower_bound(
            cumulative.begin(), cumulative.end(), distance);
        if (upper == cumulative.begin()) {
            result.push_back(points.front());
            continue;
        }
        if (upper == cumulative.end()) {
            result.push_back(points.back());
            continue;
        }
        const auto second = static_cast<std::size_t>(
            std::distance(cumulative.begin(), upper));
        const auto first = second - 1;
        const auto span = cumulative[second] - cumulative[first];
        const auto alpha = span > 0.0
            ? (distance - cumulative[first]) / span
            : 0.0;
        result.emplace_back(
            (1.0 - alpha) * points[first].X() +
                alpha * points[second].X(),
            (1.0 - alpha) * points[first].Y() +
                alpha * points[second].Y(),
            (1.0 - alpha) * points[first].Z() +
                alpha * points[second].Z());
    }
    result.front() = points.front();
    result.back() = points.back();
    return result;
}

Handle(Geom_BSplineCurve) regularizedCubicCurve(
    const std::vector<gp_Pnt>& points,
    const std::vector<double>& uniqueKnots,
    const double fairingWeight) {
    if (points.size() < 2 || uniqueKnots.size() < 2 ||
        uniqueKnots.front() != 0.0 || uniqueKnots.back() != 1.0) {
        return {};
    }
    const auto controlCount =
        static_cast<int>(uniqueKnots.size()) + kInternalCurveDegree - 1;
    if (controlCount < kInternalCurveDegree + 1) return {};
    const auto unknownCount = controlCount - 2;
    const auto parameters = chordParameters(points, 0.0, 1.0);
    if (parameters.size() != points.size()) return {};
    const auto fairingRowCount = std::max(0, controlCount - 2);
    Eigen::MatrixXd matrix = Eigen::MatrixXd::Zero(
        static_cast<Eigen::Index>(points.size() + fairingRowCount),
        unknownCount);
    Eigen::MatrixXd targets = Eigen::MatrixXd::Zero(
        matrix.rows(), 3);
    for (std::size_t sample = 0; sample < points.size(); ++sample) {
        const auto basis = curveBasisValues(
            uniqueKnots, parameters[sample]);
        Eigen::Vector3d target(
            points[sample].X(),
            points[sample].Y(),
            points[sample].Z());
        target -= basis.front() * Eigen::Vector3d(
            points.front().X(),
            points.front().Y(),
            points.front().Z());
        target -= basis.back() * Eigen::Vector3d(
            points.back().X(),
            points.back().Y(),
            points.back().Z());
        for (int control = 1; control + 1 < controlCount; ++control) {
            matrix(
                static_cast<Eigen::Index>(sample),
                control - 1) =
                basis[static_cast<std::size_t>(control)];
        }
        targets.row(static_cast<Eigen::Index>(sample)) =
            target.transpose();
    }
    const auto fairnessScale = std::sqrt(std::max(
        1.0e-12,
        fairingWeight * static_cast<double>(points.size()) /
            static_cast<double>(std::max(1, fairingRowCount))));
    for (int center = 1; center + 1 < controlCount; ++center) {
        const auto row = static_cast<Eigen::Index>(
            points.size() + static_cast<std::size_t>(center - 1));
        Eigen::Vector3d target = Eigen::Vector3d::Zero();
        const std::array<std::pair<int, double>, 3> coefficients {{
            {center - 1, 1.0},
            {center, -2.0},
            {center + 1, 1.0}}};
        for (const auto& [control, coefficient] : coefficients) {
            if (control == 0) {
                target -= fairnessScale * coefficient *
                    Eigen::Vector3d(
                        points.front().X(),
                        points.front().Y(),
                        points.front().Z());
            } else if (control + 1 == controlCount) {
                target -= fairnessScale * coefficient *
                    Eigen::Vector3d(
                        points.back().X(),
                        points.back().Y(),
                        points.back().Z());
            } else {
                matrix(row, control - 1) +=
                    fairnessScale * coefficient;
            }
        }
        targets.row(row) = target.transpose();
    }
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd> solver(matrix);
    if (solver.rank() < unknownCount) return {};
    const auto solution = solver.solve(targets);
    if (!solution.allFinite()) return {};
    TColgp_Array1OfPnt poles(1, controlCount);
    poles.SetValue(1, points.front());
    poles.SetValue(controlCount, points.back());
    for (int control = 1; control + 1 < controlCount; ++control) {
        poles.SetValue(
            control + 1,
            gp_Pnt(
                solution(control - 1, 0),
                solution(control - 1, 1),
                solution(control - 1, 2)));
    }
    TColStd_Array1OfReal knots(
        1, static_cast<int>(uniqueKnots.size()));
    TColStd_Array1OfInteger multiplicities(
        1, static_cast<int>(uniqueKnots.size()));
    for (std::size_t index = 0; index < uniqueKnots.size(); ++index) {
        knots.SetValue(static_cast<int>(index + 1), uniqueKnots[index]);
        multiplicities.SetValue(
            static_cast<int>(index + 1),
            index == 0 || index + 1 == uniqueKnots.size()
                ? kInternalCurveDegree + 1
                : 1);
    }
    try {
        return new Geom_BSplineCurve(
            poles,
            knots,
            multiplicities,
            kInternalCurveDegree,
            Standard_False);
    } catch (const Standard_Failure&) {
        return {};
    }
}

Handle(Geom_BSplineCurve) diagnosticPolyline(
    const std::vector<gp_Pnt>& points) {
    if (points.size() < 2) return {};
    TColgp_Array1OfPnt poles(1, static_cast<int>(points.size()));
    TColStd_Array1OfReal knots(1, static_cast<int>(points.size()));
    TColStd_Array1OfInteger multiplicities(
        1, static_cast<int>(points.size()));
    std::vector<double> parameters(points.size(), 0.0);
    for (std::size_t index = 1; index < points.size(); ++index) {
        parameters[index] = parameters[index - 1] +
            points[index - 1].Distance(points[index]);
    }
    if (parameters.back() <= 1.0e-14) return {};
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto arrayIndex = static_cast<int>(index + 1);
        poles.SetValue(arrayIndex, points[index]);
        knots.SetValue(arrayIndex, parameters[index]);
        multiplicities.SetValue(
            arrayIndex,
            index == 0 || index + 1 == points.size() ? 2 : 1);
    }
    try {
        return new Geom_BSplineCurve(
            poles, knots, multiplicities, 1, Standard_False);
    } catch (const Standard_Failure&) {
        return {};
    }
}

struct InternalCurveValidation {
    bool projected = false;
    double maximumProjectionError = 0.0;
    double maximumCarrierPathDeviation = 0.0;
    double maximumSampleTurnDegrees = 0.0;
    double bendingEnergy = 0.0;
    int selfIntersectionCount = 0;
    int sampleCount = 0;
    int worstSample = 0;
    double worstParameter = 0.0;
    std::size_t worstCarrierSample = 0;
    gp_Pnt worstCurvePoint;
    gp_Pnt worstProjectedPoint;
};

struct ProjectedCarrierPoint {
    std::size_t vertex = 0;
    gp_Pnt point;
};

double polylineTurnDegrees(
    const gp_Pnt& previous,
    const gp_Pnt& current,
    const gp_Pnt& next) {
    gp_Vec incoming(previous, current);
    gp_Vec outgoing(current, next);
    if (incoming.SquareMagnitude() <= 1.0e-24 ||
        outgoing.SquareMagnitude() <= 1.0e-24) {
        return 0.0;
    }
    incoming.Normalize();
    outgoing.Normalize();
    return std::acos(std::clamp(
        incoming.Dot(outgoing), -1.0, 1.0)) *
        180.0 / 3.14159265358979323846;
}

void removeExcessiveCarrierTurns(
    std::vector<ProjectedCarrierPoint>& points,
    const double maximumTurnDegrees) {
    if (points.size() < 3 || maximumTurnDegrees <= 0.0) return;
    bool changed = true;
    while (changed && points.size() > 2) {
        changed = false;
        for (std::size_t index = 1; index + 1 < points.size(); ++index) {
            if (polylineTurnDegrees(
                    points[index - 1].point,
                    points[index].point,
                    points[index + 1].point) <= maximumTurnDegrees) {
                continue;
            }
            points.erase(points.begin() + static_cast<std::ptrdiff_t>(index));
            changed = true;
            break;
        }
    }
}

double segmentSegmentDistance(
    const gp_Pnt& firstStart,
    const gp_Pnt& firstEnd,
    const gp_Pnt& secondStart,
    const gp_Pnt& secondEnd) {
    const gp_Vec firstDirection(firstStart, firstEnd);
    const gp_Vec secondDirection(secondStart, secondEnd);
    const gp_Vec offset(secondStart, firstStart);
    const auto firstSquared = firstDirection.SquareMagnitude();
    const auto secondSquared = secondDirection.SquareMagnitude();
    const auto dot = firstDirection.Dot(secondDirection);
    const auto firstOffset = firstDirection.Dot(offset);
    const auto secondOffset = secondDirection.Dot(offset);
    const auto denominator =
        firstSquared * secondSquared - dot * dot;
    auto firstParameter = 0.0;
    auto secondParameter = 0.0;
    if (firstSquared <= 1.0e-24 && secondSquared <= 1.0e-24) {
        return firstStart.Distance(secondStart);
    }
    if (firstSquared <= 1.0e-24) {
        secondParameter = std::clamp(
            secondOffset / secondSquared, 0.0, 1.0);
    } else if (secondSquared <= 1.0e-24) {
        firstParameter = std::clamp(
            -firstOffset / firstSquared, 0.0, 1.0);
    } else {
        if (std::abs(denominator) > 1.0e-24) {
            firstParameter = std::clamp(
                (dot * secondOffset - secondSquared * firstOffset) /
                    denominator,
                0.0,
                1.0);
        }
        secondParameter =
            (dot * firstParameter + secondOffset) / secondSquared;
        if (secondParameter < 0.0) {
            secondParameter = 0.0;
            firstParameter = std::clamp(
                -firstOffset / firstSquared, 0.0, 1.0);
        } else if (secondParameter > 1.0) {
            secondParameter = 1.0;
            firstParameter = std::clamp(
                (dot - firstOffset) / firstSquared, 0.0, 1.0);
        }
    }
    const auto firstPoint = firstStart.Translated(
        firstDirection * firstParameter);
    const auto secondPoint = secondStart.Translated(
        secondDirection * secondParameter);
    return firstPoint.Distance(secondPoint);
}

InternalCurveValidation validateInternalCurve(
    const Handle(Geom_Curve)& curve,
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const RegionRefitOptions& options,
    const std::vector<std::size_t>& vertices,
    const std::vector<gp_Pnt>& carrierPoints,
    const int sampleCount) {
    InternalCurveValidation result;
    if (curve.IsNull() || vertices.size() != carrierPoints.size()) return result;
    const auto parameters = chordParameters(
        carrierPoints, curve->FirstParameter(), curve->LastParameter());
    if (parameters.size() != vertices.size()) return result;
    result.projected = true;
    result.sampleCount = std::max(2, sampleCount);
    result.worstParameter = curve->FirstParameter();
    result.worstCurvePoint = curve->Value(result.worstParameter);
    result.worstProjectedPoint = result.worstCurvePoint;
    std::vector<gp_Pnt> sampledPoints;
    std::vector<gp_Vec> sampledTangents;
    sampledPoints.reserve(static_cast<std::size_t>(result.sampleCount));
    sampledTangents.reserve(static_cast<std::size_t>(result.sampleCount));
    for (int sample = 0; sample < result.sampleCount; ++sample) {
        const auto alpha = static_cast<double>(sample) /
            static_cast<double>(result.sampleCount - 1);
        const auto parameter = curve->FirstParameter() +
            alpha * (curve->LastParameter() - curve->FirstParameter());
        gp_Pnt point;
        gp_Vec tangent;
        try {
            curve->D1(parameter, point, tangent);
        } catch (const Standard_Failure&) {
            result.projected = false;
            return result;
        }
        if (!finitePoint(point) ||
            tangent.SquareMagnitude() <= 1.0e-24) {
            result.projected = false;
            return result;
        }
        tangent.Normalize();
        sampledPoints.push_back(point);
        sampledTangents.push_back(tangent);
        if (sample > 0) {
            const auto angle = std::acos(std::clamp(
                sampledTangents[static_cast<std::size_t>(sample - 1)].Dot(
                    tangent),
                -1.0,
                1.0));
            result.maximumSampleTurnDegrees = std::max(
                result.maximumSampleTurnDegrees,
                angle * 180.0 / 3.14159265358979323846);
            result.bendingEnergy += angle * angle;
        }
        const auto lower = std::lower_bound(
            parameters.begin(), parameters.end(), parameter);
        auto carrierSample = lower == parameters.end()
            ? parameters.size() - 1
            : static_cast<std::size_t>(
                std::distance(parameters.begin(), lower));
        if (carrierSample > 0 &&
            std::abs(parameters[carrierSample - 1] - parameter) <
                std::abs(parameters[carrierSample] - parameter)) {
            --carrierSample;
        }
        gp_Pnt projected;
        double distance = 0.0;
        if (!projectToOriginalRegion(
                point,
                constraints,
                projected,
                distance,
                options.curveProjectionTolerance,
                preferredFace(mesh.vertices[vertices[carrierSample]]))) {
            result.projected = false;
            return result;
        }
        if (distance > result.maximumProjectionError) {
            result.maximumProjectionError = distance;
            result.worstSample = sample;
            result.worstParameter = parameter;
            result.worstCarrierSample = carrierSample;
            result.worstCurvePoint = point;
            result.worstProjectedPoint = projected;
        }
    }
    for (std::size_t sample = 0; sample < parameters.size(); ++sample) {
        result.maximumCarrierPathDeviation = std::max(
            result.maximumCarrierPathDeviation,
            curve->Value(parameters[sample]).Distance(
                carrierPoints[sample]));
    }
    const auto intersectionTolerance = std::max(
        1.0e-9, options.curveProjectionTolerance * 1.0e-3);
    for (std::size_t first = 0; first + 1 < sampledPoints.size(); ++first) {
        for (std::size_t second = first + 2;
             second + 1 < sampledPoints.size();
             ++second) {
            if (segmentSegmentDistance(
                    sampledPoints[first],
                    sampledPoints[first + 1],
                    sampledPoints[second],
                    sampledPoints[second + 1]) <=
                intersectionTolerance) {
                ++result.selfIntersectionCount;
            }
        }
    }
    return result;
}

bool makeInternalCurve(
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const RegionRefitOptions& options,
    const std::vector<std::size_t>& vertices,
    const gp_Pnt& startPoint,
    const gp_Pnt& endPoint,
    Handle(Geom_Curve)& curve,
    std::vector<double>& meshParameters,
    double& maximumProjectionError,
    RegionInternalCurveConstructionMode& constructionMode,
    bool& productQualityPassed,
    int& validationSampleCount,
    int& controlPointCount,
    double& maximumSampleTurnDegrees,
    double& bendingEnergy,
    int& selfIntersectionCount,
    std::string& diagnostic,
    std::string& message) {
    if (vertices.size() < 2) {
        message = "An internal curve needs at least two carrier vertices.";
        return false;
    }
    std::vector<gp_Pnt> points;
    points.reserve(vertices.size());
    double maximumCarrierProjectionDistance = 0.0;
    for (const auto vertex : vertices) {
        gp_Pnt projected;
        double distance = 0.0;
        if (!projectToOriginalRegion(
                mesh.vertices[vertex].point,
                constraints,
                projected,
                distance,
                options.curveProjectionTolerance,
                preferredFace(mesh.vertices[vertex]))) {
            message = "An internal curve sample could not be projected to the original STEP region.";
            return false;
        }
        maximumCarrierProjectionDistance =
            std::max(maximumCarrierProjectionDistance, distance);
        points.push_back(projected);
    }
    points.front() = startPoint;
    points.back() = endPoint;
    std::vector<ProjectedCarrierPoint> projectedCarrier;
    projectedCarrier.reserve(points.size());
    const auto duplicateTolerance = std::max(
        1.0e-10, options.curveProjectionTolerance * 1.0e-6);
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (projectedCarrier.empty() ||
            projectedCarrier.back().point.Distance(points[index]) >
                duplicateTolerance) {
            projectedCarrier.push_back({vertices[index], points[index]});
        }
    }
    if (projectedCarrier.size() < 2 ||
        projectedCarrier.front().point.Distance(
            projectedCarrier.back().point) <=
            duplicateTolerance) {
        message =
            "The projected internal interface collapsed to a degenerate curve.";
        return false;
    }
    constexpr std::size_t maximumInterpolationPointCount = 129;
    if (projectedCarrier.size() > maximumInterpolationPointCount) {
        std::vector<ProjectedCarrierPoint> reduced;
        reduced.reserve(maximumInterpolationPointCount);
        for (std::size_t index = 0;
             index < maximumInterpolationPointCount; ++index) {
            const auto source = index * (projectedCarrier.size() - 1) /
                (maximumInterpolationPointCount - 1);
            if (reduced.empty() ||
                reduced.back().point.Distance(
                    projectedCarrier[source].point) >
                    duplicateTolerance) {
                reduced.push_back(projectedCarrier[source]);
            }
        }
        if (reduced.back().point.Distance(projectedCarrier.back().point) >
            duplicateTolerance) {
            reduced.push_back(projectedCarrier.back());
        }
        projectedCarrier = std::move(reduced);
    }
    double maximumProjectedInputGap = 0.0;
    for (std::size_t index = 1; index < projectedCarrier.size(); ++index) {
        maximumProjectedInputGap = std::max(
            maximumProjectedInputGap,
            projectedCarrier[index - 1].point.Distance(
                projectedCarrier[index].point));
    }
    const auto originalProjectedCarrier = projectedCarrier;
    removeExcessiveCarrierTurns(
        projectedCarrier,
        options.maximumInternalCurveSampleTurnDegrees);
    if (projectedCarrier.size() < 2 ||
        projectedCarrier.front().point.Distance(
            projectedCarrier.back().point) <= duplicateTolerance) {
        message =
            "The projected internal interface collapsed while removing local reversals.";
        return false;
    }
    std::vector<std::size_t> validationVertices;
    std::vector<gp_Pnt> validationPoints;
    validationVertices.reserve(projectedCarrier.size());
    validationPoints.reserve(projectedCarrier.size());
    for (const auto& item : projectedCarrier) {
        validationVertices.push_back(item.vertex);
        validationPoints.push_back(item.point);
    }
    auto fitPoints = resampleByArcLength(
        validationPoints,
        std::min<std::size_t>(
            maximumInterpolationPointCount,
            std::max<std::size_t>(17, validationPoints.size())));
    if (fitPoints.size() < 2) {
        message = "The projected internal interface could not be resampled.";
        return false;
    }
    InternalCurveValidation selectedValidation;
    auto selectedProjectedInputCount = validationPoints.size();
    std::vector<double> uniqueKnots {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0};
    auto selectedControlPointCount = 0;
    while (options.curveFairingIterations > 0 &&
           static_cast<int>(uniqueKnots.size()) +
                   kInternalCurveDegree - 1 <=
               options.maximumControlPointsPerSide) {
        const auto candidate = regularizedCubicCurve(
            fitPoints, uniqueKnots, options.curveFairingWeight);
        if (candidate.IsNull()) break;
        const auto sampleCount = std::max(
            options.curveValidationSampleCount,
            8 * std::max(1, candidate->NbKnots() - 1) + 1);
        const auto validation = validateInternalCurve(
            candidate,
            mesh,
            constraints,
            options,
            validationVertices,
            validationPoints,
            sampleCount);
        if (validation.projected &&
            validation.maximumProjectionError <=
                options.curveProjectionTolerance &&
            validation.maximumCarrierPathDeviation <= options.tolerance3d &&
            validation.maximumSampleTurnDegrees <=
                options.maximumInternalCurveSampleTurnDegrees &&
            validation.selfIntersectionCount == 0 &&
            candidate->Continuity() >= GeomAbs_C2) {
            curve = candidate;
            selectedValidation = validation;
            constructionMode =
                RegionInternalCurveConstructionMode::RegularizedCubic;
            productQualityPassed = true;
            selectedControlPointCount = candidate->NbPoles();
            break;
        }
        if (!selectedValidation.projected ||
            std::max(
                validation.maximumProjectionError,
                validation.maximumCarrierPathDeviation) <
                std::max(
                    selectedValidation.maximumProjectionError,
                    selectedValidation.maximumCarrierPathDeviation)) {
            selectedValidation = validation;
        }
        if (static_cast<int>(uniqueKnots.size()) +
                kInternalCurveDegree - 1 >=
            options.maximumControlPointsPerSide) {
            break;
        }
        const auto parameters = chordParameters(fitPoints, 0.0, 1.0);
        auto worstParameter = 0.5;
        auto worstDistance = -1.0;
        for (std::size_t sample = 1;
             sample + 1 < fitPoints.size();
             ++sample) {
            const auto distance = candidate->Value(parameters[sample]).Distance(
                fitPoints[sample]);
            if (distance > worstDistance) {
                worstDistance = distance;
                worstParameter = parameters[sample];
            }
        }
        if (std::any_of(
                uniqueKnots.begin(),
                uniqueKnots.end(),
                [&](const auto knot) {
                    return std::abs(knot - worstParameter) < 0.01;
                })) {
            auto largestSpan = 0.0;
            for (std::size_t knot = 0;
                 knot + 1 < uniqueKnots.size();
                 ++knot) {
                const auto span =
                    uniqueKnots[knot + 1] - uniqueKnots[knot];
                if (span > largestSpan) {
                    largestSpan = span;
                    worstParameter =
                        0.5 * (uniqueKnots[knot] + uniqueKnots[knot + 1]);
                }
            }
        }
        uniqueKnots.push_back(worstParameter);
        std::sort(uniqueKnots.begin(), uniqueKnots.end());
    }
    if (curve.IsNull()) {
        auto candidate = diagnosticPolyline(validationPoints);
        const auto sampleCount = std::max(
            options.curveValidationSampleCount,
            static_cast<int>(8 * (validationPoints.size() - 1) + 1));
        auto validation = validateInternalCurve(
            candidate,
            mesh,
            constraints,
            options,
            validationVertices,
            validationPoints,
            sampleCount);
        if (candidate.IsNull() || !validation.projected ||
            validation.maximumProjectionError >
                options.curveProjectionTolerance) {
            std::vector<std::size_t> originalValidationVertices;
            std::vector<gp_Pnt> originalValidationPoints;
            originalValidationVertices.reserve(
                originalProjectedCarrier.size());
            originalValidationPoints.reserve(
                originalProjectedCarrier.size());
            for (const auto& item : originalProjectedCarrier) {
                originalValidationVertices.push_back(item.vertex);
                originalValidationPoints.push_back(item.point);
            }
            candidate = diagnosticPolyline(originalValidationPoints);
            validation = validateInternalCurve(
                candidate,
                mesh,
                constraints,
                options,
                originalValidationVertices,
                originalValidationPoints,
                std::max(
                    options.curveValidationSampleCount,
                    static_cast<int>(
                        8 * (originalValidationPoints.size() - 1) + 1)));
            selectedProjectedInputCount =
                originalValidationPoints.size();
        }
        if (candidate.IsNull() || !validation.projected ||
            validation.maximumProjectionError >
                options.curveProjectionTolerance) {
            message =
                "Neither the bounded local cubic nor the diagnostic polyline stayed inside the original STEP projection corridor.";
            return false;
        }
        curve = candidate;
        selectedValidation = validation;
        constructionMode =
            RegionInternalCurveConstructionMode::DiagnosticPolyline;
        productQualityPassed = false;
        selectedControlPointCount =
            Handle(Geom_BSplineCurve)::DownCast(candidate)->NbPoles();
    }
    meshParameters = chordParameters(
        points, curve->FirstParameter(), curve->LastParameter());
    if (meshParameters.size() != vertices.size()) {
        message = "The frozen internal curve has a degenerate parameter range.";
        return false;
    }
    maximumProjectionError = selectedValidation.maximumProjectionError;
    validationSampleCount = selectedValidation.sampleCount;
    controlPointCount = selectedControlPointCount;
    maximumSampleTurnDegrees =
        selectedValidation.maximumSampleTurnDegrees;
    bendingEnergy = selectedValidation.bendingEnergy;
    selfIntersectionCount = selectedValidation.selfIntersectionCount;
    std::ostringstream diagnosticStream;
    diagnosticStream
        << "fit_method=" << toString(constructionMode)
        << "; product_quality_passed=" << productQualityPassed
        << "; control_point_count=" << controlPointCount
        << "; projected_input_count=" << selectedProjectedInputCount
        << "; original_projected_input_count="
        << originalProjectedCarrier.size()
        << "; max_projected_input_gap=" << std::setprecision(12)
        << maximumProjectedInputGap
        << "; max_carrier_projection_distance="
        << maximumCarrierProjectionDistance
        << "; max_carrier_path_deviation="
        << selectedValidation.maximumCarrierPathDeviation
        << "; max_sample_turn_degrees="
        << maximumSampleTurnDegrees
        << "; bending_energy=" << bendingEnergy
        << "; self_intersection_count=" << selfIntersectionCount
        << "; validation_sample_count=" << validationSampleCount
        << "; validation_sample=" << selectedValidation.worstSample
        << "; validation_parameter=" << selectedValidation.worstParameter
        << "; carrier_sample=" << selectedValidation.worstCarrierSample
        << "; curve_point=("
        << selectedValidation.worstCurvePoint.X() << ","
        << selectedValidation.worstCurvePoint.Y() << ","
        << selectedValidation.worstCurvePoint.Z() << ")"
        << "; projected_point=("
        << selectedValidation.worstProjectedPoint.X() << ","
        << selectedValidation.worstProjectedPoint.Y() << ","
        << selectedValidation.worstProjectedPoint.Z() << ")";
    diagnostic = diagnosticStream.str();
    return true;
}

RegionCurveContinuity boundaryContinuity(
    const RegionBoundaryEdgeConstraint& constraint) {
    return constraint.continuity == RegionBoundaryContinuity::Smooth
        ? RegionCurveContinuity::G1
        : RegionCurveContinuity::G0;
}

std::string stableHash(const RegionCurveNetwork& network) {
    std::uint64_t hash = 1469598103934665603ull;
    auto append = [&](const std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    append(static_cast<std::uint64_t>(network.patchCount));
    for (const auto patch : network.trianglePatches) {
        append(static_cast<std::uint64_t>(patch));
    }
    for (const auto& node : network.nodes) {
        append(node.id);
        append(node.meshVertex);
        append(static_cast<std::uint64_t>(std::llround(node.point.X() * 1.0e12)));
        append(static_cast<std::uint64_t>(std::llround(node.point.Y() * 1.0e12)));
        append(static_cast<std::uint64_t>(std::llround(node.point.Z() * 1.0e12)));
    }
    for (const auto& edge : network.edges) {
        append(edge.id);
        append(edge.startNode);
        append(edge.endNode);
        append(static_cast<std::uint64_t>(edge.firstPatch + 1));
        append(static_cast<std::uint64_t>(edge.secondPatch + 1));
        append(static_cast<std::uint64_t>(edge.requestedContinuity));
        append(static_cast<std::uint64_t>(edge.continuity));
        append(static_cast<std::uint64_t>(edge.constructionMode));
        append(edge.productQualityPassed);
        append(static_cast<std::uint64_t>(edge.controlPointCount));
        append(static_cast<std::uint64_t>(
            std::llround(edge.maximumSampleTurnDegrees * 1.0e9)));
        append(static_cast<std::uint64_t>(
            std::llround(edge.bendingEnergy * 1.0e12)));
        append(static_cast<std::uint64_t>(edge.selfIntersectionCount));
        append(edge.originalBoundary);
        append(edge.hasOriginalEdge ? edge.originalEdgeId : 0);
        for (const auto vertex : edge.meshVertices) append(vertex);
        constexpr int hashSamples = 17;
        for (int sample = 0; sample < hashSamples; ++sample) {
            const auto alpha = static_cast<double>(sample) /
                static_cast<double>(hashSamples - 1);
            const auto parameter = edge.firstParameter +
                alpha * (edge.lastParameter - edge.firstParameter);
            const auto point = edge.curve->Value(parameter);
            append(static_cast<std::uint64_t>(std::llround(point.X() * 1.0e12)));
            append(static_cast<std::uint64_t>(std::llround(point.Y() * 1.0e12)));
            append(static_cast<std::uint64_t>(std::llround(point.Z() * 1.0e12)));
        }
    }
    for (const auto& loop : network.patchLoops) {
        append(static_cast<std::uint64_t>(loop.patch + 1));
        for (const auto& use : loop.edges) {
            append(use.edgeIndex);
            append(use.forward);
        }
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

bool orientPatchLoops(RegionCurveNetwork& network, std::string& message) {
    struct Use {
        int patch = -1;
        bool forward = true;
    };
    std::vector<std::vector<Use>> uses(network.edges.size());
    for (const auto& loop : network.patchLoops) {
        for (const auto& use : loop.edges) {
            if (use.edgeIndex >= uses.size()) {
                message = "A patch loop references a missing curve edge.";
                return false;
            }
            uses[use.edgeIndex].push_back({loop.patch, use.forward});
        }
    }
    std::vector<int> flips(static_cast<std::size_t>(network.patchCount), -1);
    for (int root = 0; root < network.patchCount; ++root) {
        if (flips[static_cast<std::size_t>(root)] >= 0) continue;
        flips[static_cast<std::size_t>(root)] = 0;
        std::queue<int> pending;
        pending.push(root);
        while (!pending.empty()) {
            const auto patch = pending.front();
            pending.pop();
            for (std::size_t edgeIndex = 0;
                 edgeIndex < network.edges.size();
                 ++edgeIndex) {
                const auto& edge = network.edges[edgeIndex];
                if (edge.originalBoundary || uses[edgeIndex].size() != 2) {
                    continue;
                }
                const auto& first = uses[edgeIndex][0];
                const auto& second = uses[edgeIndex][1];
                const auto* current = first.patch == patch ? &first
                    : second.patch == patch ? &second : nullptr;
                const auto* adjacent = first.patch == patch ? &second
                    : second.patch == patch ? &first : nullptr;
                if (current == nullptr || adjacent == nullptr) continue;
                const auto requiredAdjacentFlip =
                    flips[static_cast<std::size_t>(patch)] ^
                    (current->forward == adjacent->forward ? 1 : 0);
                auto& adjacentFlip =
                    flips[static_cast<std::size_t>(adjacent->patch)];
                if (adjacentFlip < 0) {
                    adjacentFlip = requiredAdjacentFlip;
                    pending.push(adjacent->patch);
                } else if (adjacentFlip != requiredAdjacentFlip) {
                    message = "Patch loops cannot be oriented consistently across shared curves.";
                    return false;
                }
            }
        }
    }
    for (auto& loop : network.patchLoops) {
        if (loop.patch < 0 || loop.patch >= network.patchCount) return false;
        if (flips[static_cast<std::size_t>(loop.patch)] == 0) continue;
        std::reverse(loop.edges.begin(), loop.edges.end());
        for (auto& use : loop.edges) use.forward = !use.forward;
    }
    return true;
}

bool splitBoundaryCurvesForFourSides(
    RegionCurveNetwork& network,
    std::string& message) {
    for (auto& loop : network.patchLoops) {
        while (loop.edges.size() < 4) {
            auto selectedOrder = loop.edges.size();
            auto selectedLength = -1.0;
            for (std::size_t order = 0; order < loop.edges.size(); ++order) {
                const auto edgeIndex = loop.edges[order].edgeIndex;
                if (edgeIndex >= network.edges.size()) continue;
                const auto& edge = network.edges[edgeIndex];
                if (!edge.originalBoundary ||
                    edge.meshVertices.size() < 3 ||
                    edge.meshParameters.size() != edge.meshVertices.size()) {
                    continue;
                }
                const auto length = edge.lastParameter - edge.firstParameter;
                if (length > selectedLength) {
                    selectedLength = length;
                    selectedOrder = order;
                }
            }
            if (selectedOrder >= loop.edges.size()) {
                message =
                    "A patch loop has fewer than four sides and no splittable original boundary edge.";
                return false;
            }
            const auto selectedUse = loop.edges[selectedOrder];
            const auto originalIndex = selectedUse.edgeIndex;
            auto& original = network.edges[originalIndex];
            const auto midpoint =
                (original.firstParameter + original.lastParameter) * 0.5;
            auto splitOffset = std::size_t {1};
            auto splitDistance = std::numeric_limits<double>::infinity();
            for (std::size_t offset = 1;
                 offset + 1 < original.meshParameters.size(); ++offset) {
                const auto distance =
                    std::abs(original.meshParameters[offset] - midpoint);
                if (distance < splitDistance) {
                    splitDistance = distance;
                    splitOffset = offset;
                }
            }
            const auto splitParameter =
                original.meshParameters[splitOffset];
            if (!(splitParameter > original.firstParameter) ||
                !(splitParameter < original.lastParameter)) {
                message =
                    "A selected original boundary edge has no interior split parameter.";
                return false;
            }
            auto node = std::find_if(
                network.nodes.begin(),
                network.nodes.end(),
                [&](const auto& candidate) {
                    return candidate.meshVertex ==
                        original.meshVertices[splitOffset];
                });
            std::size_t splitNode = 0;
            if (node == network.nodes.end()) {
                RegionCurveNetworkNode created;
                created.id = network.nodes.size();
                created.meshVertex = original.meshVertices[splitOffset];
                created.point = original.curve->Value(splitParameter);
                created.originalBoundary = true;
                splitNode = created.id;
                network.nodes.push_back(std::move(created));
            } else {
                splitNode = node->id;
            }

            auto second = original;
            const auto originalEndNode = original.endNode;
            second.id = network.edges.size();
            second.startNode = splitNode;
            second.endNode = originalEndNode;
            second.firstParameter = splitParameter;
            second.meshVertices.assign(
                original.meshVertices.begin() +
                    static_cast<std::ptrdiff_t>(splitOffset),
                original.meshVertices.end());
            second.meshParameters.assign(
                original.meshParameters.begin() +
                    static_cast<std::ptrdiff_t>(splitOffset),
                original.meshParameters.end());
            second.reusesOriginalEdge = false;
            second.splitsOriginalEdge = true;

            original.endNode = splitNode;
            original.lastParameter = splitParameter;
            original.meshVertices.erase(
                original.meshVertices.begin() +
                    static_cast<std::ptrdiff_t>(splitOffset + 1),
                original.meshVertices.end());
            original.meshParameters.erase(
                original.meshParameters.begin() +
                    static_cast<std::ptrdiff_t>(splitOffset + 1),
                original.meshParameters.end());
            original.reusesOriginalEdge = false;
            original.splitsOriginalEdge = true;
            network.edges.push_back(std::move(second));

            const auto secondIndex = network.edges.size() - 1;
            const std::array<RegionCurveNetworkEdgeUse, 2> replacements =
                selectedUse.forward
                ? std::array<RegionCurveNetworkEdgeUse, 2> {{
                    {originalIndex, true}, {secondIndex, true}}}
                : std::array<RegionCurveNetworkEdgeUse, 2> {{
                    {secondIndex, false}, {originalIndex, false}}};
            loop.edges.erase(
                loop.edges.begin() +
                static_cast<std::ptrdiff_t>(selectedOrder));
            loop.edges.insert(
                loop.edges.begin() +
                    static_cast<std::ptrdiff_t>(selectedOrder),
                replacements.begin(),
                replacements.end());
        }
    }
    for (auto& node : network.nodes) node.incidentEdges.clear();
    for (std::size_t index = 0; index < network.edges.size(); ++index) {
        auto& edge = network.edges[index];
        edge.id = index;
        if (edge.startNode >= network.nodes.size() ||
            edge.endNode >= network.nodes.size()) {
            message = "A split curve edge references a missing network node.";
            return false;
        }
        network.nodes[edge.startNode].incidentEdges.push_back(index);
        network.nodes[edge.endNode].incidentEdges.push_back(index);
    }
    return true;
}

bool buildNetwork(
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const RegionRefitOptions& options,
    const RegionPatchLayout& layout,
    RegionCurveNetwork& network,
    std::string& message) {
    network.patchCount = layout.patchCount;
    network.trianglePatches = layout.trianglePatches;
    network.partitionLayoutHash = layout.hash;
    std::map<std::size_t, std::size_t> nodeByMeshVertex;
    std::map<MeshEdge, std::pair<std::size_t, bool>> edgeUseByMeshEdge;
    double worstInternalProjectionError = -1.0;
    std::size_t worstInternalEdgeId = 0;
    std::string worstInternalProjectionDiagnostic;

    std::map<std::pair<EdgeId, int>, std::vector<MeshEdge>> outerByOwner;
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        const auto& loop = layout.patches[patch].boundaryLoop;
        for (std::size_t position = 0; position < loop.size(); ++position) {
            const auto first = loop[position];
            const auto second = loop[(position + 1) % loop.size()];
            const auto edgeId = commonBoundaryEdgeId(
                mesh.vertices[first], mesh.vertices[second]);
            if (edgeId.has_value()) {
                outerByOwner[{*edgeId, static_cast<int>(patch)}].push_back(
                    orderedEdge(first, second));
            }
        }
    }
    for (const auto& [owner, meshEdges] : outerByOwner) {
        bool valid = false;
        auto vertices = orderedChain(meshEdges, valid);
        const auto* constraint = constraintFor(constraints, owner.first);
        if (!valid || constraint == nullptr || constraint->originalEdge.IsNull()) {
            message = "An original boundary ownership chain is invalid.";
            return false;
        }
        double originalFirst = 0.0;
        double originalLast = 0.0;
        const auto curve = BRep_Tool::Curve(
            constraint->originalEdge, originalFirst, originalLast);
        double firstParameter = 0.0;
        double lastParameter = 0.0;
        if (curve.IsNull() ||
            !curveParameter(
                curve, mesh.vertices[vertices.front()].point,
                originalFirst, originalLast, firstParameter) ||
            !curveParameter(
                curve, mesh.vertices[vertices.back()].point,
                originalFirst, originalLast, lastParameter)) {
            message = "An original boundary chain has no recoverable curve range.";
            return false;
        }
        if (lastParameter < firstParameter) {
            std::reverse(vertices.begin(), vertices.end());
            std::swap(firstParameter, lastParameter);
        }
        std::vector<gp_Pnt> points;
        std::vector<double> parameters;
        points.reserve(vertices.size());
        parameters.reserve(vertices.size());
        for (const auto vertex : vertices) {
            double parameter = 0.0;
            if (!curveParameter(
                    curve, mesh.vertices[vertex].point,
                    originalFirst, originalLast, parameter)) {
                message = "An original boundary sample could not be parameterized.";
                return false;
            }
            points.push_back(curve->Value(parameter));
            parameters.push_back(parameter);
        }
        if (parameters.size() != vertices.size()) {
            message = "An original boundary segment is degenerate.";
            return false;
        }
        const auto start = nodeFor(
            network, nodeByMeshVertex, vertices.front(),
            curve->Value(firstParameter), true);
        const auto end = nodeFor(
            network, nodeByMeshVertex, vertices.back(),
            curve->Value(lastParameter), true);
        const auto parameterTolerance = std::max(
            1.0e-10, std::abs(originalLast - originalFirst) * 1.0e-9);
        const auto fullRange =
            std::abs(firstParameter - std::min(originalFirst, originalLast)) <=
                parameterTolerance &&
            std::abs(lastParameter - std::max(originalFirst, originalLast)) <=
                parameterTolerance;
        RegionCurveNetworkEdge edge;
        edge.id = network.edges.size();
        edge.startNode = start;
        edge.endNode = end;
        edge.firstPatch = owner.second;
        edge.continuity = boundaryContinuity(*constraint);
        edge.requestedContinuity = edge.continuity;
        edge.originalBoundary = true;
        edge.hasOriginalEdge = true;
        edge.originalEdgeId = owner.first;
        edge.originalEdge = constraint->originalEdge;
        edge.curve = curve;
        edge.firstParameter = firstParameter;
        edge.lastParameter = lastParameter;
        edge.meshVertices = vertices;
        edge.meshParameters = std::move(parameters);
        edge.reusesOriginalEdge = fullRange;
        edge.splitsOriginalEdge = !fullRange;
        edge.frozen = true;
        edge.validationSampleCount =
            options.curveValidationSampleCount;
        network.nodes[start].incidentEdges.push_back(edge.id);
        network.nodes[end].incidentEdges.push_back(edge.id);
        network.edges.push_back(std::move(edge));
        const auto edgeIndex = network.edges.size() - 1;
        for (std::size_t index = 0; index + 1 < vertices.size(); ++index) {
            edgeUseByMeshEdge[orderedEdge(vertices[index], vertices[index + 1])] = {
                edgeIndex, vertices[index] == network.edges[edgeIndex].meshVertices[index]};
        }
    }

    for (const auto& seam : layout.seams) {
        bool valid = false;
        auto vertices = orderedChain(seam.meshEdges, valid);
        if (!valid) {
            message = "An internal partition interface is open, branched, or cyclic.";
            return false;
        }
        gp_Pnt firstProjected;
        gp_Pnt lastProjected;
        double firstDistance = 0.0;
        double lastDistance = 0.0;
        if (!projectToOriginalRegion(
                mesh.vertices[vertices.front()].point,
                constraints,
                firstProjected,
                firstDistance,
                options.curveProjectionTolerance,
                preferredFace(mesh.vertices[vertices.front()])) ||
            !projectToOriginalRegion(
                mesh.vertices[vertices.back()].point,
                constraints,
                lastProjected,
                lastDistance,
                options.curveProjectionTolerance,
                preferredFace(mesh.vertices[vertices.back()]))) {
            message = "An internal curve endpoint could not be projected.";
            return false;
        }
        const auto start = nodeFor(
            network, nodeByMeshVertex, vertices.front(),
            firstProjected, mesh.vertices[vertices.front()].boundary);
        const auto end = nodeFor(
            network, nodeByMeshVertex, vertices.back(),
            lastProjected, mesh.vertices[vertices.back()].boundary);
        Handle(Geom_Curve) curve;
        std::vector<double> parameters;
        double maximumProjectionError = 0.0;
        RegionInternalCurveConstructionMode constructionMode =
            RegionInternalCurveConstructionMode::RegularizedCubic;
        bool productQualityPassed = false;
        int validationSampleCount = 0;
        int controlPointCount = 0;
        double maximumSampleTurnDegrees = 0.0;
        double bendingEnergy = 0.0;
        int selfIntersectionCount = 0;
        std::string curveDiagnostic;
        if (!makeInternalCurve(
                mesh, constraints, options, vertices,
                network.nodes[start].point, network.nodes[end].point,
                curve, parameters, maximumProjectionError,
                constructionMode, productQualityPassed,
                validationSampleCount,
                controlPointCount,
                maximumSampleTurnDegrees,
                bendingEnergy,
                selfIntersectionCount,
                curveDiagnostic, message)) {
            return false;
        }
        RegionCurveNetworkEdge edge;
        edge.id = network.edges.size();
        edge.startNode = start;
        edge.endNode = end;
        edge.firstPatch = seam.firstPatch;
        edge.secondPatch = seam.secondPatch;
        edge.requestedContinuity =
            internalContinuity(mesh, vertices, options);
        edge.continuity = productQualityPassed
            ? edge.requestedContinuity
            : RegionCurveContinuity::G0;
        edge.constructionMode = constructionMode;
        edge.productQualityPassed = productQualityPassed;
        edge.validationSampleCount = validationSampleCount;
        edge.controlPointCount = controlPointCount;
        edge.maximumSampleTurnDegrees = maximumSampleTurnDegrees;
        edge.bendingEnergy = bendingEnergy;
        edge.selfIntersectionCount = selfIntersectionCount;
        edge.curve = curve;
        edge.firstParameter = curve->FirstParameter();
        edge.lastParameter = curve->LastParameter();
        edge.meshVertices = vertices;
        edge.meshParameters = std::move(parameters);
        edge.frozen = true;
        edge.maximumProjectionError = maximumProjectionError;
        if (maximumProjectionError > worstInternalProjectionError) {
            worstInternalProjectionError = maximumProjectionError;
            worstInternalEdgeId = edge.id;
            std::ostringstream stream;
            stream << curveDiagnostic
                << "; start_mesh_vertex=" << vertices.front()
                << "; end_mesh_vertex=" << vertices.back();
            worstInternalProjectionDiagnostic = stream.str();
        }
        network.nodes[start].incidentEdges.push_back(edge.id);
        network.nodes[end].incidentEdges.push_back(edge.id);
        network.edges.push_back(std::move(edge));
        const auto edgeIndex = network.edges.size() - 1;
        for (std::size_t index = 0; index + 1 < vertices.size(); ++index) {
            edgeUseByMeshEdge[orderedEdge(vertices[index], vertices[index + 1])] = {
                edgeIndex, true};
        }
    }

    network.patchLoops.reserve(layout.patches.size());
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        RegionCurveNetworkPatchLoop curveLoop;
        curveLoop.patch = static_cast<int>(patch);
        const auto& meshLoop = layout.patches[patch].boundaryLoop;
        for (std::size_t position = 0; position < meshLoop.size(); ++position) {
            const auto first = meshLoop[position];
            const auto second = meshLoop[(position + 1) % meshLoop.size()];
            const auto found = edgeUseByMeshEdge.find(orderedEdge(first, second));
            if (found == edgeUseByMeshEdge.end()) {
                message = "A patch loop segment has no frozen curve-network edge.";
                return false;
            }
            const auto& edge = network.edges[found->second.first];
            const auto vertexFound = std::find(
                edge.meshVertices.begin(), edge.meshVertices.end(), first);
            if (vertexFound == edge.meshVertices.end()) {
                message = "A patch loop segment is not owned by its curve-network edge.";
                return false;
            }
            const auto vertexIndex = static_cast<std::size_t>(
                std::distance(edge.meshVertices.begin(), vertexFound));
            const auto forward = vertexIndex + 1 < edge.meshVertices.size() &&
                edge.meshVertices[vertexIndex + 1] == second;
            const auto backward = vertexIndex > 0 &&
                edge.meshVertices[vertexIndex - 1] == second;
            if (!forward && !backward) {
                message = "A patch loop curve direction is inconsistent.";
                return false;
            }
            if (curveLoop.edges.empty() ||
                curveLoop.edges.back().edgeIndex != found->second.first) {
                curveLoop.edges.push_back({found->second.first, forward});
            } else if (curveLoop.edges.back().forward != forward) {
                message = "A patch loop reverses direction inside one frozen curve.";
                return false;
            }
        }
        if (curveLoop.edges.size() > 1 &&
            curveLoop.edges.front().edgeIndex == curveLoop.edges.back().edgeIndex) {
            if (curveLoop.edges.front().forward != curveLoop.edges.back().forward) {
                message = "A patch loop has inconsistent wrap-around curve direction.";
                return false;
            }
            curveLoop.edges.pop_back();
        }
        network.patchLoops.push_back(std::move(curveLoop));
    }
    if (!splitBoundaryCurvesForFourSides(network, message) ||
        !orientPatchLoops(network, message)) {
        return false;
    }
    const auto valid = validateRegionCurveNetwork(
        network,
        options.curveProjectionTolerance,
        options.curveValidationSampleCount,
        message);
    if (!valid && !network.report.projectionPassed &&
        !worstInternalProjectionDiagnostic.empty()) {
        std::ostringstream stream;
        stream << message
            << " Worst internal curve edge " << worstInternalEdgeId
            << ": " << worstInternalProjectionDiagnostic << ".";
        message = stream.str();
    }
    return valid;
}

}

const char* toString(const RegionCurveContinuity continuity) {
    switch (continuity) {
    case RegionCurveContinuity::G0: return "G0";
    case RegionCurveContinuity::G1: return "G1";
    case RegionCurveContinuity::G2: return "G2";
    }
    return "Unknown";
}

const char* toString(
    const RegionInternalCurveConstructionMode mode) {
    switch (mode) {
    case RegionInternalCurveConstructionMode::RegularizedCubic:
        return "RegularizedCubic";
    case RegionInternalCurveConstructionMode::DiagnosticPolyline:
        return "DiagnosticPolyline";
    }
    return "Unknown";
}

bool RegionCurveNetwork::valid() const {
    return report.finitePassed && report.closurePassed &&
        report.ownershipPassed && report.projectionPassed &&
        report.orientationPassed && report.intersectionPassed &&
        report.nonManifoldEdgePassed && report.frozen && !hash.empty();
}

bool RegionCurveNetworkGenerationResult::success() const {
    return status == RegionRefitStatus::Success && !selectedCandidates.empty();
}

bool validateRegionCurveNetwork(
    RegionCurveNetwork& network,
    const double projectionTolerance,
    const int validationSampleCount,
    std::string& message) {
    message.clear();
    network.report = {};
    network.report.nodeCount = network.nodes.size();
    network.report.edgeCount = network.edges.size();
    if (projectionTolerance <= 0.0 || validationSampleCount < 2 ||
        network.patchCount <= 0 ||
        network.patchLoops.size() != static_cast<std::size_t>(network.patchCount)) {
        message = "Curve-network validation received inconsistent input.";
        network.report.rejectionReason = message;
        return false;
    }
    network.report.finitePassed = std::all_of(
        network.nodes.begin(), network.nodes.end(),
        [](const auto& node) { return finitePoint(node.point); });
    std::vector<int> edgeUseCount(network.edges.size(), 0);
    std::vector<std::vector<bool>> edgeUseDirections(network.edges.size());
    network.report.closurePassed = true;
    for (const auto& loop : network.patchLoops) {
        if (loop.patch < 0 || loop.patch >= network.patchCount ||
            loop.edges.size() < 2) {
            network.report.closurePassed = false;
            break;
        }
        for (std::size_t index = 0; index < loop.edges.size(); ++index) {
            const auto& use = loop.edges[index];
            const auto& nextUse = loop.edges[(index + 1) % loop.edges.size()];
            if (use.edgeIndex >= network.edges.size() ||
                nextUse.edgeIndex >= network.edges.size()) {
                network.report.closurePassed = false;
                break;
            }
            const auto& edge = network.edges[use.edgeIndex];
            const auto& next = network.edges[nextUse.edgeIndex];
            const auto endNode = use.forward ? edge.endNode : edge.startNode;
            const auto nextStart = nextUse.forward ? next.startNode : next.endNode;
            if (endNode != nextStart) {
                network.report.closurePassed = false;
                break;
            }
            ++edgeUseCount[use.edgeIndex];
            edgeUseDirections[use.edgeIndex].push_back(use.forward);
        }
    }
    network.report.ownershipPassed = network.report.closurePassed;
    network.report.nonManifoldEdgePassed = true;
    network.report.projectionPassed = true;
    network.report.orientationPassed = network.report.closurePassed;
    network.report.intersectionPassed = true;
    network.report.productQualityPassed = true;
    network.report.frozen = true;
    for (std::size_t index = 0; index < network.edges.size(); ++index) {
        auto& edge = network.edges[index];
        if (edge.id != index || edge.curve.IsNull() ||
            edge.startNode >= network.nodes.size() ||
            edge.endNode >= network.nodes.size() ||
            !std::isfinite(edge.firstParameter) ||
            !std::isfinite(edge.lastParameter) ||
            edge.lastParameter <= edge.firstParameter ||
            edge.meshVertices.size() < 2 ||
            edge.meshParameters.size() != edge.meshVertices.size()) {
            network.report.finitePassed = false;
        } else {
            const auto parametersIncreasing = std::adjacent_find(
                edge.meshParameters.begin(),
                edge.meshParameters.end(),
                [](const auto first, const auto second) {
                    return !std::isfinite(first) || !std::isfinite(second) ||
                        second <= first;
                }) == edge.meshParameters.end();
            const auto endpointsMatch =
                edge.curve->Value(edge.firstParameter).Distance(
                    network.nodes[edge.startNode].point) <=
                    projectionTolerance &&
                edge.curve->Value(edge.lastParameter).Distance(
                    network.nodes[edge.endNode].point) <=
                    projectionTolerance;
            network.report.finitePassed = network.report.finitePassed &&
                parametersIncreasing && endpointsMatch;
        }
        const auto expectedUseCount = edge.originalBoundary ? 1 : 2;
        if (edgeUseCount[index] != expectedUseCount) {
            network.report.ownershipPassed = false;
        }
        if (!edge.originalBoundary &&
            (edge.firstPatch < 0 || edge.secondPatch < 0 ||
                edge.firstPatch == edge.secondPatch)) {
            network.report.nonManifoldEdgePassed = false;
        }
        if (edge.originalBoundary) {
            ++network.report.boundaryEdgeCount;
        } else {
            ++network.report.internalEdgeCount;
            switch (edge.constructionMode) {
            case RegionInternalCurveConstructionMode::RegularizedCubic:
                ++network.report.regularizedCubicEdgeCount;
                break;
            case RegionInternalCurveConstructionMode::DiagnosticPolyline:
                ++network.report.diagnosticPolylineEdgeCount;
                break;
            }
            if (edge.requestedContinuity != edge.continuity) {
                ++network.report.continuityDowngradeCount;
            }
            network.report.productQualityPassed =
                network.report.productQualityPassed &&
                edge.productQualityPassed &&
                edge.constructionMode ==
                    RegionInternalCurveConstructionMode::RegularizedCubic &&
                edge.requestedContinuity == edge.continuity;
            network.report.maximumSampleTurnDegrees = std::max(
                network.report.maximumSampleTurnDegrees,
                edge.maximumSampleTurnDegrees);
            network.report.maximumBendingEnergy = std::max(
                network.report.maximumBendingEnergy,
                edge.bendingEnergy);
            network.report.selfIntersectionCount +=
                edge.selfIntersectionCount;
            if (edgeUseDirections[index].size() != 2 ||
                edgeUseDirections[index][0] ==
                    edgeUseDirections[index][1]) {
                network.report.orientationPassed = false;
            }
        }
        switch (edge.continuity) {
        case RegionCurveContinuity::G0: ++network.report.g0EdgeCount; break;
        case RegionCurveContinuity::G1: ++network.report.g1EdgeCount; break;
        case RegionCurveContinuity::G2: ++network.report.g2EdgeCount; break;
        }
        network.report.maximumProjectionError = std::max(
            network.report.maximumProjectionError,
            edge.maximumProjectionError);
        network.report.projectionPassed =
            network.report.projectionPassed &&
            std::isfinite(edge.maximumProjectionError) &&
            edge.maximumProjectionError <= projectionTolerance;
        network.report.frozen = network.report.frozen && edge.frozen;
    }
    network.report.intersectionPassed =
        network.report.selfIntersectionCount == 0;
    const auto intersectionTolerance = std::isfinite(projectionTolerance)
        ? std::max(1.0e-9, projectionTolerance * 1.0e-3)
        : 1.0e-9;
    const auto segmentsShareOnlyNetworkNode = [](
        const RegionCurveNetworkEdge& firstEdge,
        const int firstSegment,
        const RegionCurveNetworkEdge& secondEdge,
        const int secondSegment,
        const int segmentCount) {
        return
            (firstEdge.startNode == secondEdge.startNode &&
                firstSegment == 0 && secondSegment == 0) ||
            (firstEdge.startNode == secondEdge.endNode &&
                firstSegment == 0 && secondSegment + 1 == segmentCount) ||
            (firstEdge.endNode == secondEdge.startNode &&
                firstSegment + 1 == segmentCount && secondSegment == 0) ||
            (firstEdge.endNode == secondEdge.endNode &&
                firstSegment + 1 == segmentCount &&
                secondSegment + 1 == segmentCount);
    };
    for (std::size_t first = 0; first < network.edges.size(); ++first) {
        const auto& firstEdge = network.edges[first];
        if (firstEdge.curve.IsNull()) continue;
        for (std::size_t second = first + 1;
             second < network.edges.size();
             ++second) {
            const auto& secondEdge = network.edges[second];
            if (secondEdge.curve.IsNull() ||
                (firstEdge.originalBoundary && secondEdge.originalBoundary)) {
                continue;
            }
            constexpr int samples = 65;
            constexpr int segmentCount = samples - 1;
            std::array<gp_Pnt, samples> firstPoints;
            std::array<gp_Pnt, samples> secondPoints;
            for (int sample = 0; sample < samples; ++sample) {
                const auto alpha = static_cast<double>(sample) /
                    static_cast<double>(samples - 1);
                firstPoints[static_cast<std::size_t>(sample)] =
                    firstEdge.curve->Value(
                        firstEdge.firstParameter +
                        alpha * (firstEdge.lastParameter -
                            firstEdge.firstParameter));
                secondPoints[static_cast<std::size_t>(sample)] =
                    secondEdge.curve->Value(
                        secondEdge.firstParameter +
                        alpha * (secondEdge.lastParameter -
                            secondEdge.firstParameter));
            }
            auto intersects = false;
            for (int firstSegment = 0;
                 firstSegment + 1 < samples && !intersects;
                 ++firstSegment) {
                for (int secondSegment = 0;
                     secondSegment + 1 < samples;
                     ++secondSegment) {
                    if (segmentsShareOnlyNetworkNode(
                            firstEdge,
                            firstSegment,
                            secondEdge,
                            secondSegment,
                            segmentCount)) {
                        continue;
                    }
                    if (segmentSegmentDistance(
                            firstPoints[static_cast<std::size_t>(
                                firstSegment)],
                            firstPoints[static_cast<std::size_t>(
                                firstSegment + 1)],
                            secondPoints[static_cast<std::size_t>(
                                secondSegment)],
                            secondPoints[static_cast<std::size_t>(
                                secondSegment + 1)]) <=
                        intersectionTolerance) {
                        intersects = true;
                        break;
                    }
                }
            }
            if (intersects) {
                ++network.report.unexpectedCurveIntersectionCount;
                network.report.intersectionPassed = false;
            }
        }
    }
    if (!network.report.finitePassed) message = "Curve network contains non-finite or incomplete geometry.";
    else if (!network.report.closurePassed) message = "Curve network contains an open or misoriented patch loop.";
    else if (!network.report.ownershipPassed) message = "Curve network edge ownership counts are inconsistent.";
    else if (!network.report.orientationPassed) message = "A shared internal curve is used in the same direction by both adjacent patches.";
    else if (!network.report.intersectionPassed) message = "Curve network contains a self-intersection or a non-node curve crossing.";
    else if (!network.report.nonManifoldEdgePassed) message = "Curve network contains an invalid shared-edge adjacency.";
    else if (!network.report.projectionPassed) {
        std::ostringstream stream;
        stream << "Curve network exceeds the original STEP projection tolerance: maximum "
               << network.report.maximumProjectionError
               << ", tolerance " << projectionTolerance << '.';
        message = stream.str();
    }
    else if (!network.report.frozen) message = "Curve network contains an unfrozen edge.";
    if (!message.empty()) {
        network.report.rejectionReason = message;
        return false;
    }
    network.hash = stableHash(network);
    return true;
}

RegionCurveNetworkGenerationResult RegionCurveNetworkGenerator::generate(
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const RegionRefitLayoutMode mode,
    const RegionRefitOptions& options,
    const bool allowDiagnosticCandidates) const {
    RegionCurveNetworkGenerationResult result;
    const auto partitions = RegionPatchLayoutGenerator().generate(
        mesh, mode, options, allowDiagnosticCandidates);
    result.layoutReports = partitions.candidates;
    if (!partitions.success()) {
        result.status = partitions.status;
        result.message = partitions.message;
        return result;
    }
    std::set<std::string> selectedHashes;
    for (const auto& layout : partitions.selectedCandidates) {
        selectedHashes.insert(layout.hash);
    }
    for (const auto& layout : partitions.constructibleCandidates) {
        RegionCurveNetworkCandidate candidate;
        candidate.partitionLayout = layout;
        std::string failure;
        if (!buildNetwork(
                mesh, constraints, options, layout,
                candidate.network, failure)) {
            const auto report = std::find_if(
                result.layoutReports.begin(), result.layoutReports.end(),
                [&](const auto& item) { return item.layoutHash == layout.hash; });
            if (report != result.layoutReports.end()) {
                report->rejectionReason = failure;
            }
            continue;
        }
        if (selectedHashes.contains(layout.hash)) {
            result.selectedCandidates.push_back(candidate);
        }
        result.constructibleCandidates.push_back(std::move(candidate));
    }
    if (result.selectedCandidates.empty()) {
        result.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.message = "No partition candidate produced a frozen, closed curve network.";
        return result;
    }
    result.status = RegionRefitStatus::Success;
    result.message = "Generated and froze deterministic STEP-constrained curve networks.";
    return result;
}

}
