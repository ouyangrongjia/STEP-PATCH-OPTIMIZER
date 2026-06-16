#pragma once

#include "merge/MergeCandidate.h"

#include <vector>

namespace spo {

struct CandidateTypeCounts {
    int feature_bounded_refit = 0;
    int unknown = 0;
};

CandidateTypeCounts countCandidateTypes(const std::vector<MergeCandidate>& candidates);
std::vector<MergeCandidate> filterCandidatesByType(
    const std::vector<MergeCandidate>& candidates,
    MergeCandidateType type,
    bool includeHidden = false);
std::vector<MergeCandidate> filterNonHiddenCandidates(const std::vector<MergeCandidate>& candidates);

}
