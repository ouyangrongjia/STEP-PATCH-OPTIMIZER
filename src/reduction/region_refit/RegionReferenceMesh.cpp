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

double cotangent(const gp_Vec& first, const gp_Vec& second) {
    gp_Vec cross = first.Crossed(second);
    const auto denominator = cross.Magnitude();
    return denominator > 1.0e-24 ? first.Dot(second) / denominator : 0.0;
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
        hashValue(hash, vertex.boundaryEdgeIds.size());
        for (const auto edgeId : vertex.boundaryEdgeIds) hashValue(hash, edgeId);
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

void RegionReferenceMesh::updateIntegrationWeights() {
    for (auto& vertex : vertices) {
        vertex.areaWeight = 0.0;
        vertex.boundaryLengthWeight = 0.0;
    }

    for (const auto& triangle : triangles) {
        const auto ia = triangle.vertices[0];
        const auto ib = triangle.vertices[1];
        const auto ic = triangle.vertices[2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) continue;
        const auto& a = vertices[ia].point;
        const auto& b = vertices[ib].point;
        const auto& c = vertices[ic].point;
        const gp_Vec ab(a, b);
        const gp_Vec ac(a, c);
        const gp_Vec ba(b, a);
        const gp_Vec bc(b, c);
        const gp_Vec ca(c, a);
        const gp_Vec cb(c, b);
        const auto doubledArea = ab.Crossed(ac).Magnitude();
        if (doubledArea <= 1.0e-24) continue;
        const auto area = doubledArea * 0.5;
        const bool obtuseA = ab.Dot(ac) < 0.0;
        const bool obtuseB = ba.Dot(bc) < 0.0;
        const bool obtuseC = ca.Dot(cb) < 0.0;
        if (obtuseA || obtuseB || obtuseC) {
            vertices[ia].areaWeight += obtuseA ? area * 0.5 : area * 0.25;
            vertices[ib].areaWeight += obtuseB ? area * 0.5 : area * 0.25;
            vertices[ic].areaWeight += obtuseC ? area * 0.5 : area * 0.25;
            continue;
        }
        const auto cotA = cotangent(ab, ac);
        const auto cotB = cotangent(ba, bc);
        const auto cotC = cotangent(ca, cb);
        vertices[ia].areaWeight += (a.SquareDistance(b) * cotC + a.SquareDistance(c) * cotB) / 8.0;
        vertices[ib].areaWeight += (b.SquareDistance(a) * cotC + b.SquareDistance(c) * cotA) / 8.0;
        vertices[ic].areaWeight += (c.SquareDistance(a) * cotB + c.SquareDistance(b) * cotA) / 8.0;
    }

    for (std::size_t index = 1; index < boundarySamples.size(); ++index) {
        const auto& previous = boundarySamples[index - 1];
        const auto& current = boundarySamples[index];
        if (previous.edgeId != current.edgeId ||
            previous.vertexIndex >= vertices.size() || current.vertexIndex >= vertices.size()) {
            continue;
        }
        const auto length = vertices[previous.vertexIndex].point.Distance(vertices[current.vertexIndex].point);
        vertices[previous.vertexIndex].boundaryLengthWeight += length * 0.5;
        vertices[current.vertexIndex].boundaryLengthWeight += length * 0.5;
    }

    for (auto& vertex : vertices) {
        vertex.areaWeight = std::max(vertex.areaWeight, 1.0e-18);
        vertex.boundaryLengthWeight = std::max(vertex.boundaryLengthWeight, 1.0e-18);
    }
}

}
