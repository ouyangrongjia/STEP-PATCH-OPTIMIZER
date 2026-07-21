#include "reduction/region_refit/RegionReferenceMesh.h"

#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace spo {

namespace {

constexpr double kMinimumTriangleShapeQuality = 1.0e-8;

StlVec3 toStl(const gp_Pnt& point) {
    return {point.X(), point.Y(), point.Z()};
}

StlVec3 toStl(const gp_Vec& vector) {
    return {vector.X(), vector.Y(), vector.Z()};
}

void hashValue(std::uint64_t& hash, const std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffu;
        hash *= 1099511628211ull;
    }
}

std::uint64_t quantized(const double value) {
    return static_cast<std::uint64_t>(std::llround(value * 1.0e9));
}

}

bool RegionReferenceMesh::empty() const {
    return vertices.empty() || triangles.empty();
}

StlMesh RegionReferenceMesh::toStlMesh() const {
    StlMesh mesh;
    for (const auto& triangle : triangles) {
        const auto& p0 = vertices[triangle.vertices[0]].point;
        const auto& p1 = vertices[triangle.vertices[1]].point;
        const auto& p2 = vertices[triangle.vertices[2]].point;
        gp_Vec normal(p0, p1);
        normal.Cross(gp_Vec(p0, p2));
        const auto maximumEdgeSquared = std::max({
            p0.SquareDistance(p1),
            p1.SquareDistance(p2),
            p2.SquareDistance(p0)});
        if (maximumEdgeSquared <= 1.0e-24 ||
            normal.Magnitude() / maximumEdgeSquared <= kMinimumTriangleShapeQuality) {
            continue;
        }
        gp_Vec sampledNormal(vertices[triangle.vertices[0]].normal);
        sampledNormal += gp_Vec(vertices[triangle.vertices[1]].normal);
        sampledNormal += gp_Vec(vertices[triangle.vertices[2]].normal);
        if (sampledNormal.SquareMagnitude() > 1.0e-24) {
            normal = sampledNormal;
        }
        normal.Normalize();
        mesh.addTriangle({toStl(normal), toStl(p0), toStl(p1), toStl(p2)});
    }
    return mesh;
}

std::string RegionReferenceMesh::deterministicFingerprint() const {
    std::uint64_t hash = 1469598103934665603ull;
    hashValue(hash, vertices.size());
    hashValue(hash, triangles.size());
    for (const auto& vertex : vertices) {
        hashValue(hash, quantized(vertex.point.X()));
        hashValue(hash, quantized(vertex.point.Y()));
        hashValue(hash, quantized(vertex.point.Z()));
        hashValue(hash, vertex.boundary ? 1u : 0u);
        hashValue(hash, vertex.internalSeam ? 1u : 0u);
        hashValue(hash, vertex.normalDiscontinuity ? 1u : 0u);
    }
    for (const auto& triangle : triangles) {
        hashValue(hash, triangle.vertices[0]);
        hashValue(hash, triangle.vertices[1]);
        hashValue(hash, triangle.vertices[2]);
        hashValue(hash, triangle.faceId);
    }

    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

}
