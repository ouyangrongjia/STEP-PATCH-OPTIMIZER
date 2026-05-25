#pragma once

#include "merge/MergeCandidate.h"

#include <string>

namespace spo {

enum class RegionMergeFailureReason {
    None,
    NotImplemented,
    NotSupported,
    CandidateNotFound,
    InvalidCandidate,
    UnsupportedCandidateType,
    RejectedCandidate,
    HiddenCandidate,
    InsufficientFaces,
    ProtectedEdgeConflict,
    LockedEdgeConflict,
    BoundaryLoopInvalid,
    MultipleOuterLoopsNotSupported,
    InnerLoopsNotSupported,
    PrimitiveFitFailed,
    DeviationTooLarge,
    SurfaceConstructionFailed,
    TopologyReplacementFailed,
    SewingFailed,
    ValidationFailed
};

struct RegionMergeResult {
    bool success = false;
    RegionMergeFailureReason failure_reason = RegionMergeFailureReason::None;
    std::string message;

    int candidate_id = -1;
    MergeCandidateType candidate_type = MergeCandidateType::Unknown;

    int face_count_before = 0;
    int face_count_after = 0;
    int edge_count_before = 0;
    int edge_count_after = 0;

    double face_reduction_ratio = 0.0;
    double edge_reduction_ratio = 0.0;

    double max_deviation = 0.0;
    double mean_deviation = 0.0;
    double rms_deviation = 0.0;
};

}
