#include "stl/StlRegionIndex.h"

#include <algorithm>
#include <cmath>

namespace spo {
namespace {

void includeVertex(StlBoundingBox& bbox, const StlVec3& vertex) {
    bbox.min.x = std::min(bbox.min.x, vertex.x);
    bbox.min.y = std::min(bbox.min.y, vertex.y);
    bbox.min.z = std::min(bbox.min.z, vertex.z);
    bbox.max.x = std::max(bbox.max.x, vertex.x);
    bbox.max.y = std::max(bbox.max.y, vertex.y);
    bbox.max.z = std::max(bbox.max.z, vertex.z);
}

StlBoundingBox triangleBounds(const StlTriangle& triangle) {
    StlBoundingBox bbox;
    bbox.valid = true;
    bbox.min = triangle.v0;
    bbox.max = triangle.v0;
    includeVertex(bbox, triangle.v1);
    includeVertex(bbox, triangle.v2);
    return bbox;
}

bool intersects(const StlBoundingBox& lhs, const StlBoundingBox& rhs) {
    return lhs.valid && rhs.valid &&
        lhs.min.x <= rhs.max.x && lhs.max.x >= rhs.min.x &&
        lhs.min.y <= rhs.max.y && lhs.max.y >= rhs.min.y &&
        lhs.min.z <= rhs.max.z && lhs.max.z >= rhs.min.z;
}

}

StlRegionIndex::StlRegionIndex(const StlMesh& mesh)
    : mesh_(&mesh), bounds_(mesh.boundingBox()) {
    const auto count = mesh.triangleCount();
    if (count == 0 || !bounds_.valid) {
        return;
    }
    resolution_ = std::clamp(
        static_cast<int>(std::ceil(std::cbrt(static_cast<double>(count) / 8.0))),
        8,
        64);
    triangleBounds_.reserve(count);
    for (const auto& triangle : mesh.triangles()) {
        triangleBounds_.push_back(triangleBounds(triangle));
    }
    for (std::size_t index = 0; index < triangleBounds_.size(); ++index) {
        const auto range = cellsFor(triangleBounds_[index]);
        for (int z = range.minZ; z <= range.maxZ; ++z) {
            for (int y = range.minY; y <= range.maxY; ++y) {
                for (int x = range.minX; x <= range.maxX; ++x) {
                    cells_[cellKey(x, y, z)].push_back(index);
                }
            }
        }
    }
}

bool StlRegionIndex::compatible(const StlMesh& mesh) const {
    return mesh_ == &mesh && mesh.triangleCount() == triangleBounds_.size();
}

std::vector<std::size_t> StlRegionIndex::query(const StlBoundingBox& bbox) const {
    std::vector<std::size_t> result;
    if (!bbox.valid || triangleBounds_.empty() || !intersects(bounds_, bbox)) {
        return result;
    }
    std::vector<unsigned char> visited(triangleBounds_.size(), 0);
    const auto range = cellsFor(bbox);
    for (int z = range.minZ; z <= range.maxZ; ++z) {
        for (int y = range.minY; y <= range.maxY; ++y) {
            for (int x = range.minX; x <= range.maxX; ++x) {
                const auto cell = cells_.find(cellKey(x, y, z));
                if (cell == cells_.end()) {
                    continue;
                }
                for (const auto index : cell->second) {
                    if (visited[index] == 0 && intersects(triangleBounds_[index], bbox)) {
                        visited[index] = 1;
                        result.push_back(index);
                    }
                }
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::size_t StlRegionIndex::triangleCount() const {
    return triangleBounds_.size();
}

StlRegionIndex::CellRange StlRegionIndex::cellsFor(const StlBoundingBox& bbox) const {
    const auto cell = [&](double value, double minimum, double maximum) {
        const auto width = maximum - minimum;
        if (!(width > 0.0)) {
            return 0;
        }
        const auto normalized = (value - minimum) / width;
        return std::clamp(static_cast<int>(std::floor(normalized * resolution_)), 0, resolution_ - 1);
    };
    CellRange range;
    range.minX = cell(bbox.min.x, bounds_.min.x, bounds_.max.x);
    range.minY = cell(bbox.min.y, bounds_.min.y, bounds_.max.y);
    range.minZ = cell(bbox.min.z, bounds_.min.z, bounds_.max.z);
    range.maxX = cell(bbox.max.x, bounds_.min.x, bounds_.max.x);
    range.maxY = cell(bbox.max.y, bounds_.min.y, bounds_.max.y);
    range.maxZ = cell(bbox.max.z, bounds_.min.z, bounds_.max.z);
    return range;
}

std::uint64_t StlRegionIndex::cellKey(int x, int y, int z) const {
    return static_cast<std::uint64_t>(x) +
        static_cast<std::uint64_t>(resolution_) *
            (static_cast<std::uint64_t>(y) + static_cast<std::uint64_t>(resolution_) * z);
}

}
