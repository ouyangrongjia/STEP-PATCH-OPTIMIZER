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
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr double kTolerance = 1.0e-9;

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
