#include "merge/RegionBoundaryAnalyzer.h"

#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>

#include <algorithm>
#include <queue>
#include <set>

namespace spo {

namespace {

void fail(RegionBoundaryAnalysis& analysis, RegionMergeFailureReason reason, std::string message) {
    analysis.valid = false;
    analysis.failure_reason = reason;
    analysis.message = std::move(message);
}

bool containsFace(const std::set<FaceId>& faces, FaceId face) {
    return faces.find(face) != faces.end();
}

bool containsEdge(const std::set<EdgeId>& edges, EdgeId edge) {
    return edges.find(edge) != edges.end();
}

int countConnectedFaceComponents(const ShapeDocument& document, const MergeCandidate& candidate) {
    const auto& topology = document.topology();
    const std::set<FaceId> candidateFaces(candidate.faces.begin(), candidate.faces.end());
    const std::set<EdgeId> boundaryEdges(candidate.boundary_edges.begin(), candidate.boundary_edges.end());
    std::set<FaceId> visited;
    int components = 0;

    for (const auto seed : candidateFaces) {
        if (visited.find(seed) != visited.end()) {
            continue;
        }
        ++components;
        std::queue<FaceId> queue;
        queue.push(seed);
        visited.insert(seed);

        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();
            for (const auto edgeId : topology.edgesForFace(current)) {
                if (containsEdge(boundaryEdges, edgeId)) {
                    continue;
                }
                const auto* adjacency = topology.adjacencyForEdge(edgeId);
                if (adjacency == nullptr) {
                    continue;
                }
                for (const auto next : adjacency->faces) {
                    if (!containsFace(candidateFaces, next) || visited.find(next) != visited.end()) {
                        continue;
                    }
                    visited.insert(next);
                    queue.push(next);
                }
            }
        }
    }

    return components;
}

struct BoundaryEdgeInfo {
    EdgeId id = -1;
    TopoDS_Vertex first;
    TopoDS_Vertex second;
};

bool sharesVertex(const BoundaryEdgeInfo& info, const TopoDS_Vertex& vertex) {
    return info.first.IsSame(vertex) || info.second.IsSame(vertex);
}

TopoDS_Vertex otherVertex(const BoundaryEdgeInfo& info, const TopoDS_Vertex& vertex) {
    if (info.first.IsSame(vertex)) {
        return info.second;
    }
    return info.first;
}

std::vector<int> matchingEdges(const std::vector<BoundaryEdgeInfo>& edges, const std::set<int>& remaining, const TopoDS_Vertex& vertex) {
    std::vector<int> matches;
    for (const auto index : remaining) {
        if (sharesVertex(edges[static_cast<std::size_t>(index)], vertex)) {
            matches.push_back(index);
        }
    }
    return matches;
}

bool orderBoundaryLoops(
    const std::vector<BoundaryEdgeInfo>& edges,
    std::vector<std::vector<EdgeId>>& loops,
    std::vector<EdgeId>& orderedEdges,
    bool& hasNonManifoldEdges) {
    std::set<int> remaining;
    for (int index = 0; index < static_cast<int>(edges.size()); ++index) {
        remaining.insert(index);
    }

    while (!remaining.empty()) {
        const auto firstIndex = *remaining.begin();
        remaining.erase(firstIndex);
        const auto& firstEdge = edges[static_cast<std::size_t>(firstIndex)];
        std::vector<EdgeId> loop {firstEdge.id};
        TopoDS_Vertex startVertex = firstEdge.first;
        TopoDS_Vertex currentVertex = firstEdge.second;

        while (!currentVertex.IsSame(startVertex)) {
            const auto matches = matchingEdges(edges, remaining, currentVertex);
            if (matches.empty()) {
                return false;
            }
            if (matches.size() > 1) {
                hasNonManifoldEdges = true;
                return false;
            }

            const auto nextIndex = matches.front();
            remaining.erase(nextIndex);
            const auto& nextEdge = edges[static_cast<std::size_t>(nextIndex)];
            loop.push_back(nextEdge.id);
            currentVertex = otherVertex(nextEdge, currentVertex);
        }

        orderedEdges.insert(orderedEdges.end(), loop.begin(), loop.end());
        loops.push_back(std::move(loop));
    }

    return true;
}

} // namespace

RegionBoundaryAnalysis RegionBoundaryAnalyzer::analyze(const ShapeDocument& document, const MergeCandidate& candidate) const {
    RegionBoundaryAnalysis analysis;
    if (!document.hasShape()) {
        fail(analysis, RegionMergeFailureReason::NotSupported, "Region boundary analysis requires a loaded shape.");
        return analysis;
    }
    if (candidate.boundary_edges.empty()) {
        fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate has no boundary edges.");
        return analysis;
    }

    const auto& topology = document.topology();
    for (const auto faceId : candidate.faces) {
        if (faceId < 0 || static_cast<std::size_t>(faceId) >= topology.faceCount()) {
            fail(analysis, RegionMergeFailureReason::InvalidCandidate, "Candidate references a missing face.");
            return analysis;
        }
    }

    std::vector<BoundaryEdgeInfo> boundaryEdges;
    boundaryEdges.reserve(candidate.boundary_edges.size());
    for (const auto edgeId : candidate.boundary_edges) {
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) {
            fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate references a missing boundary edge.");
            return analysis;
        }
        const auto* adjacency = topology.adjacencyForEdge(edgeId);
        if (adjacency == nullptr || adjacency->faces.size() > 2) {
            analysis.has_non_manifold_edges = true;
            fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate boundary contains a non-manifold edge.");
            return analysis;
        }

        TopoDS_Vertex first;
        TopoDS_Vertex second;
        TopExp::Vertices(topology.edge(edgeId), first, second);
        if (first.IsNull() || second.IsNull()) {
            fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate boundary contains an edge without two vertices.");
            return analysis;
        }
        boundaryEdges.push_back(BoundaryEdgeInfo {edgeId, first, second});
    }

    analysis.connected_component_count = countConnectedFaceComponents(document, candidate);
    if (analysis.connected_component_count != 1) {
        analysis.outer_wire_count = analysis.connected_component_count;
        fail(analysis, RegionMergeFailureReason::MultipleOuterLoopsNotSupported, "Candidate region has multiple disconnected boundary components.");
        return analysis;
    }

    bool hasNonManifoldEdges = false;
    analysis.boundary_closed = orderBoundaryLoops(boundaryEdges, analysis.boundary_loops, analysis.ordered_boundary_edges, hasNonManifoldEdges);
    analysis.has_non_manifold_edges = hasNonManifoldEdges;
    if (analysis.has_non_manifold_edges) {
        fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate boundary has a branching vertex.");
        return analysis;
    }
    if (!analysis.boundary_closed) {
        fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate boundary edges do not form closed loops.");
        return analysis;
    }

    analysis.outer_wire_count = analysis.boundary_loops.empty() ? 0 : 1;
    analysis.inner_wire_count = analysis.boundary_loops.size() > 1 ? static_cast<int>(analysis.boundary_loops.size() - 1) : 0;
    analysis.has_holes = analysis.inner_wire_count > 0;
    if (analysis.has_holes) {
        fail(analysis, RegionMergeFailureReason::InnerLoopsNotSupported, "Candidate boundary contains inner loops or holes.");
        return analysis;
    }

    analysis.valid = true;
    analysis.failure_reason = RegionMergeFailureReason::None;
    analysis.message = "Candidate boundary contains one closed outer loop.";
    return analysis;
}

}
