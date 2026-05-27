#pragma once

#include "brep/ShapeDocument.h"
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
    ValidationFailed,
    ExportRoundtripFailed,
    ApproximateSurfaceNotSupported
};

inline const char* regionMergeFailureReasonToString(RegionMergeFailureReason reason) {
    switch (reason) {
    case RegionMergeFailureReason::None:
        return "None";
    case RegionMergeFailureReason::NotImplemented:
        return "NotImplemented";
    case RegionMergeFailureReason::NotSupported:
        return "NotSupported";
    case RegionMergeFailureReason::CandidateNotFound:
        return "CandidateNotFound";
    case RegionMergeFailureReason::InvalidCandidate:
        return "InvalidCandidate";
    case RegionMergeFailureReason::UnsupportedCandidateType:
        return "UnsupportedCandidateType";
    case RegionMergeFailureReason::RejectedCandidate:
        return "RejectedCandidate";
    case RegionMergeFailureReason::HiddenCandidate:
        return "HiddenCandidate";
    case RegionMergeFailureReason::InsufficientFaces:
        return "InsufficientFaces";
    case RegionMergeFailureReason::ProtectedEdgeConflict:
        return "ProtectedEdgeConflict";
    case RegionMergeFailureReason::LockedEdgeConflict:
        return "LockedEdgeConflict";
    case RegionMergeFailureReason::BoundaryLoopInvalid:
        return "BoundaryLoopInvalid";
    case RegionMergeFailureReason::MultipleOuterLoopsNotSupported:
        return "MultipleOuterLoopsNotSupported";
    case RegionMergeFailureReason::InnerLoopsNotSupported:
        return "InnerLoopsNotSupported";
    case RegionMergeFailureReason::PrimitiveFitFailed:
        return "PrimitiveFitFailed";
    case RegionMergeFailureReason::DeviationTooLarge:
        return "DeviationTooLarge";
    case RegionMergeFailureReason::SurfaceConstructionFailed:
        return "SurfaceConstructionFailed";
    case RegionMergeFailureReason::TopologyReplacementFailed:
        return "TopologyReplacementFailed";
    case RegionMergeFailureReason::SewingFailed:
        return "SewingFailed";
    case RegionMergeFailureReason::ValidationFailed:
        return "ValidationFailed";
    case RegionMergeFailureReason::ExportRoundtripFailed:
        return "ExportRoundtripFailed";
    case RegionMergeFailureReason::ApproximateSurfaceNotSupported:
        return "ApproximateSurfaceNotSupported";
    }
    return "Unknown";
}

struct RegionMergeResult {
    bool success = false;
    RegionMergeFailureReason failure_reason = RegionMergeFailureReason::None;
    std::string message;
    std::string diagnostic_report;

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

    double plane_normal_x = 0.0;
    double plane_normal_y = 0.0;
    double plane_normal_z = 0.0;

    double primitive_center_x = 0.0;
    double primitive_center_y = 0.0;
    double primitive_center_z = 0.0;

    double primitive_axis_x = 0.0;
    double primitive_axis_y = 0.0;
    double primitive_axis_z = 0.0;

    double primitive_radius = 0.0;
    double primitive_secondary_radius = 0.0;

    double primitive_angle_degrees = 0.0;
    double primitive_fit_error = 0.0;

    bool brep_check_valid = false;
    ShapeDocument document;
};

}
