#include "brep/TopologyGraph.h"

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>

namespace spo {

void TopologyGraph::build(const TopoDS_Shape& shape) {
    clear();
    if (shape.IsNull()) {
        return;
    }

    TopTools_IndexedMapOfShape faceMap;
    TopTools_IndexedMapOfShape edgeMap;
    TopExp::MapShapes(shape, TopAbs_FACE, faceMap);
    TopExp::MapShapes(shape, TopAbs_EDGE, edgeMap);

    for (int index = 1; index <= faceMap.Extent(); ++index) {
        faces_.addFace(TopoDS::Face(faceMap.FindKey(index)));
    }
    for (int index = 1; index <= edgeMap.Extent(); ++index) {
        edges_.addEdge(TopoDS::Edge(edgeMap.FindKey(index)));
    }

    edgeAdjacency_.resize(edges_.size());
    for (std::size_t index = 0; index < edgeAdjacency_.size(); ++index) {
        edgeAdjacency_[index].edge = index;
    }

    TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaceMap);
    for (int index = 1; index <= edgeFaceMap.Extent(); ++index) {
        const auto edgeId = edgeMap.FindIndex(edgeFaceMap.FindKey(index));
        if (edgeId <= 0) {
            continue;
        }

        auto& adjacency = edgeAdjacency_[static_cast<std::size_t>(edgeId - 1)];
        for (TopTools_ListIteratorOfListOfShape it(edgeFaceMap.FindFromIndex(index)); it.More(); it.Next()) {
            const auto faceId = faceMap.FindIndex(it.Value());
            if (faceId > 0) {
                adjacency.faces.push_back(static_cast<FaceId>(faceId - 1));
            }
        }

        std::sort(adjacency.faces.begin(), adjacency.faces.end());
        adjacency.faces.erase(std::unique(adjacency.faces.begin(), adjacency.faces.end()), adjacency.faces.end());
    }
}

void TopologyGraph::clear() {
    faces_ = FaceIndex {};
    edges_ = EdgeIndex {};
    edgeAdjacency_.clear();
}

bool TopologyGraph::empty() const {
    return faces_.size() == 0 && edges_.size() == 0;
}

std::size_t TopologyGraph::faceCount() const {
    return faces_.size();
}

std::size_t TopologyGraph::edgeCount() const {
    return edges_.size();
}

std::optional<FaceId> TopologyGraph::faceIdFor(const TopoDS_Shape& shape) const {
    if (shape.IsNull()) {
        return std::nullopt;
    }

    const auto target = shape.ShapeType() == TopAbs_FACE ? shape : TopoDS_Shape {};
    if (target.IsNull()) {
        return std::nullopt;
    }

    for (FaceId id = 0; id < faces_.size(); ++id) {
        if (faces_.face(id).IsSame(target)) {
            return id;
        }
    }
    return std::nullopt;
}

std::optional<EdgeId> TopologyGraph::edgeIdFor(const TopoDS_Shape& shape) const {
    if (shape.IsNull()) {
        return std::nullopt;
    }

    const auto target = shape.ShapeType() == TopAbs_EDGE ? shape : TopoDS_Shape {};
    if (target.IsNull()) {
        return std::nullopt;
    }

    for (EdgeId id = 0; id < edges_.size(); ++id) {
        if (edges_.edge(id).IsSame(target)) {
            return id;
        }
    }
    return std::nullopt;
}

const TopoDS_Face& TopologyGraph::face(FaceId id) const {
    return faces_.face(id);
}

const TopoDS_Edge& TopologyGraph::edge(EdgeId id) const {
    return edges_.edge(id);
}

const EdgeAdjacency* TopologyGraph::adjacencyForEdge(EdgeId id) const {
    if (id >= edgeAdjacency_.size()) {
        return nullptr;
    }
    return &edgeAdjacency_[id];
}

std::vector<EdgeId> TopologyGraph::edgesForFace(FaceId id) const {
    std::vector<EdgeId> result;
    for (const auto& adjacency : edgeAdjacency_) {
        if (std::find(adjacency.faces.begin(), adjacency.faces.end(), id) != adjacency.faces.end()) {
            result.push_back(adjacency.edge);
        }
    }
    return result;
}

std::vector<FaceId> TopologyGraph::adjacentFaces(FaceId id) const {
    std::vector<FaceId> result;
    for (const auto& adjacency : edgeAdjacency_) {
        if (std::find(adjacency.faces.begin(), adjacency.faces.end(), id) == adjacency.faces.end()) {
            continue;
        }
        for (const auto face : adjacency.faces) {
            if (face != id) {
                result.push_back(face);
            }
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

}
