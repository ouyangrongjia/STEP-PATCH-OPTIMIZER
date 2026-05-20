#include "brep/EdgeIndex.h"

namespace spo {

EdgeId EdgeIndex::addEdge(const TopoDS_Edge& edge) {
    const auto id = edges_.size();
    edges_.push_back(edge);
    return id;
}

std::size_t EdgeIndex::size() const {
    return edges_.size();
}

const TopoDS_Edge& EdgeIndex::edge(EdgeId id) const {
    return edges_.at(id);
}

}
