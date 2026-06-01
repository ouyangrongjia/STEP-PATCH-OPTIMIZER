#include "stl/StlMesh.h"

#include <algorithm>

namespace spo {

namespace {

void include_vertex(StlBoundingBox& bbox, const StlVec3& vertex) {
    bbox.min.x = std::min(bbox.min.x, vertex.x);
    bbox.min.y = std::min(bbox.min.y, vertex.y);
    bbox.min.z = std::min(bbox.min.z, vertex.z);
    bbox.max.x = std::max(bbox.max.x, vertex.x);
    bbox.max.y = std::max(bbox.max.y, vertex.y);
    bbox.max.z = std::max(bbox.max.z, vertex.z);
}

}

void StlMesh::clear() {
    triangles_.clear();
}

bool StlMesh::empty() const {
    return triangles_.empty();
}

std::size_t StlMesh::triangleCount() const {
    return triangles_.size();
}

void StlMesh::addTriangle(const StlTriangle& triangle) {
    triangles_.push_back(triangle);
}

const std::vector<StlTriangle>& StlMesh::triangles() const {
    return triangles_;
}

std::vector<StlTriangle>& StlMesh::triangles() {
    return triangles_;
}

StlBoundingBox StlMesh::boundingBox() const {
    StlBoundingBox bbox;
    if (triangles_.empty()) {
        return bbox;
    }

    bbox.valid = true;
    bbox.min = triangles_.front().v0;
    bbox.max = triangles_.front().v0;

    for (const auto& triangle : triangles_) {
        include_vertex(bbox, triangle.v0);
        include_vertex(bbox, triangle.v1);
        include_vertex(bbox, triangle.v2);
    }

    return bbox;
}

}
