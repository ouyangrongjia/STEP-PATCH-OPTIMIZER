#include "patch/MultiFacePatchAnalyzer.h"

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/ImportedPatchInfo.h"
#include "patch/PatchPreviewReport.h"
#include "patch/PatchReplacementReport.h"

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>

#include <string>

namespace spo {

namespace {

PatchReplacementReport make_report_with_context(const PatchReplacementInput& input) {
    PatchReplacementReport report;

    if (input.candidate != nullptr) {
        report.candidateId = input.candidate->candidate_id;
        report.sourceFaceCount = input.candidate->face_count > 0
            ? input.candidate->face_count
            : static_cast<int>(input.candidate->faces.size());
        report.sourceBoundaryEdgeCount = input.candidate->boundary_edge_count > 0
            ? input.candidate->boundary_edge_count
            : static_cast<int>(input.candidate->boundary_edges.size());
    }
    if (input.boundary != nullptr && !input.boundary->ordered_boundary_edges.empty()) {
        report.sourceBoundaryEdgeCount = static_cast<int>(input.boundary->ordered_boundary_edges.size());
    }
    if (input.importedPatch != nullptr) {
        report.patchFaceCount = input.importedPatch->faceCount;
        report.patchEdgeCount = input.importedPatch->edgeCount;
        report.patchShellCount = input.importedPatch->shellCount;
        report.patchSolidCount = input.importedPatch->solidCount;
        report.usedMultiFacePatch = input.importedPatch->faceCount > 1;
    }

    return report;
}

PatchReplacementReport fail(
    const PatchReplacementInput& input,
    PatchReplacementFailureReason reason,
    std::string message) {
    auto report = make_report_with_context(input);
    report.failureReason = reason;
    report.message = std::move(message);
    return report;
}

bool boundary_is_supported_for_t6_0(const RegionBoundaryAnalysis& boundary) {
    return boundary.valid &&
        boundary.connected_component_count == 1 &&
        boundary.outer_wire_count == 1 &&
        boundary.inner_wire_count == 0 &&
        boundary.boundary_closed &&
        !boundary.ordered_boundary_edges.empty() &&
        !boundary.has_holes &&
        !boundary.has_non_manifold_edges &&
        !boundary.has_branching_boundary;
}

}

const char* toString(PatchReplacementFailureReason reason) {
    switch (reason) {
    case PatchReplacementFailureReason::None:
        return "None";
    case PatchReplacementFailureReason::MissingDocument:
        return "MissingDocument";
    case PatchReplacementFailureReason::MissingCandidate:
        return "MissingCandidate";
    case PatchReplacementFailureReason::MissingBoundary:
        return "MissingBoundary";
    case PatchReplacementFailureReason::MissingImportedPatch:
        return "MissingImportedPatch";
    case PatchReplacementFailureReason::MissingPreviewReport:
        return "MissingPreviewReport";
    case PatchReplacementFailureReason::InvalidBoundary:
        return "InvalidBoundary";
    case PatchReplacementFailureReason::PreviewNotReady:
        return "PreviewNotReady";
    case PatchReplacementFailureReason::PreviewHighRisk:
        return "PreviewHighRisk";
    case PatchReplacementFailureReason::ImportFailed:
        return "ImportFailed";
    case PatchReplacementFailureReason::EmptyPatchShape:
        return "EmptyPatchShape";
    case PatchReplacementFailureReason::InvalidPatchBRep:
        return "InvalidPatchBRep";
    case PatchReplacementFailureReason::InvalidPatchBBox:
        return "InvalidPatchBBox";
    case PatchReplacementFailureReason::NoPatchFaces:
        return "NoPatchFaces";
    case PatchReplacementFailureReason::UnsupportedCandidate:
        return "UnsupportedCandidate";
    case PatchReplacementFailureReason::BuildFailed:
        return "BuildFailed";
    case PatchReplacementFailureReason::GateFailed:
        return "GateFailed";
    }
    return "Unknown";
}

MultiFacePatchAnalysis MultiFacePatchAnalyzer::analyze(const ImportedPatchInfo& importedPatch) const {
    MultiFacePatchAnalysis analysis;
    analysis.bboxValid = importedPatch.bboxValid;
    analysis.brepCheckValid = importedPatch.brepCheckValid;

    if (importedPatch.shape.IsNull()) {
        analysis.message = "Imported patch shape is empty.";
        return analysis;
    }

    for (TopExp_Explorer explorer(importedPatch.shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        analysis.faces.push_back(TopoDS::Face(explorer.Current()));
    }
    for (TopExp_Explorer explorer(importedPatch.shape, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        analysis.edges.push_back(TopoDS::Edge(explorer.Current()));
    }
    for (TopExp_Explorer explorer(importedPatch.shape, TopAbs_SHELL); explorer.More(); explorer.Next()) {
        ++analysis.shellCount;
    }
    for (TopExp_Explorer explorer(importedPatch.shape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
        ++analysis.solidCount;
    }

    analysis.faceCount = static_cast<int>(analysis.faces.size());
    analysis.edgeCount = static_cast<int>(analysis.edges.size());
    analysis.hasAtLeastOneFace = analysis.faceCount > 0;
    analysis.isSingleFace = analysis.faceCount == 1;
    analysis.isMultiFace = analysis.faceCount > 1;

    if (!analysis.hasAtLeastOneFace) {
        analysis.message = "Imported patch shape contains no faces.";
        return analysis;
    }

    TopTools_IndexedDataMapOfShapeListOfShape edgeToFaces;
    TopExp::MapShapesAndAncestors(importedPatch.shape, TopAbs_EDGE, TopAbs_FACE, edgeToFaces);
    for (int index = 1; index <= edgeToFaces.Extent(); ++index) {
        const auto edge = TopoDS::Edge(edgeToFaces.FindKey(index));
        const auto usageCount = edgeToFaces.FindFromIndex(index).Extent();
        if (usageCount <= 1) {
            analysis.outerEdges.push_back(edge);
        } else {
            analysis.internalEdges.push_back(edge);
        }
    }

    analysis.success = true;
    analysis.message = analysis.isMultiFace
        ? "Analyzed multi-face imported patch."
        : "Analyzed single-face imported patch.";
    return analysis;
}

PatchReplacementReport validatePatchReplacementInput(const PatchReplacementInput& input) {
    if (input.document == nullptr) {
        return fail(input, PatchReplacementFailureReason::MissingDocument, "Patch replacement requires a document.");
    }
    if (!input.document->hasShape()) {
        return fail(input, PatchReplacementFailureReason::MissingDocument, "Patch replacement requires a document with a shape.");
    }
    if (input.candidate == nullptr) {
        return fail(input, PatchReplacementFailureReason::MissingCandidate, "Patch replacement requires a selected candidate.");
    }
    if (input.candidate->faces.empty()) {
        return fail(input, PatchReplacementFailureReason::UnsupportedCandidate, "Selected candidate has no source faces.");
    }
    if (input.candidate->candidate_type != MergeCandidateType::FeatureBoundedRefit) {
        return fail(input, PatchReplacementFailureReason::UnsupportedCandidate, "Only FeatureBoundedRefit candidates are supported by T6 replacement input validation.");
    }
    if (input.boundary == nullptr) {
        return fail(input, PatchReplacementFailureReason::MissingBoundary, "Patch replacement requires boundary analysis.");
    }
    if (!boundary_is_supported_for_t6_0(*input.boundary)) {
        return fail(input, PatchReplacementFailureReason::InvalidBoundary, "Boundary analysis is not a single valid closed outer wire without holes or non-manifold edges.");
    }
    if (input.importedPatch == nullptr) {
        return fail(input, PatchReplacementFailureReason::MissingImportedPatch, "Patch replacement requires an imported patch.");
    }
    if (!input.importedPatch->success) {
        return fail(input, PatchReplacementFailureReason::ImportFailed, "Imported patch did not import successfully.");
    }
    if (input.importedPatch->shape.IsNull()) {
        return fail(input, PatchReplacementFailureReason::EmptyPatchShape, "Imported patch shape is empty.");
    }
    if (!input.importedPatch->bboxValid) {
        return fail(input, PatchReplacementFailureReason::InvalidPatchBBox, "Imported patch bounding box is invalid.");
    }
    if (!input.importedPatch->brepCheckValid) {
        return fail(input, PatchReplacementFailureReason::InvalidPatchBRep, "Imported patch failed BRepCheck.");
    }
    if (input.previewReport == nullptr) {
        return fail(input, PatchReplacementFailureReason::MissingPreviewReport, "Patch replacement requires a preview report.");
    }
    if (!input.previewReport->success) {
        return fail(input, PatchReplacementFailureReason::PreviewNotReady, "Patch preview report is not ready.");
    }
    if (input.previewReport->highRisk && !input.allowHighRiskPatchPreview) {
        return fail(input, PatchReplacementFailureReason::PreviewHighRisk, "Patch preview is high risk.");
    }
    if (input.importedPatch->faceCount <= 0) {
        return fail(input, PatchReplacementFailureReason::NoPatchFaces, "Imported patch has no faces.");
    }

    auto report = make_report_with_context(input);
    report.success = true;
    report.failureReason = PatchReplacementFailureReason::None;
    report.message = "Patch replacement input is valid for T6.0 topology analysis.";
    return report;
}

}
