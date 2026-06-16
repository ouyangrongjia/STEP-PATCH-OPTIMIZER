#include "brep/EdgeIndex.h"
#include "brep/FaceIndex.h"
#include "brep/ShapeDocument.h"
#include "io/StlReader.h"
#include "io/StlWriter.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "stl/StlMesh.h"
#include "stl/StlRegionExtractor.h"
#include "stl/StpSampledFittingMeshBuilder.h"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Solid.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr double kTolerance = 1.0e-9;

struct QuantizedVertex {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    bool operator==(const QuantizedVertex& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct QuantizedVertexHash {
    std::size_t operator()(const QuantizedVertex& vertex) const {
        const auto hx = std::hash<std::int64_t>{}(vertex.x);
        const auto hy = std::hash<std::int64_t>{}(vertex.y);
        const auto hz = std::hash<std::int64_t>{}(vertex.z);
        return hx ^ (hy << 1) ^ (hz << 2);
    }
};

struct QuantizedEdge {
    int first = -1;
    int second = -1;

    bool operator==(const QuantizedEdge& other) const {
        return first == other.first && second == other.second;
    }
};

struct QuantizedEdgeHash {
    std::size_t operator()(const QuantizedEdge& edge) const {
        const auto h0 = std::hash<int>{}(edge.first);
        const auto h1 = std::hash<int>{}(edge.second);
        return h0 ^ (h1 << 1);
    }
};

QuantizedVertex quantize_vertex(const spo::StlVec3& vertex) {
    constexpr double scale = 1.0e8;
    return {
        static_cast<std::int64_t>(std::llround(vertex.x * scale)),
        static_cast<std::int64_t>(std::llround(vertex.y * scale)),
        static_cast<std::int64_t>(std::llround(vertex.z * scale))
    };
}

int boundary_cycle_count(const spo::StlMesh& mesh) {
    std::vector<QuantizedVertex> vertices;
    std::unordered_map<QuantizedVertex, int, QuantizedVertexHash> vertexIds;
    std::unordered_map<QuantizedEdge, int, QuantizedEdgeHash> edgeCounts;

    auto vertex_id = [&](const spo::StlVec3& vertex) {
        const auto key = quantize_vertex(vertex);
        const auto found = vertexIds.find(key);
        if (found != vertexIds.end()) {
            return found->second;
        }
        const auto id = static_cast<int>(vertices.size());
        vertexIds.emplace(key, id);
        vertices.push_back(key);
        return id;
    };

    auto add_edge = [&](int lhs, int rhs) {
        if (lhs == rhs) {
            return;
        }
        const QuantizedEdge edge {std::min(lhs, rhs), std::max(lhs, rhs)};
        ++edgeCounts[edge];
    };

    for (const auto& triangle : mesh.triangles()) {
        const auto v0 = vertex_id(triangle.v0);
        const auto v1 = vertex_id(triangle.v1);
        const auto v2 = vertex_id(triangle.v2);
        add_edge(v0, v1);
        add_edge(v1, v2);
        add_edge(v2, v0);
    }

    std::vector<std::vector<int>> adjacency(vertices.size());
    for (const auto& [edge, count] : edgeCounts) {
        if (count != 1) {
            continue;
        }
        adjacency[static_cast<std::size_t>(edge.first)].push_back(edge.second);
        adjacency[static_cast<std::size_t>(edge.second)].push_back(edge.first);
    }

    int cycles = 0;
    std::vector<bool> visited(vertices.size(), false);
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        if (visited[index] || adjacency[index].empty()) {
            continue;
        }
        ++cycles;
        std::queue<int> queue;
        queue.push(static_cast<int>(index));
        visited[index] = true;
        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();
            for (const auto next : adjacency[static_cast<std::size_t>(current)]) {
                if (!visited[static_cast<std::size_t>(next)]) {
                    visited[static_cast<std::size_t>(next)] = true;
                    queue.push(next);
                }
            }
        }
    }
    return cycles;
}

