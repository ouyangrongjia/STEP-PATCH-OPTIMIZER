#include "merge/FeatureBoundedRegionBuilder.h"

#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"

#include <algorithm>
#include <queue>
#include <set>

namespace spo {

namespace {

bool allAdjacentFacesInRegion(const EdgeAdjacency& adjacency, const std::set<FaceId>& regionFaces) {
    if (adjacency.faces.size() != 2) {
        return false;
    }

    return std::all_of(adjacency.faces.begin(), adjacency.faces.end(), [&regionFaces](FaceId face) {
        return regionFaces.contains(face);
    });
}

}

std::vector<MergeCandidate> FeatureBoundedRegionBuilder::build(
    const ShapeDocument& document,
    const std::set<EdgeId>& protectedEdges,
    int minRegionFaces) const {
    std::vector<MergeCandidate> candidates;
    if (!document.hasShape()) {
        return candidates;
    }

    const auto& topology = document.topology();
    std::vector<bool> visited(topology.faceCount(), false);
    const auto minFaces = std::max(1, minRegionFaces);

    for (FaceId seed = 0; seed < topology.faceCount(); ++seed) {
        if (visited[seed]) {
            continue;
        }

        std::queue<FaceId> queue;
        std::set<FaceId> regionFaces;
        visited[seed] = true;
        queue.push(seed);
        regionFaces.insert(seed);

        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();

            for (const auto edge : topology.edgesForFace(current)) {
                const auto* adjacency = topology.adjacencyForEdge(edge);
                if (adjacency == nullptr || adjacency->faces.size() != 2 || protectedEdges.contains(edge)) {
                    continue;
                }

                for (const auto next : adjacency->faces) {
                    if (next == current || visited[next]) {
                        continue;
                    }
                    visited[next] = true;
                    queue.push(next);
                    regionFaces.insert(next);
                }
            }
        }

        if (static_cast<int>(regionFaces.size()) < minFaces) {
            continue;
        }

        std::set<EdgeId> internalEdges;
        std::set<EdgeId> boundaryEdges;
        std::set<EdgeId> protectedBoundaryEdges;

        for (const auto face : regionFaces) {
            for (const auto edge : topology.edgesForFace(face)) {
                const auto* adjacency = topology.adjacencyForEdge(edge);
                const bool isProtected = protectedEdges.contains(edge);
                if (adjacency != nullptr && !isProtected && allAdjacentFacesInRegion(*adjacency, regionFaces)) {
                    internalEdges.insert(edge);
                    continue;
                }

                boundaryEdges.insert(edge);
                if (isProtected) {
                    protectedBoundaryEdges.insert(edge);
                }
            }
        }

        MergeCandidate candidate;
        candidate.candidate_id = static_cast<int>(candidates.size());
        candidate.candidate_type = MergeCandidateType::FeatureBoundedRefit;
        candidate.faces.assign(regionFaces.begin(), regionFaces.end());
        candidate.internal_edges.assign(internalEdges.begin(), internalEdges.end());
        candidate.boundary_edges.assign(boundaryEdges.begin(), boundaryEdges.end());
        candidate.protected_edges.assign(protectedBoundaryEdges.begin(), protectedBoundaryEdges.end());
        candidate.blocked_edges = candidate.protected_edges;
        candidate.face_count = static_cast<int>(candidate.faces.size());
        candidate.internal_edge_count = static_cast<int>(candidate.internal_edges.size());
        candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());
        candidates.push_back(std::move(candidate));
    }

    return candidates;
}

}
