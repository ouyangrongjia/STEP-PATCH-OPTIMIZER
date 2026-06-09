#include "brep/ShapeDocument.h"
#include "command/CommandContext.h"
#include "command/PatchReplacementCommand.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/ImportedPatchInfo.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchPreviewReport.h"
#include "patch/PatchReplacementInput.h"
#include "patch/PatchReplacementReport.h"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

TopoDS_Shape make_box(double size = 10.0) {
    return BRepPrimAPI_MakeBox(size, size, size).Shape();
}

TopoDS_Face make_open_face(double size = 10.0) {
    const gp_Pnt p00(0.0, 0.0, 0.0);
    const gp_Pnt p10(size, 0.0, 0.0);
    const gp_Pnt p11(size, size, 0.0);
    const gp_Pnt p01(0.0, size, 0.0);

    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p00, p10).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p10, p11).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p11, p01).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p01, p00).Edge());
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
}

int count_shapes(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
    int count = 0;
    for (TopExp_Explorer explorer(shape, type); explorer.More(); explorer.Next()) {
        ++count;
    }
    return count;
}

void fill_bbox(spo::ImportedPatchInfo& imported) {
    Bnd_Box box;
    BRepBndLib::Add(imported.shape, box);
    if (box.IsVoid()) {
        return;
    }

    box.Get(
        imported.bboxMinX,
        imported.bboxMinY,
        imported.bboxMinZ,
        imported.bboxMaxX,
        imported.bboxMaxY,
        imported.bboxMaxZ);
    imported.bboxValid = true;
}

spo::ImportedPatchInfo imported_patch(const TopoDS_Shape& shape) {
    spo::ImportedPatchInfo imported;
    imported.success = true;
    imported.brepCheckValid = true;
    imported.shape = shape;
    imported.faceCount = count_shapes(shape, TopAbs_FACE);
    imported.edgeCount = count_shapes(shape, TopAbs_EDGE);
    imported.shellCount = count_shapes(shape, TopAbs_SHELL);
    imported.solidCount = count_shapes(shape, TopAbs_SOLID);
    fill_bbox(imported);
    return imported;
}

struct CommandFixture {
    spo::CommandContext context;
    spo::ShapeDocument inputDocument;
    spo::MergeCandidate candidate;
    spo::RegionBoundaryAnalysis boundary;
    spo::ImportedPatchInfo imported;
    spo::PatchArtifactPaths artifacts;
    spo::PatchPreviewReport preview;

    explicit CommandFixture(const TopoDS_Shape& patchShape)
        : context(),
          inputDocument(make_box(10.0), {}),
          imported(imported_patch(patchShape)) {
        context.document = inputDocument;

        candidate.candidate_id = 23;
        candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
        candidate.status = spo::MergeCandidateStatus::Accepted;
        candidate.faces = {0, 1};
        candidate.boundary_edges = {0, 1, 2, 3};
        candidate.face_count = static_cast<int>(candidate.faces.size());
        candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());

        boundary.valid = true;
        boundary.connected_component_count = 1;
        boundary.outer_wire_count = 1;
        boundary.inner_wire_count = 0;
        boundary.boundary_closed = true;
        boundary.has_holes = false;
        boundary.has_non_manifold_edges = false;
        boundary.has_branching_boundary = false;
        boundary.ordered_boundary_edges = candidate.boundary_edges;

        artifacts.success = true;
        artifacts.foundStep = true;
        artifacts.localStlPath = "data/crop_stl/test/candidate_0023.stl";
        artifacts.patchStepPath = "data/crop_stp/test/candidate_0023.stp";

        preview.success = true;
        preview.highRisk = false;
        preview.candidateId = candidate.candidate_id;
        preview.sourceFaceCount = candidate.face_count;
        preview.sourceBoundaryEdgeCount = candidate.boundary_edge_count;
        preview.patchFaceCount = imported.faceCount;
        preview.patchEdgeCount = imported.edgeCount;
        preview.patchShellCount = imported.shellCount;
        preview.patchSolidCount = imported.solidCount;
        preview.patchBRepCheckValid = true;
        preview.patchBboxValid = true;
    }

    spo::PatchReplacementInput input() const {
        spo::PatchReplacementInput input;
        input.document = &inputDocument;
        input.candidate = &candidate;
        input.boundary = &boundary;
        input.importedPatch = &imported;
        input.artifactPaths = &artifacts;
        input.previewReport = &preview;
        return input;
    }

    void set_patch(const TopoDS_Shape& patchShape) {
        imported = imported_patch(patchShape);
        preview.patchFaceCount = imported.faceCount;
        preview.patchEdgeCount = imported.edgeCount;
        preview.patchShellCount = imported.shellCount;
        preview.patchSolidCount = imported.solidCount;
        preview.patchBRepCheckValid = true;
        preview.patchBboxValid = imported.bboxValid;
    }
};

