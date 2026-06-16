#pragma once

#include "feature/FeatureEdgeDetector.h"
#include "merge/MergeCandidate.h"

#include <set>
#include <vector>

namespace spo {

class ShapeDocument;

struct MergePlannerOptions {
    int min_feature_bounded_region_faces = 2;

    bool enable_feature_bounded_refit_candidates = true;
};

struct MergePlannerResult {
    std::vector<MergeCandidate> candidates;
    int visited_faces = 0;
    int rejected_regions = 0;
    int protected_edge_count = 0;
};

class MergePlanner {
public:
    MergePlannerResult plan(
        const ShapeDocument& document,
        const FeatureEdgeDetectionResult& featureEdges,
        const std::set<EdgeId>& lockedEdges,
        const MergePlannerOptions& options) const;
};

}