int connected_component_count(const spo::StlMesh& mesh) {
    const auto triangleCount = mesh.triangleCount();
    if (triangleCount == 0) {
        return 0;
    }

    std::vector<std::vector<std::size_t>> adjacency(triangleCount);
    std::unordered_map<QuantizedVertex, std::vector<std::size_t>, QuantizedVertexHash> trianglesByVertex;

    auto add_vertex = [&](const spo::StlVec3& vertex, std::size_t triangleIndex) {
        trianglesByVertex[quantize_vertex(vertex)].push_back(triangleIndex);
    };

    const auto& triangles = mesh.triangles();
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        add_vertex(triangles[i].v0, i);
        add_vertex(triangles[i].v1, i);
        add_vertex(triangles[i].v2, i);
    }

    for (const auto& [_, owners] : trianglesByVertex) {
        if (owners.size() < 2) {
            continue;
        }
        const auto first = owners.front();
        for (std::size_t i = 1; i < owners.size(); ++i) {
            adjacency[first].push_back(owners[i]);
            adjacency[owners[i]].push_back(first);
        }
    }

    int components = 0;
    std::vector<bool> visited(triangleCount, false);
    for (std::size_t i = 0; i < triangleCount; ++i) {
        if (visited[i]) {
            continue;
        }
        ++components;
        std::queue<std::size_t> queue;
        queue.push(i);
        visited[i] = true;
        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();
            for (const auto next : adjacency[current]) {
                if (!visited[next]) {
                    visited[next] = true;
                    queue.push(next);
                }
            }
        }
    }
    return components;
}

spo::StlVec3 geometric_normal(const spo::StlTriangle& triangle) {
    const auto ax = triangle.v1.x - triangle.v0.x;
    const auto ay = triangle.v1.y - triangle.v0.y;
    const auto az = triangle.v1.z - triangle.v0.z;
    const auto bx = triangle.v2.x - triangle.v0.x;
    const auto by = triangle.v2.y - triangle.v0.y;
    const auto bz = triangle.v2.z - triangle.v0.z;
    return {
        ay * bz - az * by,
        az * bx - ax * bz,
        ax * by - ay * bx
    };
}

double vector_magnitude(const spo::StlVec3& vector) {
    return std::sqrt(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z);
}

double dot(const spo::StlVec3& lhs, const spo::StlVec3& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

void assert_triangle_normals_match_geometry(const spo::StlMesh& mesh) {
    for (const auto& triangle : mesh.triangles()) {
        const auto normal = geometric_normal(triangle);
        const auto geometricMagnitude = vector_magnitude(normal);
        const auto storedMagnitude = vector_magnitude(triangle.normal);
        assert(geometricMagnitude > 1.0e-12);
        assert(storedMagnitude > 1.0e-12);
        assert(dot(normal, triangle.normal) > 0.0);
    }
}

struct PlanarFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
};

PlanarFixture make_planar_square_fixture(double size = 10.0) {
    // Create a planar square face 10x10 on the XY plane
    const auto h = size * 0.5;
    gp_Pnt p1(-h, -h, 0.0);
    gp_Pnt p2( h, -h, 0.0);
    gp_Pnt p3( h,  h, 0.0);
    gp_Pnt p4(-h,  h, 0.0);

    auto edge1 = BRepBuilderAPI_MakeEdge(p1, p2);
    auto edge2 = BRepBuilderAPI_MakeEdge(p2, p3);
    auto edge3 = BRepBuilderAPI_MakeEdge(p3, p4);
    auto edge4 = BRepBuilderAPI_MakeEdge(p4, p1);

    BRepBuilderAPI_MakeWire wireMaker(edge1, edge2, edge3, edge4);
    auto face = BRepBuilderAPI_MakeFace(wireMaker);

    spo::ShapeDocument document(face, "");

    // Set up candidate
    spo::MergeCandidate candidate;
    candidate.candidate_id = 0;
    candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    candidate.face_count = 1;

    const auto& topology = document.topology();
    for (std::size_t fi = 0; fi < topology.faceCount(); ++fi) {
        candidate.faces.push_back(static_cast<spo::FaceId>(fi));
    }
    // boundary edges
    for (std::size_t ei = 0; ei < topology.edgeCount(); ++ei) {
        candidate.boundary_edges.push_back(static_cast<spo::EdgeId>(ei));
    }

    return {document, candidate};
}

