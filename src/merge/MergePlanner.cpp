#include "merge/MergePlanner.h"

#include "brep/ShapeDocument.h"
#include "merge/MergeRegionGrower.h"

#include <set>

namespace spo {

MergePlannerResult MergePlanner::plan(
    const ShapeDocument& document,
    const FeatureEdgeDetectionResult& featureEdges,
    const std::set<EdgeId>& lockedEdges,
    const MergePlannerOptions& options) const {
    MergePlannerResult result;
    if (!document.hasShape()) {
        return result;
    }

    std::set<EdgeId> protectedEdges = lockedEdges;
    for (const auto& edge : featureEdges.edges) {
        protectedEdges.insert(edge.edge);
    }
    result.protected_edge_count = static_cast<int>(protectedEdges.size());

    if (options.enable_plane_candidates) {
        MergeRegionGrower grower;
        result.candidates = grower.growPlaneLikeRegions(
            document,
            protectedEdges,
            options,
            &result.visited_faces,
            &result.rejected_regions);
    }

    return result;
}

}
