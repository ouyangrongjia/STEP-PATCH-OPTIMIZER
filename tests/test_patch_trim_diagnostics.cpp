#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/PatchTrimDiagnostics.h"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

TopoDS_Face make_rect_face(double x0, double y0, double x1, double y1, double z = 0.0) {
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(x0, y0, z), gp_Pnt(x1, y0, z)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(x1, y0, z), gp_Pnt(x1, y1, z)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(x1, y1, z), gp_Pnt(x0, y1, z)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(x0, y1, z), gp_Pnt(x0, y0, z)).Edge());
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}

TopoDS_Shape make_compound(const std::vector<TopoDS_Face>& faces) {
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& face : faces) {
        builder.Add(compound, face);
    }
    return compound;
}

struct SquareFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
    spo::RegionBoundaryAnalysis boundary;

    SquareFixture()
        : document(make_rect_face(0.0, 0.0, 1.0, 1.0), {}) {
        candidate.candidate_id = 71;
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

spo::PatchTrimDiagnosticsReport analyze(const SquareFixture& fixture, const TopoDS_Shape& replacement) {
    spo::PatchTrimDiagnosticsInput input;
    input.beforeDocument = &fixture.document;
    input.candidate = &fixture.candidate;
    input.boundary = &fixture.boundary;
    input.replacementShape = &replacement;

    spo::PatchTrimDiagnosticsOptions options;
    options.boundarySamplesPerEdge = 9;
    options.surfaceGridDivisions = 5;
    options.distanceTolerance = 0.03;
    return spo::PatchTrimDiagnostics().analyze(input, options);
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void test_matching_replacement_has_no_trim_failures() {
    const SquareFixture fixture;
    const auto report = analyze(fixture, make_rect_face(0.0, 0.0, 1.0, 1.0));

    assert(report.captured);
    assert(report.replacementFaceCount == 1);
    assert(report.overCoverSampleCount == 0);
    assert(report.underCoverSampleCount == 0);
    assert(report.boundaryGapMax <= 1.0e-6);
    assert(report.internalSeamGapMax <= 1.0e-6);
    assert(!report.roundtripChanged);
}

void test_larger_replacement_reports_over_cover() {
    const SquareFixture fixture;
    const auto report = analyze(fixture, make_rect_face(-0.2, -0.2, 1.2, 1.2));

    assert(report.captured);
    assert(report.replacementFaceCount == 1);
    assert(report.overCoverSampleCount > 0);
    assert(report.overCoverRatio > 0.0);
    assert(report.overCoverMaxDistance > 0.15);
}

void test_smaller_replacement_reports_under_cover_and_boundary_gap() {
    const SquareFixture fixture;
    const auto report = analyze(fixture, make_rect_face(0.2, 0.2, 0.8, 0.8));

    assert(report.captured);
    assert(report.underCoverSampleCount > 0);
    assert(report.underCoverMaxDistance > 0.15);
    assert(report.boundaryGapMax > 0.15);
    assert(report.boundaryGapP95 > 0.15);
    assert(report.worstBoundaryEdgeId >= 0);
}

void test_internal_open_seam_reports_internal_gap() {
    const SquareFixture fixture;
    const auto replacement = make_compound({
        make_rect_face(0.0, 0.0, 0.45, 1.0),
        make_rect_face(0.55, 0.0, 1.0, 1.0),
    });

    const auto report = analyze(fixture, replacement);

    assert(report.captured);
    assert(report.replacementFaceCount == 2);
    assert(report.internalSeamGapMax > 0.0);
    assert(report.internalSeamGapP95 > 0.0);
    assert(report.worstInternalEdgeId >= 0);
}

void test_no_hard_coded_real_sample_path_in_trim_diagnostics_sources() {
#if defined(SPO_SOURCE_DIR)
    const auto sourceRoot = std::filesystem::path(SPO_SOURCE_DIR);
    const auto header = read_text_file(sourceRoot / "src" / "patch" / "PatchTrimDiagnostics.h");
    const auto source = read_text_file(sourceRoot / "src" / "patch" / "PatchTrimDiagnostics.cpp");
    const auto allText = header + source;
    const auto bannedLocalCandidate = std::string("local_candidate_") + "0179";
    const auto bannedClay = std::string("03_") + "\xE9\x85\x8D\xE4\xBB\xB6" + "_Clay_candidate_" + "0179";

    assert(allText.find(bannedLocalCandidate) == std::string::npos);
    assert(allText.find(bannedClay) == std::string::npos);
#endif
}

}

void run_patch_trim_diagnostics_tests() {
    test_matching_replacement_has_no_trim_failures();
    test_larger_replacement_reports_over_cover();
    test_smaller_replacement_reports_under_cover_and_boundary_gap();
    test_internal_open_seam_reports_internal_gap();
    test_no_hard_coded_real_sample_path_in_trim_diagnostics_sources();
}
