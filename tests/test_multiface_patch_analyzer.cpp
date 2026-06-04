#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/ImportedPatchInfo.h"
#include "patch/MultiFacePatchAnalyzer.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchPreviewReport.h"
#include "patch/PatchReplacementInput.h"
#include "patch/PatchReplacementReport.h"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <string>

namespace {

TopoDS_Face make_planar_face(double xOffset = 0.0) {
    const gp_Pnt p00(xOffset, 0.0, 0.0);
    const gp_Pnt p10(xOffset + 1.0, 0.0, 0.0);
    const gp_Pnt p11(xOffset + 1.0, 1.0, 0.0);
    const gp_Pnt p01(xOffset, 1.0, 0.0);

    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p00, p10).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p10, p11).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p11, p01).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p01, p00).Edge());
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}

TopoDS_Shape make_compound_patch() {
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    builder.Add(compound, make_planar_face());
    builder.Add(compound, make_planar_face(2.0));
    return compound;
}

spo::ImportedPatchInfo imported_patch(const TopoDS_Shape& shape, int faceCount) {
    spo::ImportedPatchInfo imported;
    imported.success = true;
    imported.brepCheckValid = true;
    imported.bboxValid = true;
    imported.shape = shape;
    imported.faceCount = faceCount;
    imported.edgeCount = faceCount * 4;
    return imported;
}

struct ValidInputFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
    spo::RegionBoundaryAnalysis boundary;
    spo::ImportedPatchInfo imported;
    spo::PatchArtifactPaths artifacts;
    spo::PatchPreviewReport preview;

    ValidInputFixture()
        : document(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), {}),
          imported(imported_patch(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(), 12)) {
        candidate.candidate_id = 7;
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
        artifacts.localStlPath = "data/crop_stl/test/candidate_0007.stl";
        artifacts.patchStepPath = "data/crop_stp/test/candidate_0007.stp";

        preview.success = true;
        preview.highRisk = false;
        preview.candidateId = candidate.candidate_id;
        preview.sourceFaceCount = candidate.face_count;
        preview.sourceBoundaryEdgeCount = candidate.boundary_edge_count;
        preview.patchFaceCount = imported.faceCount;
        preview.patchEdgeCount = imported.edgeCount;
        preview.patchBRepCheckValid = true;
        preview.patchBboxValid = true;
    }

    spo::PatchReplacementInput input() const {
        spo::PatchReplacementInput input;
        input.document = &document;
        input.candidate = &candidate;
        input.boundary = &boundary;
        input.importedPatch = &imported;
        input.artifactPaths = &artifacts;
        input.previewReport = &preview;
        return input;
    }
};

void assert_failed_for(
    const spo::PatchReplacementInput& input,
    spo::PatchReplacementFailureReason reason) {
    const auto report = spo::validatePatchReplacementInput(input);
    assert(!report.success);
    assert(report.failureReason == reason);
    assert(!report.message.empty());
}

void test_empty_imported_patch_fails_with_clear_message() {
    spo::ImportedPatchInfo imported;
    imported.success = true;
    imported.bboxValid = true;
    imported.brepCheckValid = true;

    const auto analysis = spo::MultiFacePatchAnalyzer().analyze(imported);

    assert(!analysis.success);
    assert(!analysis.message.empty());
    assert(analysis.faceCount == 0);
    assert(!analysis.hasAtLeastOneFace);
}

void test_one_face_patch_analyzes_as_single_face() {
    const auto imported = imported_patch(make_planar_face(), 1);
    const auto analysis = spo::MultiFacePatchAnalyzer().analyze(imported);

    assert(analysis.success);
    assert(analysis.faceCount == 1);
    assert(!analysis.faces.empty());
    assert(analysis.edgeCount == 4);
    assert(analysis.isSingleFace);
    assert(!analysis.isMultiFace);
    assert(analysis.outerEdges.size() == 4);
    assert(analysis.internalEdges.empty());
}

void test_multi_face_box_patch_is_accepted() {
    const auto imported = imported_patch(BRepPrimAPI_MakeBox(2.0, 3.0, 4.0).Shape(), 6);
    const auto analysis = spo::MultiFacePatchAnalyzer().analyze(imported);

    assert(analysis.success);
    assert(analysis.faceCount > 1);
    assert(analysis.isMultiFace);
    assert(!analysis.isSingleFace);
    assert(!analysis.internalEdges.empty());
}

void test_compound_patch_counts_all_faces() {
    const auto imported = imported_patch(make_compound_patch(), 2);
    const auto analysis = spo::MultiFacePatchAnalyzer().analyze(imported);

    assert(analysis.success);
    assert(analysis.faceCount == 2);
    assert(analysis.faces.size() == 2);
    assert(analysis.isMultiFace);
    assert(analysis.outerEdges.size() == 8);
    assert(analysis.internalEdges.empty());
}

void test_validate_input_missing_fields_fail_without_crash() {
    {
        const ValidInputFixture fixture;
        auto input = fixture.input();
        input.document = nullptr;
        assert_failed_for(input, spo::PatchReplacementFailureReason::MissingDocument);
    }
    {
        const ValidInputFixture fixture;
        auto input = fixture.input();
        input.candidate = nullptr;
        assert_failed_for(input, spo::PatchReplacementFailureReason::MissingCandidate);
    }
    {
        const ValidInputFixture fixture;
        auto input = fixture.input();
        input.boundary = nullptr;
        assert_failed_for(input, spo::PatchReplacementFailureReason::MissingBoundary);
    }
    {
        const ValidInputFixture fixture;
        auto input = fixture.input();
        input.importedPatch = nullptr;
        assert_failed_for(input, spo::PatchReplacementFailureReason::MissingImportedPatch);
    }
    {
        const ValidInputFixture fixture;
        auto input = fixture.input();
        input.previewReport = nullptr;
        assert_failed_for(input, spo::PatchReplacementFailureReason::MissingPreviewReport);
    }
}

void test_validate_input_rejects_invalid_boundary() {
    ValidInputFixture fixture;
    fixture.boundary.valid = false;
    assert_failed_for(fixture.input(), spo::PatchReplacementFailureReason::InvalidBoundary);
}

void test_validate_input_accepts_multi_face_patch() {
    const ValidInputFixture fixture;
    const auto report = spo::validatePatchReplacementInput(fixture.input());

    assert(report.success);
    assert(report.failureReason == spo::PatchReplacementFailureReason::None);
    assert(report.usedMultiFacePatch);
    assert(report.candidateId == 7);
    assert(report.sourceFaceCount == 2);
    assert(report.sourceBoundaryEdgeCount == 4);
    assert(report.patchFaceCount == 12);
    assert(report.replacementFaceCount == 0);
}

void test_high_risk_preview_is_rejected() {
    ValidInputFixture fixture;
    fixture.preview.highRisk = true;

    assert_failed_for(fixture.input(), spo::PatchReplacementFailureReason::PreviewHighRisk);
}

}

void run_multiface_patch_analyzer_tests() {
    test_empty_imported_patch_fails_with_clear_message();
    test_one_face_patch_analyzes_as_single_face();
    test_multi_face_box_patch_is_accepted();
    test_compound_patch_counts_all_faces();
    test_validate_input_missing_fields_fail_without_crash();
    test_validate_input_rejects_invalid_boundary();
    test_validate_input_accepts_multi_face_patch();
    test_high_risk_preview_is_rejected();
}
