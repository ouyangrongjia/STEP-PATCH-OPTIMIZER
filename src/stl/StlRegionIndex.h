#pragma once

#include "stl/StlMesh.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace spo {

class StlRegionIndex {
public:
    explicit StlRegionIndex(const StlMesh& mesh);

    bool compatible(const StlMesh& mesh) const;
    std::vector<std::size_t> query(const StlBoundingBox& bbox) const;
    std::size_t triangleCount() const;

private:
    struct CellRange {
        int minX = 0;
        int minY = 0;
        int minZ = 0;
        int maxX = 0;
        int maxY = 0;
        int maxZ = 0;
    };

    CellRange cellsFor(const StlBoundingBox& bbox) const;
    std::uint64_t cellKey(int x, int y, int z) const;

    const StlMesh* mesh_ = nullptr;
    StlBoundingBox bounds_;
    int resolution_ = 1;
    std::vector<StlBoundingBox> triangleBounds_;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> cells_;
};

}