void test_planar_face_sampling_produces_triangles() {
    auto fixture = make_planar_square_fixture(10.0);
    spo::StlMesh mesh;
    spo::StpSampledFittingMeshBuilder builder;
    auto report = builder.build(fixture.document, fixture.candidate, {}, mesh);

    assert(report.success);
    assert(report.outputTriangleCount > 0);
    assert(mesh.triangleCount() > 0);
    assert(report.sourceFaceCount == 1);
    assert(report.interiorSampleCount > 0);
    assert(report.boundarySampleCount > 0);
    // Each of 4 edges should have at least min samples
    for (const auto& face : mesh.triangles()) {
        // All triangles should be on or near the XY plane
        assert(std::abs(face.v0.z) < 1.0e-3);
        assert(std::abs(face.v1.z) < 1.0e-3);
        assert(std::abs(face.v2.z) < 1.0e-3);
    }
}

void test_boundary_sample_count_per_edge() {
    auto fixture = make_planar_square_fixture(10.0);
    spo::StpSampledFittingOptions options;
    options.boundarySamplesPerEdge = 16;
    options.minBoundarySamplesPerEdge = 8;

    spo::StlMesh mesh;
    spo::StpSampledFittingMeshBuilder builder;
    auto report = builder.build(fixture.document, fixture.candidate, options, mesh);

    assert(report.success);
    assert(report.boundarySampleCount > 0);
    // 4 edges, each should have at least 8 samples
    assert(report.boundarySampleCount >= 4 * 8);
    assert(report.boundaryEdgeCount == 4);
}

void test_b1_corner_feature_dense_sampling_adds_anchor_facets() {
    auto fixture = make_planar_square_fixture(10.0);

    spo::StpSampledFittingOptions baselineOptions;
    spo::StlMesh baselineMesh;
    spo::StpSampledFittingMeshBuilder builder;
    const auto baselineReport = builder.build(fixture.document, fixture.candidate, baselineOptions, baselineMesh);
    assert(baselineReport.success);
    assert(!baselineReport.cornerFeatureDenseSamplingEnabled);
    assert(baselineReport.featureEdgeDenseSampleCount == 0);
    assert(baselineReport.cornerAnchorSampleCount == 0);
    assert(baselineReport.cornerFeatureSurfaceDivisionCount == 0);

    spo::StpSampledFittingOptions b1Options;
    b1Options.enableCornerFeatureDenseSampling = true;
    b1Options.cornerFeatureSamplesPerEdge = 32;
    spo::StlMesh b1Mesh;
    const auto b1Report = builder.build(fixture.document, fixture.candidate, b1Options, b1Mesh);

    assert(b1Report.success);
    assert(b1Report.cornerFeatureDenseSamplingEnabled);
    assert(b1Report.featureEdgeDenseSampleCount >= 4 * 8);
    assert(b1Report.cornerAnchorSampleCount == 4);
    assert(b1Report.cornerFeatureSurfaceDivisionCount > baselineOptions.maxInteriorDivisions);
    assert(b1Report.outputTriangleCount > baselineReport.outputTriangleCount);
    assert(b1Mesh.triangleCount() > baselineMesh.triangleCount());
    assert(connected_component_count(baselineMesh) == 1);
    assert(connected_component_count(b1Mesh) == 1);
}

