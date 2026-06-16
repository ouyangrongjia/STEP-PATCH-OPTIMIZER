#pragma once

namespace spo {

enum class BoundaryAnalysisFailureReason {
    None,
    NotSupported,
    InvalidCandidate,
    BoundaryLoopInvalid,
    MultipleOuterLoopsNotSupported,
    InnerLoopsNotSupported
};

inline const char* boundaryAnalysisFailureReasonToString(BoundaryAnalysisFailureReason reason) {
    switch (reason) {
    case BoundaryAnalysisFailureReason::None:
        return "None";
    case BoundaryAnalysisFailureReason::NotSupported:
        return "NotSupported";
    case BoundaryAnalysisFailureReason::InvalidCandidate:
        return "InvalidCandidate";
    case BoundaryAnalysisFailureReason::BoundaryLoopInvalid:
        return "BoundaryLoopInvalid";
    case BoundaryAnalysisFailureReason::MultipleOuterLoopsNotSupported:
        return "MultipleOuterLoopsNotSupported";
    case BoundaryAnalysisFailureReason::InnerLoopsNotSupported:
        return "InnerLoopsNotSupported";
    }
    return "Unknown";
}

}
