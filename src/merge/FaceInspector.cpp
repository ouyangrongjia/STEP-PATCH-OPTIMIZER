#include "merge/FaceInspector.h"

#include "brep/SurfaceTypeProbe.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <Precision.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace spo {

namespace {

bool containsFace(const MergeCandidate& candidate, FaceId faceId) {
    return std::find(candidate.faces.begin(), candidate.faces.end(), faceId) != candidate.faces.end();
}

bool containsFeatureEdge(const FeatureEdgeDetectionResult& featureEdges, EdgeId edgeId) {
    return std::find_if(featureEdges.edges.begin(), featureEdges.edges.end(), [edgeId](const FeatureEdge& edge) {
        return edge.edge == edgeId;
    }) != featureEdges.edges.end();
}

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

bool isSphereLikeSinglePatch(const TopoDS_Face& face) {
    if (face.IsNull()) {
        return false;
    }

    BRepAdaptor_Surface surface(face);
    if (surface.GetType() == GeomAbs_Sphere) {
        return true;
    }
    if (surface.GetType() != GeomAbs_BSplineSurface && surface.GetType() != GeomAbs_BezierSurface) {
        return false;
    }

    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    if (!std::isfinite(uMin) || !std::isfinite(uMax) || !std::isfinite(vMin) || !std::isfinite(vMax)) {
        return false;
    }

    std::vector<gp_Pnt> points;
    for (const auto uFactor : {0.25, 0.5, 0.75}) {
        for (const auto vFactor : {0.25, 0.5, 0.75}) {
            BRepLProp_SLProps props(surface, uMin + (uMax - uMin) * uFactor, vMin + (vMax - vMin) * vFactor, 0, Precision::Confusion());
            points.push_back(props.Value());
        }
    }

    std::array<std::array<double, 4>, 4> normalMatrix {};
    std::array<double, 4> normalRhs {};
    for (const auto& point : points) {
        const std::array<double, 4> row {point.X(), point.Y(), point.Z(), 1.0};
        const auto target = -(point.X() * point.X() + point.Y() * point.Y() + point.Z() * point.Z());
        for (int i = 0; i < 4; ++i) {
            normalRhs[i] += row[i] * target;
            for (int j = 0; j < 4; ++j) {
                normalMatrix[i][j] += row[i] * row[j];
            }
        }
    }

    std::array<double, 4> coefficients {};
    if (!solveLinear4x4(normalMatrix, normalRhs, coefficients)) {
        return false;
    }

    const gp_Pnt center(-0.5 * coefficients[0], -0.5 * coefficients[1], -0.5 * coefficients[2]);
    const auto radiusSquared = center.X() * center.X() + center.Y() * center.Y() + center.Z() * center.Z() - coefficients[3];
    if (radiusSquared <= Precision::Confusion()) {
        return false;
    }
    const auto radius = std::sqrt(radiusSquared);
    double maxResidual = 0.0;
    for (const auto& point : points) {
        maxResidual = std::max(maxResidual, std::abs(center.Distance(point) - radius));
    }
    return maxResidual <= std::max(0.20, radius * 0.06);
}

}

FaceInspectInfo inspectFace(
    const ShapeDocument& document,
    FaceId faceId,
    const std::vector<MergeCandidate>& candidates,
    const std::set<int>& visibleCandidateIds,
    const FeatureEdgeDetectionResult* featureEdges,
    const std::set<EdgeId>& lockedEdges) {
    FaceInspectInfo info;
    info.face_id = faceId;
    if (!document.hasShape() || faceId >= document.topology().faceCount()) {
        return info;
    }

    info.valid = true;
    info.surface_type = surfaceTypeName(document.topology().face(faceId));
    info.sphere_like_single_patch = isSphereLikeSinglePatch(document.topology().face(faceId));

    for (const auto edgeId : document.topology().edgesForFace(faceId)) {
        if (featureEdges != nullptr && containsFeatureEdge(*featureEdges, edgeId)) {
            ++info.adjacent_protected_edge_count;
        }
        if (lockedEdges.contains(edgeId)) {
            ++info.adjacent_locked_edge_count;
        }
    }
    info.adjacent_to_protected_edge = info.adjacent_protected_edge_count > 0;
    info.adjacent_to_locked_edge = info.adjacent_locked_edge_count > 0;

    const MergeCandidate* hiddenMatch = nullptr;
    const MergeCandidate* nonHiddenMatch = nullptr;
    for (const auto& candidate : candidates) {
        if (!containsFace(candidate, faceId)) {
            continue;
        }
        if (candidate.status == MergeCandidateStatus::Hidden) {
            if (hiddenMatch == nullptr) {
                hiddenMatch = &candidate;
            }
            continue;
        }
        nonHiddenMatch = &candidate;
        break;
    }

    const auto* match = nonHiddenMatch != nullptr ? nonHiddenMatch : hiddenMatch;
    if (match == nullptr) {
        return info;
    }

    info.candidate_id = match->candidate_id;
    info.candidate_type = match->candidate_type;
    info.candidate_status = match->status;
    info.risk_level = match->risk_level;
    info.candidate_face_count = match->face_count;
    info.candidate_boundary_edge_count = match->boundary_edge_count;
    info.candidate_internal_edge_count = match->internal_edge_count;
    info.max_normal_angle_deg = match->max_normal_angle_deg;
    info.max_distance = match->max_distance;

    if (match->status == MergeCandidateStatus::Hidden) {
        info.candidate_state = FaceInspectCandidateState::InHiddenCandidate;
    } else if (visibleCandidateIds.contains(match->candidate_id)) {
        info.candidate_state = FaceInspectCandidateState::InVisibleCandidate;
    } else {
        info.candidate_state = FaceInspectCandidateState::InCandidateButNotDisplayed;
    }
    return info;
}

}
