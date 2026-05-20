#pragma once

#include "common/GeometryTypes.h"

#include <TopoDS_Edge.hxx>

#include <vector>

namespace spo {

class EdgeIndex {
public:
    EdgeId addEdge(const TopoDS_Edge& edge);
    std::size_t size() const;
    const TopoDS_Edge& edge(EdgeId id) const;

private:
    std::vector<TopoDS_Edge> edges_;
};

}