void test_b2_boundary_guard_band_expands_connected_mesh() {
    auto fixture = make_planar_square_fixture(10.0);

    spo::StpSampledFittingOptions baselineOptions;
    spo::StlMesh baselineMesh;
    spo::StpSampledFittingMeshBuilder builder;
    const auto baselineReport = builder.build(fixture.document, fixture.candidate, baselineOptions, baselineMesh);
    assert(baselineReport.success);
    assert(!baselineReport.boundaryGuardBandSamplingEnabled);
    assert(baselineReport.boundaryGuardBandSampleCount == 0);
    assert(baselineReport.boundaryGuardBandTriangleCount == 0);

    spo::StpSampledFittingOptions b2Options;
    b2Options.enableBoundaryGuardBandSampling = true;
    b2Options.boundaryGuardBandSamplesPerEdge = 16;
    b2Options.boundaryGuardBandRingCount = 2;
    b2Options.boundaryGuardBandSpacing = 0.5;
    spo::StlMesh b2Mesh;
    const auto b2Report = builder.build(fixture.document, fixture.candidate, b2Options, b2Mesh);

    assert(b2Report.success);
    assert(b2Report.boundaryGuardBandSamplingEnabled);
    assert(b2Report.boundaryGuardBandRingCount == 2);
    assert(b2Report.boundaryGuardBandSpacing == 0.5);
    assert(b2Report.boundaryGuardBandEdgeCount == 4);
    assert(b2Report.boundaryGuardBandSampleCount >= 4 * 16 * 2);
    assert(b2Report.boundaryGuardBandTriangleCount > 0);
    assert(b2Report.outputTriangleCount > baselineReport.outputTriangleCount);
    assert(b2Report.output_bbox.min.x < baselineReport.output_bbox.min.x);
    assert(b2Report.output_bbox.min.y < baselineReport.output_bbox.min.y);
    assert(b2Report.output_bbox.max.x > baselineReport.output_bbox.max.x);
    assert(b2Report.output_bbox.max.y > baselineReport.output_bbox.max.y);
    assert(connected_component_count(b2Mesh) == 1);
}

void test_b2_1_over_cover_strip_expands_connected_mesh_without_guard_band() {
    auto fixture = make_planar_square_fixture(10.0);

    spo::StpSampledFittingOptions baselineOptions;
    spo::StlMesh baselineMesh;
    spo::StpSampledFittingMeshBuilder builder;
    const auto baselineReport = builder.build(fixture.document, fixture.candidate, baselineOptions, baselineMesh);
    assert(baselineReport.success);
    assert(!baselineReport.boundaryOverCoverStripEnabled);
    assert(baselineReport.boundaryOverCoverTriangleCount == 0);

    spo::StpSampledFittingOptions b21Options;
    b21Options.enableBoundaryOverCoverStrip = true;
    b21Options.boundaryOverCoverWidth = 0.4;
    b21Options.boundaryOverCoverRingCount = 1;
    spo::StlMesh b21Mesh;
    const auto b21Report = builder.build(fixture.document, fixture.candidate, b21Options, b21Mesh);

    assert(b21Report.success);
    assert(b21Report.boundaryOverCoverStripEnabled);
    assert(b21Report.boundaryOverCoverWidth == 0.4);
    assert(b21Report.boundaryOverCoverRingCount == 1);
    assert(b21Report.boundaryOverCoverSampleCount > 0);
    assert(b21Report.boundaryOverCoverTriangleCount > 0);
    assert(b21Report.boundaryOverCoverRejectedCount == 0);
    assert(b21Report.boundaryOverCoverBoundaryCoverage >= 0.99);
    assert(!b21Report.boundaryGuardBandSamplingEnabled);
    assert(b21Report.boundaryGuardBandTriangleCount == 0);
    assert(b21Report.outputTriangleCount > baselineReport.outputTriangleCount);
    assert(b21Report.output_bbox.min.x < baselineReport.output_bbox.min.x);
    assert(b21Report.output_bbox.min.y < baselineReport.output_bbox.min.y);
    assert(b21Report.output_bbox.max.x > baselineReport.output_bbox.max.x);
    assert(b21Report.output_bbox.max.y > baselineReport.output_bbox.max.y);
    assert(connected_component_count(b21Mesh) == 1);
    assert(boundary_cycle_count(b21Mesh) == 1);
}

void test_b2_1_over_cover_strip_preserves_planar_winding_and_normals() {
    auto fixture = make_planar_square_fixture(10.0);

    spo::StpSampledFittingOptions baselineOptions;
    spo::StlMesh baselineMesh;
    spo::StpSampledFittingMeshBuilder builder;
    const auto baselineReport = builder.build(fixture.document, fixture.candidate, baselineOptions, baselineMesh);
    assert(baselineReport.success);

    spo::StpSampledFittingOptions b21Options;
    b21Options.enableBoundaryOverCoverStrip = true;
    b21Options.boundaryOverCoverWidth = 0.4;
    b21Options.boundaryOverCoverRingCount = 1;
    spo::StlMesh b21Mesh;
    const auto b21Report = builder.build(fixture.document, fixture.candidate, b21Options, b21Mesh);

    assert(b21Report.success);
    assert(b21Report.boundaryOverCoverTriangleCount > 0);
    assert(b21Mesh.triangleCount() > baselineMesh.triangleCount());
    assert_triangle_normals_match_geometry(b21Mesh);

    const auto& triangles = b21Mesh.triangles();
    for (std::size_t index = baselineMesh.triangleCount(); index < triangles.size(); ++index) {
        const auto normal = geometric_normal(triangles[index]);
        assert(normal.z > 0.0);
        assert(triangles[index].normal.z > 0.0);
    }
}

