#include "merge/MergePlanner.h"

#include "brep/ShapeDocument.h"
#include "merge/MergeRegionGrower.h"

#include <set>
#include <iterator>

namespace spo {

namespace {

void appendCandidates(
    std::vector<MergeCandidate>& target,
    std::vector<MergeCandidate> source,
    int* visitedFaces,
    int* rejectedRegions,
    int& totalVisitedFaces,
    int& totalRejectedRegions) {
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
    if (visitedFaces != nullptr) {
        totalVisitedFaces += *visitedFaces;
    }
    if (rejectedRegions != nullptr) {
        totalRejectedRegions += *rejectedRegions;
    }
}

void renumberCandidates(std::vector<MergeCandidate>& candidates) {
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        candidates[index].candidate_id = static_cast<int>(index);
    }
}

}

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

    MergeRegionGrower grower;
    if (options.enable_plane_candidates) {
        int visited = 0;
        int rejected = 0;
        appendCandidates(
            result.candidates,
            grower.growPlaneLikeRegions(
                document,
                protectedEdges,
                options,
                &visited,
                &rejected),
            &visited,
            &rejected,
            result.visited_faces,
            result.rejected_regions);
    }
    if (options.enable_cylinder_candidates) {
        int visited = 0;
        int rejected = 0;
        appendCandidates(
            result.candidates,
            grower.growCylinderLikeRegions(
                document,
                protectedEdges,
                options,
                &visited,
                &rejected),
            &visited,
            &rejected,
            result.visited_faces,
            result.rejected_regions);
    }
    if (options.enable_sphere_candidates) {
        int visited = 0;
        int rejected = 0;
        appendCandidates(
            result.candidates,
            grower.growSphereLikeRegions(
                document,
                protectedEdges,
                options,
                &visited,
                &rejected),
            &visited,
            &rejected,
            result.visited_faces,
            result.rejected_regions);
    }
    if (options.enable_cone_candidates) {
        int visited = 0;
        int rejected = 0;
        appendCandidates(
            result.candidates,
            grower.growConeLikeRegions(
                document,
                protectedEdges,
                options,
                &visited,
                &rejected),
            &visited,
            &rejected,
            result.visited_faces,
            result.rejected_regions);
    }
    if (options.enable_torus_candidates) {
        int visited = 0;
        int rejected = 0;
        appendCandidates(
            result.candidates,
            grower.growTorusLikeRegions(
            document,
            protectedEdges,
            options,
                &visited,
                &rejected),
            &visited,
            &rejected,
            result.visited_faces,
            result.rejected_regions);
    }

    renumberCandidates(result.candidates);
    return result;
}

}
