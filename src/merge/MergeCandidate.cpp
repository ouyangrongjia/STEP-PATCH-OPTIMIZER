#include "merge/MergeCandidate.h"

namespace spo {

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
