#include "merge/CandidateFilters.h"

namespace spo {

CandidateTypeCounts countCandidateTypes(const std::vector<MergeCandidate>& candidates) {
    CandidateTypeCounts counts;
    for (const auto& candidate : candidates) {
        switch (candidate.candidate_type) {
        case MergeCandidateType::FeatureBoundedRefit:
            ++counts.feature_bounded_refit;
            break;
        case MergeCandidateType::SameDomain:
        case MergeCandidateType::Unknown:
            ++counts.unknown;
            break;
        }
    }
    return counts;
}

std::vector<MergeCandidate> filterCandidatesByType(
    const std::vector<MergeCandidate>& candidates,
    MergeCandidateType type,
    bool includeHidden) {
    std::vector<MergeCandidate> result;
    for (const auto& candidate : candidates) {
        if (candidate.candidate_type == type &&
            (includeHidden || candidate.status != MergeCandidateStatus::Hidden)) {
            result.push_back(candidate);
        }
    }
    return result;
}

std::vector<MergeCandidate> filterNonHiddenCandidates(const std::vector<MergeCandidate>& candidates) {
    std::vector<MergeCandidate> result;
    for (const auto& candidate : candidates) {
        if (candidate.status != MergeCandidateStatus::Hidden) {
            result.push_back(candidate);
        }
    }
    return result;
}

}
