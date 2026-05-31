#pragma once

#include "merge/MergeCandidate.h"

#include <set>
#include <vector>

namespace spo {

class ShapeDocument;

class FeatureBoundedRegionBuilder {
public:
    std::vector<MergeCandidate> build(
        const ShapeDocument& document,
        const std::set<EdgeId>& protectedEdges,
        int minRegionFaces = 2,
        int* visitedFaces = nullptr,
        int* rejectedRegions = nullptr) const;
};

}
