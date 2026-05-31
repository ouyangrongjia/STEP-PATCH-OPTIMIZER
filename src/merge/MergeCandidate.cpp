#include "merge/MergeCandidate.h"

namespace spo {

const char* toString(MergeCandidateType type) {
    switch (type) {
    case MergeCandidateType::SameDomain:
        return "SameDomain";
    case MergeCandidateType::PlaneLike:
        return "PlaneLike";
    case MergeCandidateType::CylinderLike:
        return "CylinderLike";
    case MergeCandidateType::ConeLike:
        return "ConeLike";
    case MergeCandidateType::SphereLike:
        return "SphereLike";
    case MergeCandidateType::TorusLike:
        return "TorusLike";
    case MergeCandidateType::FeatureBoundedRefit:
        return "FeatureBoundedRefit";
    case MergeCandidateType::FreeformG1:
        return "FreeformG1";
    case MergeCandidateType::FreeformG2:
        return "FreeformG2";
    case MergeCandidateType::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

const char* toString(MergeCandidateStatus status) {
    switch (status) {
    case MergeCandidateStatus::Pending:
        return "Pending";
    case MergeCandidateStatus::Accepted:
        return "Accepted";
    case MergeCandidateStatus::Rejected:
        return "Rejected";
    case MergeCandidateStatus::Hidden:
        return "Hidden";
    }
    return "Unknown";
}

}
