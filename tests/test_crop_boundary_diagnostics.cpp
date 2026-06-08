#include "app/AppController.h"
#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/CropBoundaryDiagnostics.h"
#include "stl/StlMesh.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

TopoDS_Face make_square_face(
    double x0 = 0.0,
    double y0 = 0.0,
    double x1 = 1.0,
    double y1 = 1.0,
    double z = 0.0) {
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(x0, y0, z), gp_Pnt(x1, y0, z)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(x1, y0, z), gp_Pnt(x1, y1, z)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(x1, y1, z), gp_Pnt(x0, y1, z)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(x0, y1, z), gp_Pnt(x0, y0, z)).Edge());
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}

spo::StlTriangle make_triangle(
    spo::StlVec3 v0,
    spo::StlVec3 v1,
    spo::StlVec3 v2) {
    spo::StlTriangle triangle;
    triangle.normal = {0.0, 0.0, 1.0};
    triangle.v0 = v0;
    triangle.v1 = v1;
    triangle.v2 = v2;
    return triangle;
}

spo::StlMesh make_full_square_mesh() {
    spo::StlMesh mesh;
    mesh.addTriangle(make_triangle({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 1.0, 0.0}));
    mesh.addTriangle(make_triangle({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}, {0.0, 1.0, 0.0}));
    return mesh;
}

spo::StlMesh make_left_half_mesh() {
    spo::StlMesh mesh;
    mesh.addTriangle(make_triangle({0.0, 0.0, 0.0}, {0.5, 0.0, 0.0}, {0.5, 1.0, 0.0}));
    mesh.addTriangle(make_triangle({0.0, 0.0, 0.0}, {0.5, 1.0, 0.0}, {0.0, 1.0, 0.0}));
    return mesh;
}

void add_rect(spo::StlMesh& mesh, double x0, double y0, double x1, double y1) {
    mesh.addTriangle(make_triangle({x0, y0, 0.0}, {x1, y0, 0.0}, {x1, y1, 0.0}));
    mesh.addTriangle(make_triangle({x0, y0, 0.0}, {x1, y1, 0.0}, {x0, y1, 0.0}));
}

spo::StlMesh make_boundary_strip_mesh() {
    spo::StlMesh mesh;
    add_rect(mesh, 0.0, 0.0, 1.0, 0.02);
    add_rect(mesh, 0.0, 0.98, 1.0, 1.0);
    add_rect(mesh, 0.0, 0.0, 0.02, 1.0);
    add_rect(mesh, 0.98, 0.0, 1.0, 1.0);
    return mesh;
}

struct DiagnosticsFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
    spo::RegionBoundaryAnalysis boundary;

    DiagnosticsFixture()
        : document(make_square_face(), {}) {
        candidate.candidate_id = 31;
        candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
        candidate.status = spo::MergeCandidateStatus::Accepted;
        candidate.faces = {0};
        candidate.face_count = 1;
        candidate.boundary_edges = document.topology().edgesForFace(0);
        candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());
        boundary = spo::RegionBoundaryAnalyzer().analyze(document, candidate);
        assert(boundary.valid);
    }
};

spo::CropBoundaryDiagnosticsOptions options() {
    spo::CropBoundaryDiagnosticsOptions options;
    options.minSamplesPerEdge = 5;
    options.maxSamplesPerEdge = 9;
    options.targetSampleSpacing = 0.2;
    options.stlCoverageTolerance = 0.05;
    options.patchBoundaryTolerance = 0.05;
    return options;
}