void assert_invalid_input_does_not_mutate(
    const spo::PatchReplacementInput& input,
    spo::PatchReplacementFailureReason expectedReason,
    spo::CommandContext& context) {
    const auto beforeStats = context.document.stats();
    spo::PatchReplacementReport report;
    spo::PatchReplacementCommand command(input, &report);

    const auto result = command.execute(context);

    assert(!result.success());
    assert(report.failureReason == expectedReason);
    assert(!report.success);
    assert(!report.rollbackApplied);
    assert(same_stats(context.document.stats(), beforeStats));
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void test_invalid_input_fails_and_does_not_mutate_document() {
    {
        CommandFixture fixture(make_box(10.0));
        auto input = fixture.input();
        input.document = nullptr;
        assert_invalid_input_does_not_mutate(input, spo::PatchReplacementFailureReason::MissingDocument, fixture.context);
    }
    {
        CommandFixture fixture(make_box(10.0));
        auto input = fixture.input();
        input.candidate = nullptr;
        assert_invalid_input_does_not_mutate(input, spo::PatchReplacementFailureReason::MissingCandidate, fixture.context);
    }
    {
        CommandFixture fixture(make_box(10.0));
        auto input = fixture.input();
        input.boundary = nullptr;
        assert_invalid_input_does_not_mutate(input, spo::PatchReplacementFailureReason::MissingBoundary, fixture.context);
    }
    {
        CommandFixture fixture(make_box(10.0));
        auto input = fixture.input();
        input.importedPatch = nullptr;
        assert_invalid_input_does_not_mutate(input, spo::PatchReplacementFailureReason::MissingImportedPatch, fixture.context);
    }
    {
        CommandFixture fixture(make_box(10.0));
        auto input = fixture.input();
        input.previewReport = nullptr;
        assert_invalid_input_does_not_mutate(input, spo::PatchReplacementFailureReason::MissingPreviewReport, fixture.context);
    }
}

void test_multi_face_patch_is_not_unsupported() {
    CommandFixture fixture(make_box(1.0));
    const auto beforeStats = fixture.context.document.stats();
    spo::PatchReplacementReport report;
    spo::PatchReplacementCommand command(fixture.input(), &report);

    const auto result = command.execute(fixture.context);

    assert(!result.success());
    assert(report.failureReason != spo::PatchReplacementFailureReason::UnsupportedCandidate);
    assert(report.failureReason == spo::PatchReplacementFailureReason::GateFailed);
    assert(report.usedMultiFacePatch);
    assert(report.patchFaceCount > 1);
    assert(report.replacementFaceCount > 0);
    if (report.usedOriginalBoundarySurfaceRetrim) {
        assert(report.replacementFaceCount == 1);
        assert(report.retrimBoundarySampleCount > 0);
        assert(report.retrimFailedProjectionCount == 0);
    }
    assert(report.rollbackApplied);
    assert(report.gateEvaluated);
    assert(!report.gatePassed);
    assert(!report.gateFailureReason.empty());
    assert(report.gateBeforeSolidCount == beforeStats.solids);
    assert(report.gateAfterFaceCount > 0);
    assert(same_stats(fixture.context.document.stats(), beforeStats));
}

void test_gate_failure_rolls_back() {
    CommandFixture fixture(make_box(1.0));
    const auto beforeStats = fixture.context.document.stats();
    spo::PatchReplacementReport report;
    spo::PatchReplacementCommand command(fixture.input(), &report);

    const auto result = command.execute(fixture.context);

    assert(!result.success());
    assert(report.failureReason == spo::PatchReplacementFailureReason::GateFailed);
    assert(report.rollbackApplied);
    assert(!report.message.empty());
    assert(report.gateEvaluated);
    assert(!report.gatePassed);
    assert(!report.gateFailureReason.empty());
    assert(report.gateBeforeFaceCount == beforeStats.faces);
    assert(report.gateBeforeEdgeCount == beforeStats.edges);
    assert(report.gateAfterFaceCount > 0);
    assert(same_stats(fixture.context.document.stats(), beforeStats));
}

void test_repair_pipeline_invoked_on_successful_minimal_path() {
    CommandFixture fixture(make_box(10.0));
    fixture.candidate.faces = {0};
    fixture.candidate.face_count = 1;
    fixture.set_patch(fixture.inputDocument.topology().face(0));
    const auto beforeStats = fixture.context.document.stats();
    spo::PatchReplacementReport report;
    spo::PatchReplacementCommand command(fixture.input(), &report);

    const auto executeResult = command.execute(fixture.context);
    assert(executeResult.success());
    assert(report.success);
    assert(report.failureReason == spo::PatchReplacementFailureReason::None);
    assert(report.usedOriginalBoundarySurfaceRetrim);
    assert(report.retrimBoundarySampleCount > 0);
    assert(report.retrimFailedProjectionCount == 0);
    assert(report.repairApplied);
    assert(report.sameParameterApplied);
    assert(report.shapeFixApplied);
    assert(report.shapeFixFaceApplied);
    assert(report.shapeFixWireApplied);
    assert(report.sewingApplied);
    assert(report.repairRunCount == 1);
    assert(report.shapeFixShapeApplied);
    assert(report.unifySameDomainApplied);
    assert(report.sewingAttemptCount > 1);
    assert(report.selectedSewingTolerance > 0.0);
    assert(report.bestSewingFaceCount > 0);
    assert(report.bestSewingEdgeCount > 0);
    assert(report.bestSewingSolidCount > 0);
    assert(report.faceCountBeforeRepair > 0);
    assert(report.faceCountAfterRepair > 0);
    assert(report.gateEvaluated);
    assert(report.gatePassed);
    assert(report.gateFailureReason == "None");
    assert(report.gateBeforeSolidCount == beforeStats.solids);
    assert(report.gateAfterSolidCount == beforeStats.solids);
    assert(report.gateRoundtripSolidCount == beforeStats.solids);
    assert(report.gateBeforeBRepCheckValid);
    assert(report.gateAfterBRepCheckValid);
    assert(report.gateStepExportOk);
    assert(report.gateStepRoundtripOk);
    assert(report.gateRoundtripBRepCheckValid);
    assert(same_stats(fixture.context.document.stats(), beforeStats));

    const auto undoResult = command.undo(fixture.context);
    assert(undoResult.success());
    assert(same_stats(fixture.context.document.stats(), beforeStats));

    const auto redoResult = command.redo(fixture.context);
    assert(redoResult.success());
    assert(same_stats(fixture.context.document.stats(), beforeStats));
    assert(command.report().repairRunCount == 1);
    assert(command.report().sewingAttemptCount == report.sewingAttemptCount);
    assert(report.success);
}

void test_free_edge_increase_after_repair_is_rejected() {
    CommandFixture fixture(make_open_face(10.0));
    const auto beforeStats = fixture.context.document.stats();
    spo::PatchReplacementReport report;
    spo::PatchReplacementCommand command(fixture.input(), &report);

    const auto result = command.execute(fixture.context);

    assert(!result.success());
    assert(report.failureReason == spo::PatchReplacementFailureReason::GateFailed ||
        report.failureReason == spo::PatchReplacementFailureReason::BuildFailed);
    if (report.failureReason == spo::PatchReplacementFailureReason::GateFailed) {
        assert(report.rollbackApplied);
        assert(report.repairApplied);
        assert(report.freeEdgesAfterRepair > 0);
        assert(report.gateEvaluated);
        assert(!report.gatePassed);
        assert(report.gateAfterFreeEdges > 0);
        assert(report.gateAfterFaceCount > 0);
    } else {
        assert(!report.repairApplied);
        assert(!report.gateEvaluated);
        assert(report.retrimBoundarySampleCount > 0);
        assert(report.retrimFailedProjectionCount > 0);
    }
    assert(same_stats(fixture.context.document.stats(), beforeStats));
}

void test_multi_face_internal_seams_are_not_unsupported() {
    CommandFixture fixture(make_box(10.0));
    const auto beforeStats = fixture.context.document.stats();
    spo::PatchReplacementReport report;
    spo::PatchReplacementCommand command(fixture.input(), &report);

    const auto result = command.execute(fixture.context);

    assert(!result.success());
    assert(report.failureReason != spo::PatchReplacementFailureReason::UnsupportedCandidate);
    assert(report.usedMultiFacePatch);
    assert(report.replacementFaceCount > 0);
    if (report.usedOriginalBoundarySurfaceRetrim) {
        assert(report.replacementFaceCount == 1);
        assert(report.retrimBoundarySampleCount > 0);
    }
    assert(report.repairApplied);
    assert(report.failureReason == spo::PatchReplacementFailureReason::GateFailed);
    assert(report.gateEvaluated);
    assert(!report.gatePassed);
    assert(same_stats(fixture.context.document.stats(), beforeStats));
}

void test_no_hard_coded_real_sample_path_in_command_sources() {
#if defined(SPO_SOURCE_DIR)
    const auto sourceRoot = std::filesystem::path(SPO_SOURCE_DIR);
    const auto header = read_text_file(sourceRoot / "src" / "command" / "PatchReplacementCommand.h");
    const auto source = read_text_file(sourceRoot / "src" / "command" / "PatchReplacementCommand.cpp");
    const auto allText = header + source;
    const auto bannedMechanical = std::string("local_candidate_") + "0179_" + "mechanical.stp";
    const auto bannedClay = std::string("03_") + "\xE9\x85\x8D\xE4\xBB\xB6" + "_Clay_candidate_" + "0179";

    assert(allText.find(bannedMechanical) == std::string::npos);
    assert(allText.find(bannedClay) == std::string::npos);
#endif
}

}

void run_patch_replacement_command_tests() {
    test_invalid_input_fails_and_does_not_mutate_document();
    test_multi_face_patch_is_not_unsupported();
    test_gate_failure_rolls_back();
    test_repair_pipeline_invoked_on_successful_minimal_path();
    test_free_edge_increase_after_repair_is_rejected();
    test_multi_face_internal_seams_are_not_unsupported();
    test_no_hard_coded_real_sample_path_in_command_sources();
}
