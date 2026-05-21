#pragma once

#include "merge/MergeCandidate.h"
#include "merge/MergePlanner.h"

#include <set>
#include <vector>

namespace spo {

class ShapeDocument;

class MergeRegionGrower {
public:
    std::vector<MergeCandidate> growPlaneLikeRegions(
        const ShapeDocument& document,
        const std::set<EdgeId>& protectedEdges,
        const MergePlannerOptions& options,
        int* visitedFaces,
        int* rejectedRegions) const;
};

}
