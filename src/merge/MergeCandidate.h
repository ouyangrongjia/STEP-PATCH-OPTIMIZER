#pragma once

#include "common/GeometryTypes.h"

#include <string>
#include <vector>

namespace spo {

enum class MergeCandidateType {
    SameDomain,
    PlaneLike,
    CylinderLike,
    ConeLike,
    SphereLike,
    TorusLike,
    FeatureBoundedRefit,
    FreeformG1,
    FreeformG2,
    Unknown
};

enum class MergeRiskLevel {
    Low,
    Medium,
    High
};

enum class MergeCandidateStatus {
    Pending,
    Accepted,
    Rejected,
    Hidden
};

const char* toString(MergeCandidateStatus status);
const char* toString(MergeCandidateType type);

struct MergeCandidate {
    int candidate_id = -1;
    MergeCandidateType candidate_type = MergeCandidateType::Unknown;
    MergeRiskLevel risk_level = MergeRiskLevel::Low;
    MergeCandidateStatus status = MergeCandidateStatus::Pending;

    std::vector<FaceId> faces;
    std::vector<EdgeId> internal_edges;
    std::vector<EdgeId> boundary_edges;
    std::vector<EdgeId> blocked_edges;
    std::vector<EdgeId> protected_edges;

    double total_area = 0.0;
    int face_count = 0;
    int internal_edge_count = 0;
    int boundary_edge_count = 0;

    double fit_error = 0.0;
    double max_distance = 0.0;
    double mean_distance = 0.0;
    double max_normal_angle_deg = 0.0;
    double mean_normal_angle_deg = 0.0;
    double curvature_deviation = 0.0;

    bool valid = true;
    std::string reject_reason;
};

}
