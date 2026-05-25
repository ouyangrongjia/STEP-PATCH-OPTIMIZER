#include "merge/FaceInspector.h"

#include "brep/SurfaceTypeProbe.h"

#include <algorithm>

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
