#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/BoundaryConstrainedPatchBuilder.h"
#include "patch/ImportedPatchInfo.h"
#include "patch/MultiFacePatchAnalyzer.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchPreviewReport.h"
#include "patch/PatchReplacementInput.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <set>
#include <vector>

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

TopoDS_Face make_rect_face(double x0, double y0, double x1, double y1, double z = 0.0) {
    const gp_Pnt p00(x0, y0, z);
    const gp_Pnt p10(x1, y0, z);
    const gp_Pnt p11(x1, y1, z);
    const gp_Pnt p01(x0, y1, z);

    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p00, p10).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p10, p11).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p11, p01).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p01, p00).Edge());
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

std::vector<spo::EdgeId> outer_boundary_edges_for_faces(
    const spo::ShapeDocument& document,
    const std::vector<spo::FaceId>& faces) {
    std::set<spo::FaceId> faceSet(faces.begin(), faces.end());
    std::vector<spo::EdgeId> edgeIds;
    for (spo::EdgeId edgeId = 0; edgeId < document.topology().edgeCount(); ++edgeId) {
        const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
        if (adjacency == nullptr) {
            continue;
        }
        int selectedFaceUseCount = 0;
        for (const auto faceId : adjacency->faces) {
            if (faceSet.find(faceId) != faceSet.end()) {
                ++selectedFaceUseCount;
            }
        }
        if (selectedFaceUseCount == 1) {
            edgeIds.push_back(edgeId);
        }
    }
    return edgeIds;
}

std::vector<spo::FaceId> first_adjacent_face_pair(const spo::ShapeDocument& document) {
    for (spo::EdgeId edgeId = 0; edgeId < document.topology().edgeCount(); ++edgeId) {
        const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
        if (adjacency != nullptr && adjacency->faces.size() == 2) {
            return {adjacency->faces[0], adjacency->faces[1]};
        }
    }
    return {};
}

TopoDS_Shape patch_shape_from_document_faces(
    const spo::ShapeDocument& document,
    const std::vector<spo::FaceId>& faces,
    bool duplicateFaces) {
    std::vector<TopoDS_Face> patchFaces;
    for (const auto faceId : faces) {
        auto face = document.topology().face(faceId);
        if (duplicateFaces) {
            face = TopoDS::Face(BRepBuilderAPI_Copy(face).Shape());
        }
        patchFaces.push_back(face);
    }
    return make_compound(patchFaces);
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

struct RetrimFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
    spo::RegionBoundaryAnalysis boundary;
    spo::ImportedPatchInfo imported;
    spo::PatchArtifactPaths artifacts;
    spo::PatchPreviewReport preview;

    explicit RetrimFixture(const TopoDS_Shape& patchShape, int patchFaceCount)
        : document(make_rect_face(0.0, 0.0, 2.0, 2.0), {}),
          imported(imported_patch(patchShape, patchFaceCount)) {
        candidate.candidate_id = 31;
        candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
        candidate.status = spo::MergeCandidateStatus::Accepted;
        candidate.faces = {0};
        candidate.face_count = 1;
        candidate.boundary_edges = document.topology().edgesForFace(0);
        candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());

        boundary = spo::RegionBoundaryAnalyzer().analyze(document, candidate);
        assert(boundary.valid);
        candidate.boundary_edges = boundary.ordered_boundary_edges;
        candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());

        artifacts.success = true;
        artifacts.foundStep = true;
        artifacts.patchStepPath = "data/crop_stp/test/candidate_0031.stp";

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

struct AdjacentBoxMultiSurfaceFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
    spo::RegionBoundaryAnalysis boundary;
    spo::ImportedPatchInfo imported;
    spo::PatchArtifactPaths artifacts;
    spo::PatchPreviewReport preview;

    explicit AdjacentBoxMultiSurfaceFixture(bool duplicatePatchFaces)
        : document(BRepPrimAPI_MakeBox(2.0, 1.0, 1.0).Shape(), {}) {
        candidate.candidate_id = 41;
        candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
        candidate.status = spo::MergeCandidateStatus::Accepted;
        candidate.faces = first_adjacent_face_pair(document);
        assert(candidate.faces.size() == 2);
        candidate.face_count = static_cast<int>(candidate.faces.size());
        candidate.boundary_edges = outer_boundary_edges_for_faces(document, candidate.faces);
        candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());

        boundary = spo::RegionBoundaryAnalyzer().analyze(document, candidate);
        assert(boundary.valid);
        candidate.boundary_edges = boundary.ordered_boundary_edges;
        candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());

        imported = imported_patch(
            patch_shape_from_document_faces(document, candidate.faces, duplicatePatchFaces),
            2);

        artifacts.success = true;
        artifacts.foundStep = true;
        artifacts.patchStepPath = "data/crop_stp/test/candidate_0041.stp";

        preview.success = true;
        preview.highRisk = false;
        preview.candidateId = candidate.candidate_id;
        preview.sourceFaceCount = candidate.face_count;
        preview.sourceBoundaryEdgeCount = candidate.boundary_edge_count;
        preview.patchFaceCount = 2;
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

spo::BoundaryConstrainedPatchBuildOptions legacy_fragment_options() {
    spo::BoundaryConstrainedPatchBuildOptions options;
    options.preferOriginalBoundarySurfaceRetrim = false;
    options.allowPatchOuterBoundaryFallback = true;
    return options;
}

void test_one_face_patch_builds_special_path() {
    BuildFixture fixture(make_planar_face(), 1);
    const auto analysis = analyze(fixture.imported);

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis, legacy_fragment_options());

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

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis, legacy_fragment_options());

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

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis, legacy_fragment_options());

    assert(result.success);
    assert(result.usedMultiFaceFragment);
    assert(result.patchFaceCount == 12);
    assert(result.replacementFaceCount > 1);
}

void test_internal_patch_seams_are_retained() {
    BuildFixture fixture(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(), 6);
    const auto analysis = analyze(fixture.imported);
    assert(!analysis.internalEdges.empty());

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis, legacy_fragment_options());

    assert(result.success);
    assert(result.internalPatchEdgeCount == static_cast<int>(analysis.internalEdges.size()));
    assert(result.internalPatchEdges.size() == analysis.internalEdges.size());
}

void test_builder_does_not_mutate_shape_document() {
    BuildFixture fixture(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(), 6);
    const auto beforeStats = fixture.document.stats();
    const auto analysis = analyze(fixture.imported);

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis, legacy_fragment_options());

    assert(result.success);
    assert(fixture.document.stats().faces == beforeStats.faces);
    assert(fixture.document.stats().edges == beforeStats.edges);
    assert(fixture.document.stats().shells == beforeStats.shells);
    assert(fixture.document.stats().solids == beforeStats.solids);
}

void test_original_boundary_surface_retrim_selects_covering_geomagic_surface() {
    const auto patch = make_compound({
        make_rect_face(-0.5, -0.5, 2.5, 2.5, 0.0),
        make_rect_face(-0.5, -0.5, 2.5, 2.5, 1.0)});
    RetrimFixture fixture(patch, 2);
    const auto analysis = analyze(fixture.imported);
    assert(analysis.faceCount == 2);

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis);

    assert(result.success);
    assert(result.usedOriginalBoundarySurfaceRetrim);
    assert(!result.usedMultiFaceFragment);
    assert(result.replacementFaceCount == 1);
    assert(result.patchFaceCount == 2);
    assert(result.retrimSelectedPatchFaceIndex == 0);
    assert(result.retrimBoundarySampleCount > 0);
    assert(result.retrimFailedProjectionCount == 0);
    assert(result.retrimMaxProjectionDistance <= 1.0e-6);
    assert(result.retrimSurfaceCoverageProjectedSampleCount == result.retrimBoundarySampleCount);
    assert(result.retrimSurfaceCoverageFailedProjectionCount == 0);
    assert(result.retrimSurfaceCoverageMaxProjectionDistance <= 1.0e-6);
    assert(result.retrimSurfaceCoverageUncoveredEdgeIds.empty());
}

