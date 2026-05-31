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
    int first_vertex = -1;
    int second_vertex = -1;
};

int vertexIndex(std::vector<TopoDS_Vertex>& vertices, const TopoDS_Vertex& vertex) {
    for (int index = 0; index < static_cast<int>(vertices.size()); ++index) {
        if (vertices[static_cast<std::size_t>(index)].IsSame(vertex)) {
            return index;
        }
    }
    vertices.push_back(vertex);
    return static_cast<int>(vertices.size() - 1);
}

int otherVertexIndex(const BoundaryEdgeInfo& info, int vertex) {
    if (info.first_vertex == vertex) {
        return info.second_vertex;
    }
    if (info.second_vertex == vertex) {
        return info.first_vertex;
    }
    return -1;
}

bool orderBoundaryLoops(
    const std::vector<BoundaryEdgeInfo>& edges,
    std::vector<std::vector<EdgeId>>& loops,
    std::vector<EdgeId>& orderedEdges) {
    std::set<int> remaining;
    for (int index = 0; index < static_cast<int>(edges.size()); ++index) {
        remaining.insert(index);
    }

    while (!remaining.empty()) {
        const auto firstIndex = *remaining.begin();
        remaining.erase(firstIndex);
        const auto& firstEdge = edges[static_cast<std::size_t>(firstIndex)];
        std::vector<EdgeId> loop {firstEdge.id};
        const int startVertex = firstEdge.first_vertex;
        int currentVertex = firstEdge.second_vertex;

        while (currentVertex != startVertex) {
            std::vector<int> matches;
            for (const auto index : remaining) {
                const auto& edge = edges[static_cast<std::size_t>(index)];
                if (edge.first_vertex == currentVertex || edge.second_vertex == currentVertex) {
                    matches.push_back(index);
                }
            }
            if (matches.empty()) {
                return false;
            }
            if (matches.size() > 1) {
                return false;
            }

            const auto nextIndex = matches.front();
            remaining.erase(nextIndex);
            const auto& nextEdge = edges[static_cast<std::size_t>(nextIndex)];
            loop.push_back(nextEdge.id);
            currentVertex = otherVertexIndex(nextEdge, currentVertex);
            if (currentVertex < 0) {
                return false;
            }
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
    if (candidate.faces.empty()) {
        fail(analysis, RegionMergeFailureReason::InvalidCandidate, "Candidate has no faces.");
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
    std::set<EdgeId> seenBoundaryEdges;
    for (const auto edgeId : candidate.boundary_edges) {
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) {
            fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate references a missing boundary edge.");
            return analysis;
        }
        if (!seenBoundaryEdges.insert(edgeId).second) {
            analysis.has_non_manifold_edges = true;
            fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate boundary references an edge more than once.");
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
        if (first.IsSame(second)) {
            analysis.has_non_manifold_edges = true;
            fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate boundary contains a degenerate edge.");
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

    std::vector<TopoDS_Vertex> boundaryVertices;
    std::vector<std::vector<int>> incidentEdges;
    for (int index = 0; index < static_cast<int>(boundaryEdges.size()); ++index) {
        auto& edge = boundaryEdges[static_cast<std::size_t>(index)];
        edge.first_vertex = vertexIndex(boundaryVertices, edge.first);
        edge.second_vertex = vertexIndex(boundaryVertices, edge.second);
        const auto requiredSize = static_cast<std::size_t>(std::max(edge.first_vertex, edge.second_vertex) + 1);
        if (incidentEdges.size() < requiredSize) {
            incidentEdges.resize(requiredSize);
        }
        incidentEdges[static_cast<std::size_t>(edge.first_vertex)].push_back(index);
        incidentEdges[static_cast<std::size_t>(edge.second_vertex)].push_back(index);
    }

    analysis.boundary_closed = true;
    for (const auto& incident : incidentEdges) {
        if (incident.size() > 2) {
            analysis.has_branching_boundary = true;
        } else if (incident.size() != 2) {
            analysis.boundary_closed = false;
        }
    }
    if (analysis.has_branching_boundary) {
        fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate boundary has a branching vertex.");
        return analysis;
    }
    if (!analysis.boundary_closed) {
        fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate boundary edges do not form closed loops.");
        return analysis;
    }

    if (!orderBoundaryLoops(boundaryEdges, analysis.boundary_loops, analysis.ordered_boundary_edges)) {
        analysis.boundary_closed = false;
        fail(analysis, RegionMergeFailureReason::BoundaryLoopInvalid, "Candidate boundary edges do not form closed loops.");
        return analysis;
    }

    analysis.outer_wire_count = analysis.boundary_loops.empty() ? 0 : 1;
    analysis.inner_wire_count = analysis.boundary_loops.size() > 1 ? static_cast<int>(analysis.boundary_loops.size() - 1) : 0;
    analysis.has_holes = analysis.inner_wire_count > 0;
    if (analysis.has_holes) {
        fail(analysis, RegionMergeFailureReason::InnerLoopsNotSupported, "Candidate boundary contains inner loops, holes, or multiple closed boundary loops.");
        return analysis;
    }

    analysis.valid = true;
    analysis.failure_reason = RegionMergeFailureReason::None;
    analysis.message = "Candidate boundary contains one closed outer loop.";
    return analysis;
}

}
