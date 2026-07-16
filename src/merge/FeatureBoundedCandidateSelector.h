#pragma once

#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"

#include <string>
#include <vector>

namespace spo {

struct FeatureBoundedCandidateSelection {
    const MergeCandidate* candidate = nullptr;
    RegionBoundaryAnalysis boundary;
    std::string firstBoundaryFailure;
};

const MergeCandidate* findCandidateById(
    const std::vector<MergeCandidate>& candidates,
    int candidateId);

std::vector<const MergeCandidate*> rankSelectableFeatureBoundedCandidates(
    const std::vector<MergeCandidate>& candidates);

FeatureBoundedCandidateSelection selectLargestValidFeatureBoundedCandidate(
    const ShapeDocument& document,
    const std::vector<MergeCandidate>& candidates);

}
