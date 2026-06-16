#include "merge/MergeCandidate.h"

namespace spo {

const char* toString(MergeCandidateType type) {
    switch (type) {
    case MergeCandidateType::SameDomain:
        return "SameDomain";
    case MergeCandidateType::FeatureBoundedRefit:
        return "FeatureBoundedRefit";
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
