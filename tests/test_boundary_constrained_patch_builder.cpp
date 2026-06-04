#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/BoundaryConstrainedPatchBuilder.h"
#include "patch/ImportedPatchInfo.h"
#include "patch/MultiFacePatchAnalyzer.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchPreviewReport.h"
#include "patch/PatchReplacementInput.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <cassert>

namespace {

TopoDS_Face make_planar_face(double size = 2.0, double xOffset = 0.0) {
    const gp_Pnt p00(xOffset, 0.0, 0.0);
    const gp_Pnt p10(xOffset + size, 0.0, 0.0);
    const gp_Pnt p11(xOffset + size, size, 0.0);
    const gp_Pnt p01(xOffset, size, 0.0);

    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p00, p10).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p10, p11).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p11, p01).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p01, p00).Edge());
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
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

struct BuildFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
    spo::RegionBoundaryAnalysis boundary;
    spo::ImportedPatchInfo imported;
    spo::PatchArtifactPaths artifacts;
    spo::PatchPreviewReport preview;

    explicit BuildFixture(const TopoDS_Shape& patchShape, int patchFaceCount)
        : document(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(), {}),
          imported(imported_patch(patchShape, patchFaceCount)) {
        candidate.candidate_id = 11;
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
        artifacts.patchStepPath = "data/crop_stp/test/candidate_0011.stp";

        preview.success = true;
        preview.highRisk = false;
        preview.candidateId = candidate.candidate_id;
        preview.sourceFaceCount = candidate.face_count;
        preview.sourceBoundaryEdgeCount = candidate.boundary_edge_count;
        preview.patchFaceCount = patchFaceCount;
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

spo::MultiFacePatchAnalysis analyze(const spo::ImportedPatchInfo& imported) {
    return spo::MultiFacePatchAnalyzer().analyze(imported);
}

void test_one_face_patch_builds_special_path() {
    BuildFixture fixture(make_planar_face(), 1);
    const auto analysis = analyze(fixture.imported);

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis);

    assert(result.success);
    assert(result.failureReason == spo::BoundaryConstrainedBuildFailureReason::None);
    assert(!result.replacementShape.IsNull());
    assert(result.replacementFaceCount == 1);
    assert(result.replacementFaces.size() == 1);
    assert(result.usedOneFaceSpecialPath);
    assert(!result.usedMultiFaceFragment);
    assert(result.sourceFaceIds == fixture.candidate.faces);
}

void test_multi_face_box_patch_builds_fragment() {
    BuildFixture fixture(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(), 6);
    const auto analysis = analyze(fixture.imported);
    assert(analysis.faceCount > 1);

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis);

    assert(result.success);
    assert(!result.replacementShape.IsNull());
    assert(result.usedMultiFaceFragment);
    assert(!result.usedOneFaceSpecialPath);
    assert(result.replacementFaceCount > 1);
    assert(result.replacementFaces.size() == analysis.faces.size());
}

void test_empty_analysis_fails() {
    BuildFixture fixture(make_planar_face(), 1);
    spo::MultiFacePatchAnalysis analysis;

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis);

    assert(!result.success);
    assert(result.failureReason == spo::BoundaryConstrainedBuildFailureReason::NoPatchFaces);
    assert(!result.message.empty());
}

void test_invalid_boundary_fails() {
    BuildFixture fixture(make_planar_face(), 1);
    fixture.boundary.valid = false;
    const auto analysis = analyze(fixture.imported);

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis);

    assert(!result.success);
    assert(result.failureReason == spo::BoundaryConstrainedBuildFailureReason::InvalidBoundary);
}

void test_synthetic_twelve_face_patch_is_not_unsupported() {
    BuildFixture fixture(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(), 12);
    auto analysis = analyze(fixture.imported);
    analysis.faceCount = 12;

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis);

    assert(result.success);
    assert(result.usedMultiFaceFragment);
    assert(result.patchFaceCount == 12);
    assert(result.replacementFaceCount > 1);
}

void test_internal_patch_seams_are_retained() {
    BuildFixture fixture(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(), 6);
    const auto analysis = analyze(fixture.imported);
    assert(!analysis.internalEdges.empty());

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis);

    assert(result.success);
    assert(result.internalPatchEdgeCount == static_cast<int>(analysis.internalEdges.size()));
    assert(result.internalPatchEdges.size() == analysis.internalEdges.size());
}

void test_builder_does_not_mutate_shape_document() {
    BuildFixture fixture(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(), 6);
    const auto beforeStats = fixture.document.stats();
    const auto analysis = analyze(fixture.imported);

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis);

    assert(result.success);
    assert(fixture.document.stats().faces == beforeStats.faces);
    assert(fixture.document.stats().edges == beforeStats.edges);
    assert(fixture.document.stats().shells == beforeStats.shells);
    assert(fixture.document.stats().solids == beforeStats.solids);
}

}

void run_boundary_constrained_patch_builder_tests() {
    test_one_face_patch_builds_special_path();
    test_multi_face_box_patch_builds_fragment();
    test_empty_analysis_fails();
    test_invalid_boundary_fails();
    test_synthetic_twelve_face_patch_is_not_unsupported();
    test_internal_patch_seams_are_retained();
    test_builder_does_not_mutate_shape_document();
}