void test_increased_div_increases_triangle_count() {
    auto fixture = make_planar_square_fixture(10.0);

    spo::StpSampledFittingOptions optionsLow;
    optionsLow.maxInteriorDivisions = 8;
    spo::StlMesh meshLow;
    spo::StpSampledFittingMeshBuilder builder;
    auto reportLow = builder.build(fixture.document, fixture.candidate, optionsLow, meshLow);
    assert(reportLow.success);
    const auto countLow = meshLow.triangleCount();

    spo::StpSampledFittingOptions optionsHigh;
    optionsHigh.maxInteriorDivisions = 20;
    spo::StlMesh meshHigh;
    auto reportHigh = builder.build(fixture.document, fixture.candidate, optionsHigh, meshHigh);
    assert(reportHigh.success);
    const auto countHigh = meshHigh.triangleCount();

    // Higher divisions should produce more triangles (or at minimum not fewer)
    assert(countHigh >= countLow);
}

void test_empty_candidate_fails() {
    TopoDS_Compound compound;
    BRep_Builder brepBuilder;
    brepBuilder.MakeCompound(compound);
    spo::ShapeDocument document(compound, "");

    spo::MergeCandidate candidate;
    candidate.candidate_id = -1;
    candidate.faces.clear();

    spo::StlMesh mesh;
    spo::StpSampledFittingMeshBuilder meshBuilder;
    auto report = meshBuilder.build(document, candidate, {}, mesh);

    assert(!report.success);
    assert(!report.message.empty());
    assert(mesh.empty());
}

void test_output_bbox_covers_candidate() {
    auto fixture = make_planar_square_fixture(10.0);
    spo::StlMesh mesh;
    spo::StpSampledFittingMeshBuilder builder;
    auto report = builder.build(fixture.document, fixture.candidate, {}, mesh);

    assert(report.success);
    assert(report.output_bbox.valid);

    // Output bbox should roughly cover the candidate 10x10 area
    const auto outputDiag = std::sqrt(
        std::pow(report.output_bbox.max.x - report.output_bbox.min.x, 2.0) +
        std::pow(report.output_bbox.max.y - report.output_bbox.min.y, 2.0) +
        std::pow(report.output_bbox.max.z - report.output_bbox.min.z, 2.0));
    assert(outputDiag > 0.0);
    assert(outputDiag >= 10.0 * 0.9); // at least 90% coverage
    assert(outputDiag <= 10.0 * 2.0); // reasonable upper bound
}

void test_stl_roundtrip() {
    auto fixture = make_planar_square_fixture(10.0);
    spo::StlMesh mesh;
    spo::StpSampledFittingMeshBuilder builder;
    auto report = builder.build(fixture.document, fixture.candidate, {}, mesh);
    assert(report.success);

    const auto tmpPath = std::filesystem::temp_directory_path() / "test_stp_sampled_roundtrip.stl";
    spo::StlWriter writer;
    auto writeResult = writer.write(mesh, tmpPath);
    assert(writeResult.success);

    spo::StlReader reader;
    auto readResult = reader.read(tmpPath);
    assert(readResult.success);
    assert(readResult.mesh.triangleCount() == mesh.triangleCount());

    std::filesystem::remove(tmpPath);
}

