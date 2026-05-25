#pragma once

#include "common/GeometryTypes.h"
#include "merge/MergeCandidate.h"

#include <string>

namespace spo {

enum class FaceInspectCandidateState {
    InVisibleCandidate,
    InHiddenCandidate,
    InCandidateButNotDisplayed,
    NotInCandidate
};

struct FaceInspectInfo {
    bool valid = false;
    FaceId face_id = 0;
    std::string surface_type = "Unknown";

    FaceInspectCandidateState candidate_state = FaceInspectCandidateState::NotInCandidate;

    int candidate_id = -1;
    MergeCandidateType candidate_type = MergeCandidateType::Unknown;
    MergeCandidateStatus candidate_status = MergeCandidateStatus::Pending;
    MergeRiskLevel risk_level = MergeRiskLevel::Low;

    int candidate_face_count = 0;
    int candidate_boundary_edge_count = 0;
    int candidate_internal_edge_count = 0;

    bool adjacent_to_protected_edge = false;
    bool adjacent_to_locked_edge = false;
    int adjacent_protected_edge_count = 0;
    int adjacent_locked_edge_count = 0;

    double max_normal_angle_deg = 0.0;
    double max_distance = 0.0;
};

const char* toString(FaceInspectCandidateState state);

}
