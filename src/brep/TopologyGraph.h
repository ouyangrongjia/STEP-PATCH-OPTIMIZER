#pragma once

#include "brep/EdgeIndex.h"
#include "brep/FaceIndex.h"
#include "common/GeometryTypes.h"

#include <TopoDS_Shape.hxx>

#include <optional>
#include <vector>

namespace spo {

struct EdgeAdjacency {
    EdgeId edge = 0;
    std::vector<FaceId> faces;
};

class TopologyGraph {
public:
    void build(const TopoDS_Shape& shape);
    void clear();

    bool empty() const;
    std::size_t faceCount() const;
    std::size_t edgeCount() const;

    std::optional<FaceId> faceIdFor(const TopoDS_Shape& shape) const;
    std::optional<EdgeId> edgeIdFor(const TopoDS_Shape& shape) const;
    const TopoDS_Face& face(FaceId id) const;
    const TopoDS_Edge& edge(EdgeId id) const;
    const EdgeAdjacency* adjacencyForEdge(EdgeId id) const;
    std::vector<EdgeId> edgesForFace(FaceId id) const;
    std::vector<FaceId> adjacentFaces(FaceId id) const;

private:
    FaceIndex faces_;
    EdgeIndex edges_;
    std::vector<EdgeAdjacency> edgeAdjacency_;
};

}
