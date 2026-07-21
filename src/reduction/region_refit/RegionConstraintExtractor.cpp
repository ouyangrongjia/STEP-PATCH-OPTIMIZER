#include "reduction/region_refit/RegionConstraintExtractor.h"

#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>

#include <algorithm>
#include <queue>
#include <set>
#include <utility>

namespace spo {

namespace {

struct BoundaryEdgeEndpoints {
    EdgeId edgeId = 0;
    TopoDS_Vertex first;
    TopoDS_Vertex second;
    int firstVertex = -1;
    int secondVertex = -1;
};

struct OrderedBoundaryEdge {
    EdgeId edgeId = 0;
    TopAbs_Orientation orientation = TopAbs_FORWARD;
};

RegionConstraintExtractionResult fail(
    const RegionRefitStatus status,
    std::string message) {
    RegionConstraintExtractionResult result;
    result.status = status;
    result.message = std::move(message);
    return result;
}

int findOrAppendVertex(
    std::vector<TopoDS_Vertex>& vertices,
    const TopoDS_Vertex& vertex) {
    for (int index = 0; index < static_cast<int>(vertices.size()); ++index) {
        if (vertices[static_cast<std::size_t>(index)].IsSame(vertex)) {
            return index;
        }
    }
    vertices.push_back(vertex);
    return static_cast<int>(vertices.size() - 1);
}

TopAbs_Orientation edgeOrientationInFace(
    const TopoDS_Edge& edge,
    const TopoDS_Face& face) {
    for (TopExp_Explorer explorer(face, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        if (explorer.Current().IsSame(edge)) {
            return TopoDS::Edge(explorer.Current()).Orientation();
        }
    }
    return TopAbs_FORWARD;
}

std::vector<FaceId> normalizedFaces(const std::vector<FaceId>& faces) {
    auto result = faces;
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool isSelected(const std::set<FaceId>& selectedFaces, const FaceId face) {
    return selectedFaces.find(face) != selectedFaces.end();
}

int countConnectedComponents(
    const TopologyGraph& topology,
    const std::set<FaceId>& selectedFaces,
    const std::vector<EdgeId>& internalEdges) {
    std::set<FaceId> visited;
    int components = 0;
    for (const auto seed : selectedFaces) {
        if (visited.find(seed) != visited.end()) {
            continue;
        }
        ++components;
        std::queue<FaceId> pending;
        pending.push(seed);
        visited.insert(seed);
        while (!pending.empty()) {
            const auto current = pending.front();
            pending.pop();
            for (const auto edgeId : internalEdges) {
                const auto* adjacency = topology.adjacencyForEdge(edgeId);
                if (adjacency == nullptr ||
                    std::find(adjacency->faces.begin(), adjacency->faces.end(), current) == adjacency->faces.end()) {
                    continue;
                }
                for (const auto adjacent : adjacency->faces) {
                    if (isSelected(selectedFaces, adjacent) && visited.insert(adjacent).second) {
                        pending.push(adjacent);
                    }
                }
            }
        }
    }
    return components;
}

bool orderBoundaryLoops(
    std::vector<BoundaryEdgeEndpoints> edges,
    std::vector<std::vector<OrderedBoundaryEdge>>& loops,
    std::string& message) {
    std::sort(edges.begin(), edges.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.edgeId < rhs.edgeId;
    });

    std::vector<TopoDS_Vertex> vertices;
    std::vector<std::vector<int>> incidentEdges;
    for (int index = 0; index < static_cast<int>(edges.size()); ++index) {
        auto& edge = edges[static_cast<std::size_t>(index)];
        edge.firstVertex = findOrAppendVertex(vertices, edge.first);
        edge.secondVertex = findOrAppendVertex(vertices, edge.second);
        const auto required = static_cast<std::size_t>(std::max(edge.firstVertex, edge.secondVertex) + 1);
        if (incidentEdges.size() < required) {
            incidentEdges.resize(required);
        }
        incidentEdges[static_cast<std::size_t>(edge.firstVertex)].push_back(index);
        incidentEdges[static_cast<std::size_t>(edge.secondVertex)].push_back(index);
    }

    for (const auto& incident : incidentEdges) {
        if (incident.size() > 2) {
            message = "Candidate boundary has a branching vertex.";
            return false;
        }
        if (incident.size() != 2) {
            message = "Candidate boundary is open.";
            return false;
        }
    }

    std::set<int> remaining;
    for (int index = 0; index < static_cast<int>(edges.size()); ++index) {
        remaining.insert(index);
    }

    while (!remaining.empty()) {
        const auto firstIndex = *remaining.begin();
        remaining.erase(firstIndex);
        const auto& first = edges[static_cast<std::size_t>(firstIndex)];
        const int startVertex = std::min(first.firstVertex, first.secondVertex);
        int currentVertex = startVertex;
        std::vector<OrderedBoundaryEdge> loop;

        int currentEdgeIndex = firstIndex;
        while (true) {
            const auto& edge = edges[static_cast<std::size_t>(currentEdgeIndex)];
            const bool forward = edge.firstVertex == currentVertex;
            loop.push_back({edge.edgeId, forward ? TopAbs_FORWARD : TopAbs_REVERSED});
            const int nextVertex = forward ? edge.secondVertex : edge.firstVertex;
            if (nextVertex == startVertex) {
                break;
            }

            std::vector<int> matches;
            for (const auto candidateIndex : remaining) {
                const auto& candidate = edges[static_cast<std::size_t>(candidateIndex)];
                if (candidate.firstVertex == nextVertex || candidate.secondVertex == nextVertex) {
                    matches.push_back(candidateIndex);
                }
            }
            if (matches.size() != 1) {
                message = "Candidate boundary does not form deterministic closed loops.";
                return false;
            }
            currentEdgeIndex = matches.front();
            remaining.erase(currentEdgeIndex);
            currentVertex = nextVertex;
        }
        loops.push_back(std::move(loop));
    }
    return true;
}

RegionBoundaryContinuity classifyContinuity(
    const TopoDS_Edge& edge,
    const TopoDS_Face& selectedFace,
    const TopoDS_Face& adjacentFace,
    const bool locked) {
    if (locked) {
        return RegionBoundaryContinuity::Locked;
    }
    if (adjacentFace.IsNull()) {
        return RegionBoundaryContinuity::Free;
    }
    try {
        const auto continuity = BRep_Tool::Continuity(edge, selectedFace, adjacentFace);
        return continuity >= GeomAbs_G1
            ? RegionBoundaryContinuity::Smooth
            : RegionBoundaryContinuity::HardCrease;
    } catch (const Standard_Failure&) {
        return RegionBoundaryContinuity::HardCrease;
    }
}

}

bool RegionConstraintExtractionResult::success() const {
    return status == RegionRefitStatus::Success;
}

RegionConstraintExtractionResult RegionConstraintExtractor::extract(
    const ShapeDocument& document,
    const RegionRefitRequest& request) const {
    if (!document.hasShape()) {
        return fail(RegionRefitStatus::InvalidRequest, "Region refit requires a loaded STEP shape.");
    }
    if (request.selectedFaces.empty()) {
        return fail(RegionRefitStatus::InvalidRequest, "Region refit requires at least one selected face.");
    }

    const auto& topology = document.topology();
    const auto selectedVector = normalizedFaces(request.selectedFaces);
    const std::set<FaceId> selectedFaces(selectedVector.begin(), selectedVector.end());
    const std::set<EdgeId> lockedEdges(request.lockedEdges.begin(), request.lockedEdges.end());
    for (const auto faceId : selectedFaces) {
        if (faceId >= topology.faceCount()) {
            return fail(RegionRefitStatus::InvalidRequest, "Region refit references a missing face.");
        }
    }
    for (const auto edgeId : lockedEdges) {
        if (edgeId >= topology.edgeCount()) {
            return fail(RegionRefitStatus::InvalidRequest, "Region refit references a missing locked edge.");
        }
    }

    RegionTopologyConstraints constraints;
    constraints.selectedFaces = selectedVector;
    std::vector<EdgeId> boundaryEdgeIds;
    for (EdgeId edgeId = 0; edgeId < topology.edgeCount(); ++edgeId) {
        const auto* adjacency = topology.adjacencyForEdge(edgeId);
        if (adjacency == nullptr) {
            continue;
        }
        int selectedCount = 0;
        FaceId soleSelectedFace = 0;
        for (const auto faceId : adjacency->faces) {
            if (isSelected(selectedFaces, faceId)) {
                ++selectedCount;
                soleSelectedFace = faceId;
            }
        }
        if (selectedCount == 0) {
            continue;
        }
        if (adjacency->faces.size() > 2) {
            return fail(RegionRefitStatus::UnsupportedTopology, "Candidate contains a non-manifold edge.");
        }
        if (adjacency->faces.size() == 1 && selectedCount == 1 &&
            BRep_Tool::IsClosed(topology.edge(edgeId), topology.face(soleSelectedFace))) {
            constraints.internalEdges.push_back(edgeId);
            continue;
        }
        if (selectedCount == static_cast<int>(adjacency->faces.size()) && selectedCount == 2) {
            constraints.internalEdges.push_back(edgeId);
            continue;
        }
        if (selectedCount == 1) {
            boundaryEdgeIds.push_back(edgeId);
            continue;
        }
        return fail(RegionRefitStatus::UnsupportedTopology, "Candidate edge adjacency is not supported.");
    }

    if (countConnectedComponents(topology, selectedFaces, constraints.internalEdges) != 1) {
        return fail(RegionRefitStatus::UnsupportedTopology, "Candidate faces are not one connected region.");
    }
    if (boundaryEdgeIds.empty()) {
        return fail(RegionRefitStatus::UnsupportedTopology, "Candidate has no outer boundary.");
    }

    std::vector<BoundaryEdgeEndpoints> endpoints;
    endpoints.reserve(boundaryEdgeIds.size());
    for (const auto edgeId : boundaryEdgeIds) {
        auto forwardEdge = topology.edge(edgeId);
        forwardEdge.Orientation(TopAbs_FORWARD);
        TopoDS_Vertex first;
        TopoDS_Vertex second;
        TopExp::Vertices(forwardEdge, first, second, true);
        if (first.IsNull() || second.IsNull()) {
            return fail(RegionRefitStatus::UnsupportedTopology, "Candidate boundary edge has invalid vertices.");
        }
        endpoints.push_back({edgeId, first, second});
    }

    std::vector<std::vector<OrderedBoundaryEdge>> loops;
    std::string loopFailure;
    if (!orderBoundaryLoops(std::move(endpoints), loops, loopFailure)) {
        return fail(RegionRefitStatus::UnsupportedTopology, std::move(loopFailure));
    }
    if (loops.size() != 1) {
        return fail(RegionRefitStatus::UnsupportedTopology, "Candidate contains holes or multiple outer loops.");
    }

    BRepBuilderAPI_MakeWire wireBuilder;
    for (const auto& ordered : loops.front()) {
        auto edge = topology.edge(ordered.edgeId);
        edge.Orientation(ordered.orientation);
        wireBuilder.Add(edge);
        constraints.orderedBoundaryEdges.push_back(ordered.edgeId);
    }
    if (!wireBuilder.IsDone()) {
        return fail(RegionRefitStatus::UnsupportedTopology, "Candidate outer boundary wire cannot be constructed.");
    }
    constraints.outerWire = wireBuilder.Wire();

    for (const auto edgeId : constraints.internalEdges) {
        if (lockedEdges.find(edgeId) != lockedEdges.end()) {
            constraints.lockedInternalEdges.push_back(edgeId);
        }
    }

    for (std::size_t index = 0; index < loops.front().size(); ++index) {
        const auto& ordered = loops.front()[index];
        const auto* adjacency = topology.adjacencyForEdge(ordered.edgeId);
        if (adjacency == nullptr) {
            return fail(RegionRefitStatus::UnsupportedTopology, "Candidate boundary adjacency is missing.");
        }

        std::optional<FaceId> selectedFaceId;
        std::optional<FaceId> adjacentFaceId;
        for (const auto faceId : adjacency->faces) {
            if (isSelected(selectedFaces, faceId)) {
                selectedFaceId = faceId;
            } else {
                adjacentFaceId = faceId;
            }
        }
        if (!selectedFaceId.has_value()) {
            return fail(RegionRefitStatus::UnsupportedTopology, "Candidate boundary has no selected-side face.");
        }

        RegionBoundaryEdgeConstraint constraint;
        constraint.edgeId = ordered.edgeId;
        constraint.selectedFaceId = *selectedFaceId;
        constraint.adjacentFaceId = adjacentFaceId;
        constraint.originalEdge = topology.edge(ordered.edgeId);
        constraint.loopOrientation = ordered.orientation;
        const auto& selectedFace = topology.face(*selectedFaceId);
        constraint.selectedFaceOrientation = edgeOrientationInFace(constraint.originalEdge, selectedFace);
        constraint.selectedPcurve = BRep_Tool::CurveOnSurface(
            constraint.originalEdge,
            selectedFace,
            constraint.selectedFirstParameter,
            constraint.selectedLastParameter);
        if (constraint.selectedPcurve.IsNull()) {
            return fail(RegionRefitStatus::UnsupportedTopology, "Candidate boundary is missing its selected-side pcurve.");
        }

        TopoDS_Face adjacentFace;
        if (adjacentFaceId.has_value()) {
            adjacentFace = topology.face(*adjacentFaceId);
            constraint.adjacentFaceOrientation = edgeOrientationInFace(constraint.originalEdge, adjacentFace);
            constraint.adjacentPcurve = BRep_Tool::CurveOnSurface(
                constraint.originalEdge,
                adjacentFace,
                constraint.adjacentFirstParameter,
                constraint.adjacentLastParameter);
        }
        const bool locked = lockedEdges.find(ordered.edgeId) != lockedEdges.end();
        constraint.continuity = classifyContinuity(
            constraint.originalEdge,
            selectedFace,
            adjacentFace,
            locked);
        constraint.softAdjacentNormalConstraint =
            constraint.continuity == RegionBoundaryContinuity::Smooth;
        constraints.boundaryEdges.push_back(std::move(constraint));
    }

    RegionConstraintExtractionResult result;
    result.status = RegionRefitStatus::Success;
    result.constraints = std::move(constraints);
    result.message = "Candidate contains one connected, single-loop region.";
    return result;
}

}