void test_boundary_band_disabled() {
    // Boundary band is not generated — verify report reflects this
    auto fixture = make_planar_square_fixture(10.0);
    spo::StpSampledFittingOptions options;
    options.bandRingCount = 2;
    options.includeBoundaryBand = true;

    spo::StlMesh mesh;
    spo::StpSampledFittingMeshBuilder builder;
    auto report = builder.build(fixture.document, fixture.candidate, options, mesh);

    assert(report.success);
    assert(report.bandRingCount == 0);
    assert(report.boundaryBandSampleCount == 0);
    assert(report.outputTriangleCount > 0);
    // boundarySampleCount comes from edge sampling for diagnostics
    assert(report.boundarySampleCount > 0);
}

void test_multiple_face_candidate() {
    // Create a box with 2 adjacent faces
    const auto h = 5.0;
    gp_Pnt p1(-h, -h, 0.0);
    gp_Pnt p2( h, -h, 0.0);
    gp_Pnt p3( h,  h, 0.0);
    gp_Pnt p4(-h,  h, 0.0);

    auto edge1 = BRepBuilderAPI_MakeEdge(p1, p2);
    auto edge2 = BRepBuilderAPI_MakeEdge(p2, p3);
    auto edge3 = BRepBuilderAPI_MakeEdge(p3, p4);
    auto edge4 = BRepBuilderAPI_MakeEdge(p4, p1);

    BRepBuilderAPI_MakeWire wireMaker1(edge1, edge2, edge3, edge4);
    auto face1 = BRepBuilderAPI_MakeFace(wireMaker1);

    // Second face: top face shifted up
    gp_Pnt p5(-h, -h, h);
    gp_Pnt p6( h, -h, h);
    gp_Pnt p7( h,  h, h);
    gp_Pnt p8(-h,  h, h);

    auto edge5 = BRepBuilderAPI_MakeEdge(p5, p6);
    auto edge6 = BRepBuilderAPI_MakeEdge(p6, p7);
    auto edge7 = BRepBuilderAPI_MakeEdge(p7, p8);
    auto edge8 = BRepBuilderAPI_MakeEdge(p8, p5);

    BRepBuilderAPI_MakeWire wireMaker2(edge5, edge6, edge7, edge8);
    auto face2 = BRepBuilderAPI_MakeFace(wireMaker2);

    // Create a compound with both faces
    TopoDS_Compound compound;
    BRep_Builder compoundBuilder;
    compoundBuilder.MakeCompound(compound);
    compoundBuilder.Add(compound, face1);
    compoundBuilder.Add(compound, face2);

    spo::ShapeDocument document(compound, "");

    spo::MergeCandidate candidate;
    candidate.candidate_id = 1;
    candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    candidate.face_count = 2;

    const auto& topology = document.topology();
    // The compound has 2 faces, they should be discovered
    // We need to create candidate faces manually since Sewing creates proper topology
    assert(topology.faceCount() >= 2);

    for (std::size_t fi = 0; fi < topology.faceCount(); ++fi) {
        candidate.faces.push_back(static_cast<spo::FaceId>(fi));
    }

    spo::StlMesh mesh;
    spo::StpSampledFittingMeshBuilder meshBuilder;
    spo::StpSampledFittingOptions options;
    options.maxInteriorDivisions = 12;

    auto report = meshBuilder.build(document, candidate, options, mesh);

    // Multi-face candidates need a closed boundary to succeed
    // If boundary analysis fails, the test validates clear failure message
    if (report.success) {
        assert(report.sourceFaceCount == 2);
        assert(report.outputTriangleCount > 0);
    } else {
        assert(!report.message.empty());
    }
}

