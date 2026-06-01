#pragma once

#include <cstddef>
#include <vector>

namespace spo {

struct StlVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct StlTriangle {
    StlVec3 normal;
    StlVec3 v0;
    StlVec3 v1;
    StlVec3 v2;
};

struct StlBoundingBox {
    bool valid = false;
    StlVec3 min;
    StlVec3 max;
};

class StlMesh {
public:
    void clear();
    bool empty() const;
    std::size_t triangleCount() const;

    void addTriangle(const StlTriangle& triangle);
    const std::vector<StlTriangle>& triangles() const;
    std::vector<StlTriangle>& triangles();

    StlBoundingBox boundingBox() const;

private:
    std::vector<StlTriangle> triangles_;
};

}
