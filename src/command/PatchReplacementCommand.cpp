#include "command/PatchReplacementCommand.h"

#include "brep/BoundaryWireBuilder.h"
#include "command/CommandContext.h"
#include "patch/BoundaryConstrainedPatchBuilder.h"
#include "patch/MultiFacePatchAnalyzer.h"
#include "patch/PatchReplacementRepair.h"
#include "validate/StrictTopologyGate.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools_ReShape.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <string>
#include <utility>

namespace spo {

namespace {

void append_warning(PatchReplacementReport& report, const std::string& warning) {
    if (warning.empty()) {
        return;
    }
    if (!report.warningMessage.empty()) {
        report.warningMessage += " ";
    }
    report.warningMessage += warning;
}

int count_shapes(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
    int count = 0;
    if (shape.IsNull()) {
        return count;
    }
    for (TopExp_Explorer explorer(shape, type); explorer.More(); explorer.Next()) {
        ++count;
    }
    return count;
}

PatchReplacementFailureReason map_build_failure(BoundaryConstrainedBuildFailureReason reason) {
    switch (reason) {
    case BoundaryConstrainedBuildFailureReason::InvalidInput:
        return PatchReplacementFailureReason::BuildFailed;
    case BoundaryConstrainedBuildFailureReason::InvalidBoundary:
        return PatchReplacementFailureReason::InvalidBoundary;
    case BoundaryConstrainedBuildFailureReason::NoPatchFaces:
        return PatchReplacementFailureReason::NoPatchFaces;
    case BoundaryConstrainedBuildFailureReason::ReplacementBuildFailed:
    case BoundaryConstrainedBuildFailureReason::BoundaryMismatch:
    case BoundaryConstrainedBuildFailureReason::ShapeFixFailed:
    case BoundaryConstrainedBuildFailureReason::None:
        return PatchReplacementFailureReason::BuildFailed;
    }
    return PatchReplacementFailureReason::BuildFailed;
}

std::string gate_failure_message(const StrictTopologyGateReport& gateReport) {
    std::string message = "StrictTopologyGate failed: ";
    message += toString(gateReport.failureReason);
    if (!gateReport.message.empty()) {
        message += ". ";
        message += gateReport.message;
    }
    return message;
}

struct ReplacementAssemblyResult {
    bool success = false;
    TopoDS_Shape shape;
    std::string message;
    std::string warning;
};

TopoDS_Shape boundary_trimmed_patch_face(
    const ShapeDocument& beforeDocument,
    const RegionBoundaryAnalysis& boundary,
    const TopoDS_Face& sourceFace,
    const TopoDS_Face& patchFace,
    std::string& message) {
    const auto surface = BRep_Tool::Surface(patchFace);
    if (surface.IsNull()) {
        message = "Imported one-face patch has no usable surface.";
        return {};
    }

    const auto wire = BoundaryWireBuilder().buildOuterWire(beforeDocument, boundary);
    if (!wire.success) {
        message = wire.message;
        return {};
    }

    BRepBuilderAPI_MakeFace faceBuilder(surface, wire.wire, Standard_True);
    if (!faceBuilder.IsDone()) {
        message = "Could not trim imported patch surface with the original CAD boundary wire.";
        return {};
    }

    auto face = faceBuilder.Face();
    face.Orientation(sourceFace.Orientation());
    message = "Trimmed one-face patch surface with the original CAD boundary wire.";
    return face;
}

ReplacementAssemblyResult assemble_replacement_shape(
    const ShapeDocument& beforeDocument,
    const RegionBoundaryAnalysis& boundary,
    const BoundaryConstrainedPatchBuildResult& buildResult) {
    ReplacementAssemblyResult result;
    if (!beforeDocument.hasShape()) {
        result.message = "Patch replacement assembly requires a before document.";
        return result;
    }
    if (buildResult.replacementShape.IsNull()) {
        result.message = "Patch replacement assembly requires a non-empty replacement shape.";
        return result;
    }
    if (buildResult.sourceFaceIds.empty()) {
        result.message = "Patch replacement assembly requires source faces.";
        return result;
    }

    BRepTools_ReShape reshaper;
    const auto& topology = beforeDocument.topology();
    TopoDS_Shape replacementShape = buildResult.replacementShape;
    bool replacedFirstFace = false;
    for (const auto faceId : buildResult.sourceFaceIds) {
        if (faceId < 0 || static_cast<std::size_t>(faceId) >= topology.faceCount()) {
            result.message = "Patch replacement assembly source face id is out of range.";
            return result;
        }
        const auto& sourceFace = topology.face(faceId);
        if (!replacedFirstFace) {
            if (!buildResult.usedOriginalBoundarySurfaceRetrim &&
                buildResult.sourceFaceIds.size() == 1 &&
                buildResult.replacementFaces.size() == 1) {
                std::string trimMessage;
                const auto trimmedFace = boundary_trimmed_patch_face(
                    beforeDocument,
                    boundary,
                    sourceFace,
                    buildResult.replacementFaces.front(),
                    trimMessage);
                if (!trimmedFace.IsNull()) {
                    replacementShape = trimmedFace;
                    result.warning = trimMessage;
                } else if (!trimMessage.empty()) {
                    result.warning = trimMessage;
                }
            }
            reshaper.Replace(sourceFace, replacementShape);
            replacedFirstFace = true;
        } else {
            reshaper.Remove(sourceFace);
        }
    }

    result.shape = reshaper.Apply(beforeDocument.shape());
    if (result.shape.IsNull()) {
        result.message = "Patch replacement assembly produced an empty shape.";
        return result;
    }
    result.success = true;
    result.message = "Assembled replacement shape with BRepTools_ReShape.";
    return result;
}

void copy_repair_report(
    PatchReplacementReport& report,
    const PatchReplacementRepairReport& repairReport) {
    report.repairApplied = true;
    report.sameParameterApplied = repairReport.sameParameterApplied;
    report.shapeFixShapeApplied = repairReport.shapeFixShapeApplied;
    report.shapeFixFaceApplied = repairReport.shapeFixFaceApplied;
    report.shapeFixWireApplied = repairReport.shapeFixWireApplied;
    report.shapeFixApplied = report.shapeFixShapeApplied || report.shapeFixFaceApplied || report.shapeFixWireApplied;
    report.unifySameDomainApplied = repairReport.unifySameDomainApplied;
    report.sewingApplied = repairReport.sewingApplied;
    report.adaptiveSewingApplied = repairReport.adaptiveSewingApplied;
    report.shellToSolidApplied = repairReport.shellToSolidApplied;
    report.repairRunCount += 1;

    report.faceCountBeforeRepair = repairReport.faceCountBeforeRepair;
    report.edgeCountBeforeRepair = repairReport.edgeCountBeforeRepair;
    report.shellCountBeforeRepair = repairReport.shellCountBeforeRepair;
    report.solidCountBeforeRepair = repairReport.solidCountBeforeRepair;
    report.faceCountAfterRepair = repairReport.faceCountAfterRepair;
    report.edgeCountAfterRepair = repairReport.edgeCountAfterRepair;
    report.shellCountAfterRepair = repairReport.shellCountAfterRepair;
    report.solidCountAfterRepair = repairReport.solidCountAfterRepair;

    report.freeEdgesBeforeRepair = repairReport.freeEdgesBeforeRepair;
    report.freeEdgesAfterRepair = repairReport.freeEdgesAfterRepair;
    report.multipleEdgesBeforeRepair = repairReport.multipleEdgesBeforeRepair;
    report.multipleEdgesAfterRepair = repairReport.multipleEdgesAfterRepair;

    report.selectedSewingTolerance = repairReport.selectedSewingTolerance;
    report.sewingAttemptCount = repairReport.sewingAttemptCount;
    report.bestSewingFreeEdges = repairReport.bestSewingFreeEdges;
    report.bestSewingMultipleEdges = repairReport.bestSewingMultipleEdges;
    report.bestSewingFaceCount = repairReport.bestSewingFaceCount;
    report.bestSewingEdgeCount = repairReport.bestSewingEdgeCount;
    report.bestSewingShellCount = repairReport.bestSewingShellCount;
    report.bestSewingSolidCount = repairReport.bestSewingSolidCount;
    report.bestSewingBRepCheckValid = repairReport.bestSewingBRepCheckValid;
    report.bestSewingCollapsed = repairReport.bestSewingCollapsed;

    report.repairWarningMessage = repairReport.warningMessage;
    append_warning(report, repairReport.warningMessage);
}

void copy_gate_report(
    PatchReplacementReport& report,
    const StrictTopologyGateReport& gateReport) {
    report.gateEvaluated = true;
    report.gatePassed = gateReport.passed;
    report.gateFailureReason = toString(gateReport.failureReason);
    report.gateMessage = gateReport.message;
    report.gateWarningMessage = gateReport.warningMessage;

    report.gateBeforeFaceCount = gateReport.beforeStats.faces;
    report.gateBeforeEdgeCount = gateReport.beforeStats.edges;
    report.gateBeforeShellCount = gateReport.beforeStats.shells;
    report.gateBeforeSolidCount = gateReport.beforeStats.solids;
    report.gateAfterFaceCount = gateReport.afterStats.faces;
    report.gateAfterEdgeCount = gateReport.afterStats.edges;
    report.gateAfterShellCount = gateReport.afterStats.shells;
    report.gateAfterSolidCount = gateReport.afterStats.solids;
    report.gateRoundtripFaceCount = gateReport.roundtripStats.faces;
    report.gateRoundtripEdgeCount = gateReport.roundtripStats.edges;
    report.gateRoundtripShellCount = gateReport.roundtripStats.shells;
    report.gateRoundtripSolidCount = gateReport.roundtripStats.solids;

    report.gateBeforeFreeEdges = gateReport.beforeFreeEdges;
    report.gateAfterFreeEdges = gateReport.afterFreeEdges;
    report.gateRoundtripFreeEdges = gateReport.roundtripFreeEdges;
    report.gateBeforeMultipleEdges = gateReport.beforeMultipleEdges;
    report.gateAfterMultipleEdges = gateReport.afterMultipleEdges;
    report.gateRoundtripMultipleEdges = gateReport.roundtripMultipleEdges;

    report.gateBeforeBRepCheckValid = gateReport.beforeBRepCheckValid;
    report.gateAfterBRepCheckValid = gateReport.afterBRepCheckValid;
    report.gateRoundtripBRepCheckValid = gateReport.roundtripBRepCheckValid;
    report.gateStepExportOk = gateReport.stepExportOk;
    report.gateStepRoundtripOk = gateReport.stepRoundtripOk;
    report.gateWatertightSolidRequired = gateReport.watertightSolidRequired;
    report.gateRoundtripWatertightRequired = gateReport.roundtripWatertightRequired;
}

}

PatchReplacementCommand::PatchReplacementCommand(
    PatchReplacementInput input,
    PatchReplacementReport* outReport,
    PatchReplacementCommandOptions options)
    : outReport_(outReport),
      options_(options) {
    hasDocumentInput_ = input.document != nullptr;
    hasCandidateInput_ = input.candidate != nullptr;
    hasBoundaryInput_ = input.boundary != nullptr;
    hasImportedPatchInput_ = input.importedPatch != nullptr;
    hasArtifactPathsInput_ = input.artifactPaths != nullptr;
    hasPreviewReportInput_ = input.previewReport != nullptr;

    if (hasDocumentInput_) {
        documentSnapshot_ = *input.document;
        input_.document = &documentSnapshot_;
    }
    if (hasCandidateInput_) {
        candidateSnapshot_ = *input.candidate;
        input_.candidate = &candidateSnapshot_;
    }
    if (hasBoundaryInput_) {
        boundarySnapshot_ = *input.boundary;
        input_.boundary = &boundarySnapshot_;
    }
    if (hasImportedPatchInput_) {
        importedPatchSnapshot_ = *input.importedPatch;
        input_.importedPatch = &importedPatchSnapshot_;
    }
    if (hasArtifactPathsInput_) {
        artifactPathsSnapshot_ = *input.artifactPaths;
        input_.artifactPaths = &artifactPathsSnapshot_;
    }
    if (hasPreviewReportInput_) {
        previewReportSnapshot_ = *input.previewReport;
        input_.previewReport = &previewReportSnapshot_;
    }
}

const char* PatchReplacementCommand::name() const {
    return "PatchReplacementCommand";
}

Result PatchReplacementCommand::execute(CommandContext& context) {
    executed_ = false;
    committed_ = false;
    beforeDocument_ = {};
    afterDocument_ = {};
    report_ = {};

    if (hasDocumentInput_) {
        rebuildStableInputFromContext(context.document);
    }

    const auto inputReport = validatePatchReplacementInput(input_);
    report_ = inputReport;
    if (!inputReport.success) {
        publishReport();
        return Result::error(report_.message);
    }

    beforeDocument_ = context.document;
    const auto analysis = MultiFacePatchAnalyzer().analyze(*input_.importedPatch);
    report_.patchFaceCount = analysis.faceCount;
    report_.patchEdgeCount = analysis.edgeCount;
    report_.patchShellCount = analysis.shellCount;
    report_.patchSolidCount = analysis.solidCount;
    report_.usedMultiFacePatch = analysis.isMultiFace;
    if (!analysis.success) {
        report_.success = false;
        report_.failureReason = PatchReplacementFailureReason::NoPatchFaces;
        report_.message = analysis.message;
        publishReport();
        return Result::error(report_.message);
    }

    const auto buildResult = BoundaryConstrainedPatchBuilder().build(input_, analysis);
    report_.sourceFaceCount = buildResult.sourceFaceCount;
    report_.patchFaceCount = buildResult.patchFaceCount;
    report_.replacementFaceCount = buildResult.replacementFaceCount;
    report_.replacementEdgeCount = count_shapes(buildResult.replacementShape, TopAbs_EDGE);
    report_.replacementShellCount = count_shapes(buildResult.replacementShape, TopAbs_SHELL);
    report_.replacementSolidCount = count_shapes(buildResult.replacementShape, TopAbs_SOLID);
    report_.usedMultiFacePatch = buildResult.usedMultiFaceFragment || analysis.isMultiFace;
    report_.usedOriginalBoundarySurfaceRetrim = buildResult.usedOriginalBoundarySurfaceRetrim;
    report_.usedMultiSurfaceBoundaryShell = buildResult.usedMultiSurfaceBoundaryShell;
    report_.retrimSelectedPatchFaceIndex = buildResult.retrimSelectedPatchFaceIndex;
    report_.retrimBoundarySampleCount = buildResult.retrimBoundarySampleCount;
    report_.retrimProjectedSampleCount = buildResult.retrimProjectedSampleCount;
    report_.retrimFailedProjectionCount = buildResult.retrimFailedProjectionCount;
    report_.retrimMaxProjectionDistance = buildResult.retrimMaxProjectionDistance;
    report_.retrimAverageProjectionDistance = buildResult.retrimAverageProjectionDistance;
    report_.retrimSurfaceCoverageProjectedSampleCount = buildResult.retrimSurfaceCoverageProjectedSampleCount;
    report_.retrimSurfaceCoverageFailedProjectionCount = buildResult.retrimSurfaceCoverageFailedProjectionCount;
    report_.retrimSurfaceCoverageMaxProjectionDistance = buildResult.retrimSurfaceCoverageMaxProjectionDistance;
    report_.retrimSurfaceCoverageAverageProjectionDistance = buildResult.retrimSurfaceCoverageAverageProjectionDistance;
    report_.retrimSurfaceCoverageUncoveredEdgeIds = buildResult.retrimSurfaceCoverageUncoveredEdgeIds;
    report_.multiSurfaceBoundarySampleCount = buildResult.multiSurfaceBoundarySampleCount;
    report_.multiSurfaceProjectedSampleCount = buildResult.multiSurfaceProjectedSampleCount;
    report_.multiSurfaceFailedProjectionCount = buildResult.multiSurfaceFailedProjectionCount;
    report_.multiSurfaceMaxProjectionDistance = buildResult.multiSurfaceMaxProjectionDistance;
    report_.multiSurfaceAverageProjectionDistance = buildResult.multiSurfaceAverageProjectionDistance;
    report_.multiSurfaceAssignedBoundarySegmentCount = buildResult.multiSurfaceAssignedBoundarySegmentCount;
    report_.multiSurfaceBuiltFaceCount = buildResult.multiSurfaceBuiltFaceCount;
    report_.multiSurfaceOpenWireCount = buildResult.multiSurfaceOpenWireCount;
    report_.multiSurfaceFailedEdgeIds = buildResult.multiSurfaceFailedEdgeIds;
    append_warning(report_, buildResult.warningMessage);

    if (!buildResult.success) {
        report_.success = false;
        report_.failureReason = map_build_failure(buildResult.failureReason);
        report_.message = buildResult.message;
        publishReport();
        return Result::error(report_.message);
    }

    const auto assemblyResult = assemble_replacement_shape(beforeDocument_, *input_.boundary, buildResult);
    if (!assemblyResult.success) {
        report_.success = false;
        report_.failureReason = PatchReplacementFailureReason::BuildFailed;
        report_.message = assemblyResult.message;
        publishReport();
        return Result::error(report_.message);
    }
    report_.sourceFacesReplaced = true;
    append_warning(report_, assemblyResult.warning);

    const auto repairResult = repairPatchReplacementShape(assemblyResult.shape, PatchReplacementRepairOptions {});
    copy_repair_report(report_, repairResult.report);
    if (!repairResult.success) {
        report_.success = false;
        report_.failureReason = PatchReplacementFailureReason::BuildFailed;
        report_.message = repairResult.report.message;
        publishReport();
        return Result::error(report_.message);
    }

    afterDocument_ = ShapeDocument(repairResult.shape, beforeDocument_.sourcePath());
    if (!afterDocument_.hasShape()) {
        report_.success = false;
        report_.failureReason = PatchReplacementFailureReason::BuildFailed;
        report_.message = "Patch replacement command did not construct an after document after repair.";
        publishReport();
        return Result::error(report_.message);
    }

    StrictTopologyGateInput gateInput;
    gateInput.beforeDocument = &beforeDocument_;
    gateInput.afterDocument = &afterDocument_;
    gateInput.replacementReport = &report_;
    gateInput.allowMultiFaceReplacement = true;
    gateInput.allowFaceCountIncrease = true;
    gateInput.requireStepRoundtrip = true;
    gateInput.requireWatertightSolid = options_.requireWatertightSolidGate;
    gateInput.requireZeroFreeEdges = options_.requireZeroFreeEdges;
    gateInput.requireZeroMultipleEdges = options_.requireZeroMultipleEdges;
    gateInput.requireRoundtripWatertight = options_.requireRoundtripWatertight;

    const auto gateReport = StrictTopologyGate().evaluate(gateInput);
    copy_gate_report(report_, gateReport);
    append_warning(report_, gateReport.warningMessage);
    if (!gateReport.passed) {
        report_.success = false;
        report_.rollbackApplied = true;
        report_.failureReason = PatchReplacementFailureReason::GateFailed;
        report_.message = gate_failure_message(gateReport);
        publishReport();
        return Result::error(report_.message);
    }

    context.document = afterDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;

    report_.success = true;
    report_.rollbackApplied = false;
    report_.failureReason = PatchReplacementFailureReason::None;
    report_.message = "Patch replacement command committed after StrictTopologyGate passed.";
    executed_ = true;
    committed_ = true;
    publishReport();
    return Result::ok();
}

bool PatchReplacementCommand::undoable() const {
    return true;
}

Result PatchReplacementCommand::undo(CommandContext& context) {
    if (!committed_ || !beforeDocument_.hasShape()) {
        return Result::error("No committed patch replacement state to undo.");
    }

    context.document = beforeDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

Result PatchReplacementCommand::redo(CommandContext& context) {
    if (!committed_ || !afterDocument_.hasShape()) {
        return Result::error("No committed patch replacement state to redo.");
    }

    context.document = afterDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

const PatchReplacementReport& PatchReplacementCommand::report() const {
    return report_;
}

void PatchReplacementCommand::publishReport() {
    if (outReport_ != nullptr) {
        *outReport_ = report_;
    }
}

void PatchReplacementCommand::rebuildStableInputFromContext(const ShapeDocument& document) {
    documentSnapshot_ = document;
    input_.document = &documentSnapshot_;
}

}