void test_report_fields_present() {
    auto fixture = make_planar_square_fixture(10.0);
    spo::StlMesh mesh;
    spo::StpSampledFittingMeshBuilder builder;
    auto report = builder.build(fixture.document, fixture.candidate, {}, mesh);

    assert(report.success);
    assert(report.candidateId >= 0);
    assert(report.sourceFaceCount == 1);
    assert(report.boundaryEdgeCount > 0);
    assert(report.boundarySampleCount > 0);
    assert(!report.cornerFeatureDenseSamplingEnabled);
    assert(report.featureEdgeDenseSampleCount == 0);
    assert(report.cornerAnchorSampleCount == 0);
    assert(report.cornerFeatureSurfaceDivisionCount == 0);
    assert(!report.boundaryGuardBandSamplingEnabled);
    assert(report.boundaryGuardBandEdgeCount == 0);
    assert(report.boundaryGuardBandSampleCount == 0);
    assert(report.boundaryGuardBandTriangleCount == 0);
    assert(!report.boundaryOverCoverStripEnabled);
    assert(report.boundaryOverCoverSampleCount == 0);
    assert(report.boundaryOverCoverTriangleCount == 0);
    assert(report.boundaryOverCoverFallbackCount == 0);
    assert(report.boundaryOverCoverRejectedCount == 0);
    assert(report.boundaryOverCoverBoundaryCoverage == 0.0);
    assert(report.interiorSampleCount > 0);
    assert(report.outputTriangleCount > 0);
    assert(report.samplingSpacing > 0.0);
    assert(report.boundarySpacing > 0.0);
    assert(report.bbox.valid);
}

void test_GeomagicFittingInputMode_toString() {
    assert(std::string(spo::toString(spo::GeomagicFittingInputMode::LegacyStlCrop)) == "legacy-stl-crop");
    assert(std::string(spo::toString(spo::GeomagicFittingInputMode::ConservativeBoundaryBandStlCrop)) == "conservative-boundary-band-stl-crop");
    assert(std::string(spo::toString(spo::GeomagicFittingInputMode::StpSampledCandidateSurface)) == "stp-sampled-candidate-surface");
    // All modes produce distinct strings
    assert(std::string(spo::toString(spo::GeomagicFittingInputMode::LegacyStlCrop)) !=
           std::string(spo::toString(spo::GeomagicFittingInputMode::StpSampledCandidateSurface)));
}

void test_bbox_includes_boundary() {
    auto fixture = make_planar_square_fixture(10.0);
    spo::StlMesh mesh;
    spo::StpSampledFittingMeshBuilder builder;
    auto report = builder.build(fixture.document, fixture.candidate, {}, mesh);

    assert(report.success);
    assert(report.output_bbox.valid);
    // The output bbox should be at least as large as the candidate bbox
    // (within reasonable tolerance)
    const auto tol = 1.0;
    assert(report.output_bbox.min.x <= report.bbox.min.x + tol);
    assert(report.output_bbox.min.y <= report.bbox.min.y + tol);
    assert(report.output_bbox.max.x >= report.bbox.max.x - tol);
    assert(report.output_bbox.max.y >= report.bbox.max.y - tol);
}

void test_no_degenerate_triangles() {
    auto fixture = make_planar_square_fixture(10.0);
    spo::StlMesh mesh;
    spo::StpSampledFittingMeshBuilder builder;
    auto report = builder.build(fixture.document, fixture.candidate, {}, mesh);

    assert(report.success);
    for (const auto& tri : mesh.triangles()) {
        // Check that the triangle area is not zero
        const auto ax = tri.v1.x - tri.v0.x;
        const auto ay = tri.v1.y - tri.v0.y;
        const auto az = tri.v1.z - tri.v0.z;
        const auto bx = tri.v2.x - tri.v0.x;
        const auto by = tri.v2.y - tri.v0.y;
        const auto bz = tri.v2.z - tri.v0.z;
        const auto cx = ay * bz - az * by;
        const auto cy = az * bx - ax * bz;
        const auto cz = ax * by - ay * bx;
        const auto area = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
        assert(area > 1.0e-12);
    }
}

}

void run_stp_sampled_fitting_mesh_tests() {
    test_planar_face_sampling_produces_triangles();
    test_boundary_sample_count_per_edge();
    test_b1_corner_feature_dense_sampling_adds_anchor_facets();
    test_b2_boundary_guard_band_expands_connected_mesh();
    test_b2_1_over_cover_strip_expands_connected_mesh_without_guard_band();
    test_b2_1_over_cover_strip_preserves_planar_winding_and_normals();
    test_increased_div_increases_triangle_count();
    test_empty_candidate_fails();
    test_output_bbox_covers_candidate();
    test_stl_roundtrip();
    test_boundary_band_disabled();
    test_report_fields_present();
    test_GeomagicFittingInputMode_toString();
    test_bbox_includes_boundary();
    test_no_degenerate_triangles();
    test_multiple_face_candidate();
}