bool has_gap_source(const spo::CropBoundaryDiagnosticsReport& report, const std::string& source) {
    return std::any_of(
        report.suspectedGapSegments.begin(),
        report.suspectedGapSegments.end(),
        [&](const auto& segment) {
            return segment.source == source;
        });
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void test_matching_crop_and_patch_have_no_missing_boundary_points() {
    const DiagnosticsFixture fixture;
    const auto localStl = make_full_square_mesh();
    const auto patch = make_square_face();

    spo::CropBoundaryDiagnosticsInput input;
    input.document = &fixture.document;
    input.candidate = &fixture.candidate;
    input.boundary = &fixture.boundary;
    input.localStlMesh = &localStl;
    input.importedPatchShape = &patch;

    const auto report = spo::CropBoundaryDiagnostics().analyze(input, options());

    assert(report.success);
    assert(report.singleClosedOuterLoop);
    assert(report.originalBoundarySampled);
    assert(report.stlCoverageEvaluated);
    assert(report.patchBoundaryEvaluated);
    assert(report.originalBoundarySampleCount > 0);
    assert(report.originalBoundaryEdges.size() == fixture.candidate.boundary_edges.size());
    assert(report.stlCoverageMissingPointCount == 0);
    assert(report.patchBoundaryMissingPointCount == 0);
    assert(report.suspectedGapCount == 0);
    assert(report.suspectedGapEdgeIds.empty());
    for (const auto& edge : report.originalBoundaryEdges) {
        assert(edge.sampleCount >= 5);
        assert(edge.edgeLength > 0.0);
        assert(edge.singleClosedOuterLoop);
        assert(!edge.samples.empty());
    }
}

void test_local_stl_missing_coverage_reports_gap_segments() {
    const DiagnosticsFixture fixture;
    const auto localStl = make_left_half_mesh();
    const auto patch = make_square_face();

    spo::CropBoundaryDiagnosticsInput input;
    input.document = &fixture.document;
    input.candidate = &fixture.candidate;
    input.boundary = &fixture.boundary;
    input.localStlMesh = &localStl;
    input.importedPatchShape = &patch;

    const auto report = spo::CropBoundaryDiagnostics().analyze(input, options());

    assert(report.success);
    assert(report.stlCoverageMissingPointCount > 0);
    assert(report.stlCoverageMaxDistance > report.stlCoverageTolerance);
    assert(report.patchBoundaryMissingPointCount == 0);
    assert(report.suspectedGapCount > 0);
    assert(!report.suspectedGapEdgeIds.empty());
    assert(has_gap_source(report, "STL"));
    assert(!report.warningMessage.empty());
}

void test_boundary_band_reports_inner_missing_area_when_boundary_points_are_covered() {
    const DiagnosticsFixture fixture;
    const auto localStl = make_boundary_strip_mesh();
    const auto patch = make_square_face();

    spo::CropBoundaryDiagnosticsInput input;
    input.document = &fixture.document;
    input.candidate = &fixture.candidate;
    input.boundary = &fixture.boundary;
    input.localStlMesh = &localStl;
    input.importedPatchShape = &patch;

    auto diagnosticOptions = options();
    diagnosticOptions.boundaryBandOffset = 0.20;
    diagnosticOptions.boundaryBandCoverageTolerance = 0.05;
    const auto report = spo::CropBoundaryDiagnostics().analyze(input, diagnosticOptions);

    assert(report.success);
    assert(report.stlCoverageMissingPointCount == 0);
    assert(report.boundaryBandEvaluated);
    assert(report.boundaryBandSampleCount > 0);
    assert(report.boundaryBandMissingPointCount > 0);
    assert(report.boundaryBandMaxDistance > report.boundaryBandCoverageTolerance);
    assert(!report.boundaryBandMissingEdgeIds.empty());
    assert(!report.suspectedCropHoleEdgeIds.empty());
    assert(has_gap_source(report, "Band"));
}

void test_source_triangle_audit_reports_centroid_only_rejection() {
    const DiagnosticsFixture fixture;
    const auto localStl = make_full_square_mesh();
    spo::StlMesh sourceMesh = localStl;
    sourceMesh.addTriangle(make_triangle(
        {0.95, 0.50, 0.0},
        {1.40, 0.45, 0.0},
        {1.40, 0.55, 0.0}));
    const auto patch = make_square_face();

    spo::CropBoundaryDiagnosticsInput input;
    input.document = &fixture.document;
    input.candidate = &fixture.candidate;
    input.boundary = &fixture.boundary;
    input.localStlMesh = &localStl;
    input.sourceStlMesh = &sourceMesh;
    input.importedPatchShape = &patch;

    auto diagnosticOptions = options();
    diagnosticOptions.boundaryBandTriangleTolerance = 0.12;
    const auto report = spo::CropBoundaryDiagnostics().analyze(input, diagnosticOptions);

    assert(report.success);
    assert(report.sourceTriangleAuditEvaluated);
    assert(report.sourceTriangleAuditCount > 0);
    assert(report.rejectedNearBoundaryTriangleCount > 0);
    assert(report.conservativeKeepCandidateCount > 0);
    assert(!report.rejectedNearBoundaryTriangles.empty());
    assert(!report.suspectedCropHoleEdgeIds.empty());

    const auto found = std::any_of(
        report.triangleDecisions.begin(),
        report.triangleDecisions.end(),
        [](const auto& decision) {
            return decision.bboxIntersectsExpandedCandidate &&
                !decision.centroidInsideCandidate &&
                decision.anyVertexInsideCandidate &&
                !decision.keptByCurrentExtractor &&
                decision.shouldKeepConservative &&
                !decision.rejectReason.empty();
        });
    assert(found);
}

void test_patch_outer_boundary_mismatch_reports_patch_gap_segments() {
    const DiagnosticsFixture fixture;
    const auto localStl = make_full_square_mesh();
    const auto patch = make_square_face(0.2, 0.2, 0.8, 0.8, 0.0);

    const auto report = spo::AppController::diagnoseCropBoundaryData(
        fixture.document,
        fixture.candidate,
        &localStl,
        patch,
        options());

    assert(report.success);
    assert(report.stlCoverageMissingPointCount == 0);
    assert(report.patchBoundaryMissingPointCount > 0);
    assert(report.patchBoundaryMaxDistance > report.patchBoundaryTolerance);
    assert(report.suspectedGapCount > 0);
    assert(has_gap_source(report, "Patch"));
    assert(!report.suspectedGapEdgeIds.empty());
}

void test_missing_local_stl_still_reports_patch_boundary_with_warning() {
    const DiagnosticsFixture fixture;
    const auto patch = make_square_face();

    const auto report = spo::AppController::diagnoseCropBoundaryData(
        fixture.document,
        fixture.candidate,
        nullptr,
        patch,
        options());

    assert(report.success);
    assert(!report.stlCoverageEvaluated);
    assert(report.patchBoundaryEvaluated);
    assert(report.patchBoundaryMissingPointCount == 0);
    assert(report.warningMessage.find("Local STL") != std::string::npos);
}

void test_no_hard_coded_real_sample_path_in_crop_boundary_diagnostics_sources() {
#if defined(SPO_SOURCE_DIR)
    const auto sourceRoot = std::filesystem::path(SPO_SOURCE_DIR);
    const auto header = read_text_file(sourceRoot / "src" / "patch" / "CropBoundaryDiagnostics.h");
    const auto source = read_text_file(sourceRoot / "src" / "patch" / "CropBoundaryDiagnostics.cpp");
    const auto appControllerHeader = read_text_file(sourceRoot / "src" / "app" / "AppController.h");
    const auto appControllerSource = read_text_file(sourceRoot / "src" / "app" / "AppController.cpp");
    const auto mainWindowHeader = read_text_file(sourceRoot / "src" / "app" / "MainWindow.h");
    const auto mainWindowSource = read_text_file(sourceRoot / "src" / "app" / "MainWindow.cpp");
    const auto occHeader = read_text_file(sourceRoot / "src" / "gui" / "OccViewWidget.h");
    const auto occSource = read_text_file(sourceRoot / "src" / "gui" / "OccViewWidget.cpp");
    const auto allText = header + source + appControllerHeader + appControllerSource +
        mainWindowHeader + mainWindowSource + occHeader + occSource;
    const auto bannedLocalCandidate = std::string("local_candidate_") + "0179";
    const auto bannedMechanical = std::string("local_candidate_") + "0179_" + "mechanical.stp";
    const auto bannedClay = std::string("03_") + "\xE9\x85\x8D\xE4\xBB\xB6" + "_Clay_candidate_" + "0179";

    assert(allText.find(bannedLocalCandidate) == std::string::npos);
    assert(allText.find(bannedMechanical) == std::string::npos);
    assert(allText.find(bannedClay) == std::string::npos);
    assert(mainWindowSource.find("Crop Boundary diagnostics") != std::string::npos);
    assert(mainWindowSource.find("original boundary sample count") != std::string::npos);
    assert(mainWindowSource.find("STL coverage missing point count") != std::string::npos);
    assert(mainWindowSource.find("patch boundary missing point count") != std::string::npos);
    assert(mainWindowSource.find("boundary-band missing point count") != std::string::npos);
    assert(mainWindowSource.find("rejected near-boundary triangle count") != std::string::npos);
    assert(occHeader.find("showCropBoundaryDiagnosticsOverlay") != std::string::npos);
#endif
}

}

void run_crop_boundary_diagnostics_tests() {
    test_matching_crop_and_patch_have_no_missing_boundary_points();
    test_local_stl_missing_coverage_reports_gap_segments();
    test_boundary_band_reports_inner_missing_area_when_boundary_points_are_covered();
    test_source_triangle_audit_reports_centroid_only_rejection();
    test_patch_outer_boundary_mismatch_reports_patch_gap_segments();
    test_missing_local_stl_still_reports_patch_boundary_with_warning();
    test_no_hard_coded_real_sample_path_in_crop_boundary_diagnostics_sources();
}