void test_original_boundary_surface_retrim_fails_without_covering_surface() {
    RetrimFixture fixture(make_rect_face(-0.5, -0.5, 2.5, 2.5, 1.0), 1);
    const auto analysis = analyze(fixture.imported);
    spo::BoundaryConstrainedPatchBuildOptions options;
    options.surfaceRetrimOptions.projectionTolerance = 0.05;

    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis, options);

    assert(!result.success);
    assert(result.failureReason == spo::BoundaryConstrainedBuildFailureReason::ReplacementBuildFailed);
    assert(result.retrimBoundarySampleCount > 0);
    assert(result.retrimFailedProjectionCount == result.retrimBoundarySampleCount);
    assert(result.retrimSurfaceCoverageProjectedSampleCount == 0);
    assert(result.retrimSurfaceCoverageFailedProjectionCount == result.retrimBoundarySampleCount);
    assert(result.retrimSurfaceCoverageMaxProjectionDistance > 0.9);
    assert(!result.retrimSurfaceCoverageUncoveredEdgeIds.empty());
    assert(!result.warningMessage.empty());
}

void test_multi_surface_boundary_shell_builds_when_surface_set_covers_boundary() {
    AdjacentBoxMultiSurfaceFixture fixture(false);
    const auto analysis = analyze(fixture.imported);
    assert(analysis.faceCount == 2);
    assert(!analysis.internalEdges.empty());

    spo::BoundaryConstrainedPatchBuildOptions options;
    options.surfaceRetrimOptions.projectionTolerance = 0.05;
    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis, options);

    assert(result.success);
    assert(result.usedMultiSurfaceBoundaryShell);
    assert(!result.usedOriginalBoundarySurfaceRetrim);
    assert(result.replacementFaceCount == 2);
    assert(result.multiSurfaceBoundarySampleCount > 0);
    assert(result.multiSurfaceProjectedSampleCount == result.multiSurfaceBoundarySampleCount);
    assert(result.multiSurfaceFailedProjectionCount == 0);
    assert(result.multiSurfaceAssignedBoundarySegmentCount >= fixture.candidate.boundary_edge_count);
    assert(result.multiSurfaceBuiltFaceCount == 2);
    assert(result.multiSurfaceOpenWireCount == 0);
    assert(result.multiSurfaceFailedEdgeIds.empty());
}

void test_multi_surface_boundary_shell_fails_without_internal_seam_closure() {
    AdjacentBoxMultiSurfaceFixture fixture(true);
    const auto analysis = analyze(fixture.imported);
    assert(analysis.faceCount == 2);
    assert(analysis.internalEdges.empty());

    spo::BoundaryConstrainedPatchBuildOptions options;
    options.surfaceRetrimOptions.projectionTolerance = 0.05;
    const auto result = spo::BoundaryConstrainedPatchBuilder().build(fixture.input(), analysis, options);

    assert(!result.success);
    assert(result.failureReason == spo::BoundaryConstrainedBuildFailureReason::ReplacementBuildFailed);
    assert(!result.usedMultiSurfaceBoundaryShell);
    assert(result.multiSurfaceBoundarySampleCount > 0);
    assert(result.multiSurfaceFailedProjectionCount == 0);
    assert(result.multiSurfaceAssignedBoundarySegmentCount >= fixture.candidate.boundary_edge_count);
    assert(result.multiSurfaceBuiltFaceCount == 0);
    assert(result.multiSurfaceOpenWireCount > 0);
    assert(!result.message.empty());
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
    test_original_boundary_surface_retrim_selects_covering_geomagic_surface();
    test_original_boundary_surface_retrim_fails_without_covering_surface();
    test_multi_surface_boundary_shell_builds_when_surface_set_covers_boundary();
    test_multi_surface_boundary_shell_fails_without_internal_seam_closure();
}
