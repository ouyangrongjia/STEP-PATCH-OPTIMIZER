#pragma once

#include "merge/MergeCandidate.h"

#include <vector>

namespace spo {

struct CandidateTypeCounts {
    int plane_like = 0;
    int cylinder_like = 0;
    int sphere_like = 0;
    int cone_like = 0;
    int torus_like = 0;
    int feature_bounded_refit = 0;
    int freeform_g1 = 0;
    int freeform_g2 = 0;
    int unknown = 0;
};

CandidateTypeCounts countCandidateTypes(const std::vector<MergeCandidate>& candidates);
std::vector<MergeCandidate> filterCandidatesByType(
    const std::vector<MergeCandidate>& candidates,
    MergeCandidateType type,
    bool includeHidden = false);
std::vector<MergeCandidate> filterNonHiddenCandidates(const std::vector<MergeCandidate>& candidates);
std::vector<MergeCandidate> filterMergeablePlaneCandidates(const std::vector<MergeCandidate>& candidates);
std::vector<MergeCandidate> filterMergeableSphereCandidates(const std::vector<MergeCandidate>& candidates);

}
