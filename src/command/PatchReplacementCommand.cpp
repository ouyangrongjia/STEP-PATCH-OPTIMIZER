#include "command/PatchReplacementCommand.h"

#include "command/CommandContext.h"
#include "patch/BoundaryConstrainedPatchBuilder.h"
#include "patch/MultiFacePatchAnalyzer.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchClosureDiagnostics.h"
#include "patch/PatchReplacementAssembler.h"
#include "patch/PatchReplacementRepair.h"
#include "patch/PatchTrimDiagnostics.h"
#include "validate/StrictTopologyGate.h"

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <filesystem>
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

std::string path_to_report_string(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.c_str()), utf8.size()};
}

void initialize_external_cad_diagnostics(
    PatchReplacementReport& report,
    const PatchReplacementInput& input) {
    auto& diagnostics = report.externalCadDiagnostics;
    diagnostics = {};
    diagnostics.captured = true;
    diagnostics.rawPatchPreflightRole = "PatchPreflightOnly";
    diagnostics.rawPatchPreflightExecuted = false;
    diagnostics.finalAppliedStepDiagnosticStage = "AppliedStepAfterSuccessfulApply";
    diagnostics.finalAppliedStepDiagnosticEligible = false;
    diagnostics.finalAppliedStepDiagnosticExecuted = false;

    if (input.artifactPaths != nullptr &&
        input.artifactPaths->foundStep &&
        !input.artifactPaths->patchStepPath.empty()) {
        diagnostics.rawPatchPreflightAvailable = true;
        diagnostics.rawPatchPreflightInputPath = path_to_report_string(input.artifactPaths->patchStepPath);
        diagnostics.rawPatchPreflightStatus = "NotConfigured";
        diagnostics.rawPatchPreflightMessage =
            "Raw Geomagic patch STEP can only be used as a pre-apply patch preflight; it does not validate the merged model.";
    } else {
        diagnostics.rawPatchPreflightAvailable = false;
        diagnostics.rawPatchPreflightStatus = "Skipped";
        diagnostics.rawPatchPreflightMessage = "No raw Geomagic patch STEP path was resolved.";
    }

    diagnostics.finalAppliedStepDiagnosticStatus = "Skipped";
    diagnostics.finalAppliedStepDiagnosticSkippedReason =
        "Final external CAD diagnostics require Patch Apply to pass and an applied STEP export to be written and read back.";
}

void mark_final_external_cad_diagnostic_after_gate(
    PatchReplacementReport& report,
    bool gatePassed) {
    auto& diagnostics = report.externalCadDiagnostics;
    diagnostics.captured = true;
    diagnostics.finalAppliedStepDiagnosticStage = "AppliedStepAfterSuccessfulApply";
    diagnostics.finalAppliedStepDiagnosticEligible = false;
    diagnostics.finalAppliedStepDiagnosticExecuted = false;
    diagnostics.finalAppliedStepDiagnosticInputPath.clear();

    if (gatePassed) {
        diagnostics.finalAppliedStepDiagnosticStatus = "WaitingForAppliedStepExport";
        diagnostics.finalAppliedStepDiagnosticSkippedReason =
            "Patch Apply passed; final external CAD diagnostics still require an applied STEP export path.";
        diagnostics.finalAppliedStepDiagnosticMessage =
            "Export the committed applied STEP first, then run the final external CAD diagnostic on that file.";
        return;
    }

    diagnostics.finalAppliedStepDiagnosticStatus = "Skipped";
    diagnostics.finalAppliedStepDiagnosticSkippedReason =
        "Patch Apply did not pass StrictTopologyGate; final external CAD diagnostics require an applied STEP, not the raw Geomagic patch STEP.";
    diagnostics.finalAppliedStepDiagnosticMessage.clear();
}

void copy_repair_report(
    PatchReplacementReport& report,
    const PatchReplacementRepairReport& repairReport) {
    static_cast<PatchReplacementRepairMetrics&>(report) = repairReport;
    report.repairApplied = true;
    report.shapeFixApplied = report.shapeFixShapeApplied || report.shapeFixFaceApplied || report.shapeFixWireApplied;
    report.repairRunCount += 1;

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
    input_.allowHighRiskPatchPreview = input.allowHighRiskPatchPreview;
    input_.strictOriginalBoundaryRetrim = input.strictOriginalBoundaryRetrim;
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
    initialize_external_cad_diagnostics(report_, input_);
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
    report_.attemptedMultiSurfaceBoundaryShell = buildResult.attemptedMultiSurfaceBoundaryShell;
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
    report_.retrimBoundaryEdgePcurveRebuildAttemptCount = buildResult.retrimBoundaryEdgePcurveRebuildAttemptCount;
    report_.retrimBoundaryEdgePcurveRebuildSuccessCount = buildResult.retrimBoundaryEdgePcurveRebuildSuccessCount;
    report_.retrimBoundaryEdgePcurveRebuildFailureCount = buildResult.retrimBoundaryEdgePcurveRebuildFailureCount;
    report_.retrimBoundaryEdgeSameParameterCheckCount = buildResult.retrimBoundaryEdgeSameParameterCheckCount;
    report_.retrimBoundaryEdgeSameParameterFailureCount = buildResult.retrimBoundaryEdgeSameParameterFailureCount;
    report_.retrimBoundaryEdgeMaxSameParameterDeviation = buildResult.retrimBoundaryEdgeMaxSameParameterDeviation;
    report_.retrimBoundaryEdgePcurveRebuildFailedEdgeIds = buildResult.retrimBoundaryEdgePcurveRebuildFailedEdgeIds;
    report_.retrimBoundaryEdgeSameParameterFailedEdgeIds = buildResult.retrimBoundaryEdgeSameParameterFailedEdgeIds;
    report_.multiSurfaceBoundarySampleCount = buildResult.multiSurfaceBoundarySampleCount;
    report_.multiSurfaceProjectedSampleCount = buildResult.multiSurfaceProjectedSampleCount;
    report_.multiSurfaceFailedProjectionCount = buildResult.multiSurfaceFailedProjectionCount;
    report_.multiSurfaceMaxProjectionDistance = buildResult.multiSurfaceMaxProjectionDistance;
    report_.multiSurfaceAverageProjectionDistance = buildResult.multiSurfaceAverageProjectionDistance;
    report_.multiSurfaceAssignedBoundarySegmentCount = buildResult.multiSurfaceAssignedBoundarySegmentCount;
    report_.multiSurfaceSplitBoundaryEdgeCount = buildResult.multiSurfaceSplitBoundaryEdgeCount;
    report_.multiSurfaceBuiltFaceCount = buildResult.multiSurfaceBuiltFaceCount;
    report_.multiSurfaceClosedWireCount = buildResult.multiSurfaceClosedWireCount;
    report_.multiSurfaceOpenWireCount = buildResult.multiSurfaceOpenWireCount;
    report_.multiSurfaceMultipleClosedWireFaceCount = buildResult.multiSurfaceMultipleClosedWireFaceCount;
    report_.multiSurfaceSkippedUnownedOpenWireFaceCount =
        buildResult.multiSurfaceSkippedUnownedOpenWireFaceCount;
    report_.multiSurfaceFailedPatchFaceIndex = buildResult.multiSurfaceFailedPatchFaceIndex;
    report_.multiSurfaceFailedFaceEdgeCount = buildResult.multiSurfaceFailedFaceEdgeCount;
    report_.multiSurfaceFailedFaceOriginalBoundarySegmentCount =
        buildResult.multiSurfaceFailedFaceOriginalBoundarySegmentCount;
    report_.multiSurfaceFailedFaceInternalEdgeCount =
        buildResult.multiSurfaceFailedFaceInternalEdgeCount;
    report_.multiSurfaceFailedOpenWireEdgeCount =
        buildResult.multiSurfaceFailedOpenWireEdgeCount;
    report_.multiSurfaceFailedOpenWireLength =
        buildResult.multiSurfaceFailedOpenWireLength;
    report_.multiSurfaceFailedOpenWireEndpointGap =
        buildResult.multiSurfaceFailedOpenWireEndpointGap;
    report_.multiSurfaceFailedOpenWireStartPointValid =
        buildResult.multiSurfaceFailedOpenWireStartPointValid;
    report_.multiSurfaceFailedOpenWireStartX =
        buildResult.multiSurfaceFailedOpenWireStartX;
    report_.multiSurfaceFailedOpenWireStartY =
        buildResult.multiSurfaceFailedOpenWireStartY;
    report_.multiSurfaceFailedOpenWireStartZ =
        buildResult.multiSurfaceFailedOpenWireStartZ;
    report_.multiSurfaceFailedOpenWireEndPointValid =
        buildResult.multiSurfaceFailedOpenWireEndPointValid;
    report_.multiSurfaceFailedOpenWireEndX =
        buildResult.multiSurfaceFailedOpenWireEndX;
    report_.multiSurfaceFailedOpenWireEndY =
        buildResult.multiSurfaceFailedOpenWireEndY;
    report_.multiSurfaceFailedOpenWireEndZ =
        buildResult.multiSurfaceFailedOpenWireEndZ;
    report_.multiSurfaceSelectedWireConnectTolerance =
        buildResult.multiSurfaceSelectedWireConnectTolerance;
    report_.multiSurfaceFallbackWireConnectAttempted =
        buildResult.multiSurfaceFallbackWireConnectAttempted;
    report_.multiSurfaceFallbackWireConnectSucceeded =
        buildResult.multiSurfaceFallbackWireConnectSucceeded;
    report_.multiSurfaceFailedEdgeIds = buildResult.multiSurfaceFailedEdgeIds;
    report_.multiSurfaceFailedFaceOriginalBoundaryEdgeIds =
        buildResult.multiSurfaceFailedFaceOriginalBoundaryEdgeIds;
    report_.multiSurfaceBoundaryEdgePcurveRebuildAttemptCount = buildResult.multiSurfaceBoundaryEdgePcurveRebuildAttemptCount;
    report_.multiSurfaceBoundaryEdgePcurveRebuildSuccessCount = buildResult.multiSurfaceBoundaryEdgePcurveRebuildSuccessCount;
    report_.multiSurfaceBoundaryEdgePcurveRebuildFailureCount = buildResult.multiSurfaceBoundaryEdgePcurveRebuildFailureCount;
    report_.multiSurfaceBoundaryEdgeSameParameterCheckCount = buildResult.multiSurfaceBoundaryEdgeSameParameterCheckCount;
    report_.multiSurfaceBoundaryEdgeSameParameterFailureCount = buildResult.multiSurfaceBoundaryEdgeSameParameterFailureCount;
    report_.multiSurfaceBoundaryEdgeMaxSameParameterDeviation = buildResult.multiSurfaceBoundaryEdgeMaxSameParameterDeviation;
    report_.multiSurfaceBoundaryEdgePcurveRebuildFailedEdgeIds = buildResult.multiSurfaceBoundaryEdgePcurveRebuildFailedEdgeIds;
    report_.multiSurfaceBoundaryEdgeSameParameterFailedEdgeIds = buildResult.multiSurfaceBoundaryEdgeSameParameterFailedEdgeIds;
    append_warning(report_, buildResult.warningMessage);

    if (!buildResult.success) {
        report_.success = false;
        report_.failureReason = map_build_failure(buildResult.failureReason);
        report_.message = buildResult.message;
        publishReport();
        return Result::error(report_.message);
    }

    PatchTrimDiagnosticsInput trimInput;
    trimInput.beforeDocument = &beforeDocument_;
    trimInput.candidate = input_.candidate;
    trimInput.boundary = input_.boundary;
    trimInput.replacementShape = &buildResult.replacementShape;
    PatchTrimDiagnosticsOptions trimOptions;
    trimOptions.boundarySamplesPerEdge = 5;
    trimOptions.surfaceGridDivisions = 3;
    trimOptions.maxBoundarySamples = 160;
    trimOptions.maxSurfaceSamples = 256;
    report_.trimDiagnostics = PatchTrimDiagnostics().analyze(trimInput, trimOptions);

    const auto assemblyResult = PatchReplacementAssembler().assemble(
        beforeDocument_,
        *input_.boundary,
        buildResult,
        input_.strictOriginalBoundaryRetrim);
    if (!assemblyResult.success) {
        report_.success = false;
        report_.failureReason = PatchReplacementFailureReason::BuildFailed;
        report_.message = assemblyResult.message;
        publishReport();
        return Result::error(report_.message);
    }
    report_.sourceFacesReplaced = true;
    append_warning(report_, assemblyResult.warning);

    const PatchClosureDiagnostics closureDiagnostics;
    const auto preRepairClosure = closureDiagnostics.capture(assemblyResult.shape);
    closureDiagnostics.copyPreRepairReport(report_, preRepairClosure);

    PatchReplacementRepairOptions repairOptions;
    if (buildResult.usedMultiSurfaceBoundaryShell &&
        buildResult.multiSurfaceMaxProjectionDistance > repairOptions.maxSewingTolerance) {
        const auto measuredBoundaryTolerance = buildResult.multiSurfaceMaxProjectionDistance * 2.0;
        repairOptions.maxSewingTolerance = std::min(0.25, measuredBoundaryTolerance);
        repairOptions.preferredSewingTolerance = repairOptions.maxSewingTolerance;
        append_warning(report_, "Multi-surface boundary shell raised repair sewing tolerance to cover measured original-boundary projection deviation.");
    }
    const auto repairResult = repairPatchReplacementShape(assemblyResult.shape, repairOptions);
    copy_repair_report(report_, repairResult.report);
    const auto postRepairClosure = closureDiagnostics.capture(repairResult.shape);
    closureDiagnostics.copyPostRepairReport(report_, postRepairClosure);
    report_.freeEdgeDiagnostics = closureDiagnostics.buildFreeEdgeDiagnostics(
        preRepairClosure,
        postRepairClosure,
        beforeDocument_,
        *input_.boundary,
        buildResult);
    report_.appearedAfterRepairDegeneratedFreeEdgeCount = static_cast<int>(std::count_if(
        report_.freeEdgeDiagnostics.begin(),
        report_.freeEdgeDiagnostics.end(),
        [](const PatchReplacementFreeEdgeDiagnostic& diagnostic) {
            return diagnostic.afterRepair &&
                diagnostic.appearedAfterRepair &&
                diagnostic.degenerated;
        }));
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
    gateInput.replacementFaceCount = report_.replacementFaceCount;
    gateInput.allowMultiFaceReplacement = true;
    gateInput.allowFaceCountIncrease = true;
    gateInput.requireStepRoundtrip = true;
    gateInput.requireWatertightSolid = options_.requireWatertightSolidGate;
    gateInput.requireZeroFreeEdges = options_.requireZeroFreeEdges;
    gateInput.requireZeroMultipleEdges = options_.requireZeroMultipleEdges;
    gateInput.requireRoundtripWatertight = options_.requireRoundtripWatertight;

    const auto gateReport = StrictTopologyGate().evaluate(gateInput);
    copy_gate_report(report_, gateReport);
    report_.trimDiagnostics.roundtripCompared = gateReport.stepRoundtripOk;
    report_.trimDiagnostics.roundtripChanged =
        gateReport.stepRoundtripOk &&
        (gateReport.roundtripStats.faces != gateReport.afterStats.faces ||
            gateReport.roundtripStats.edges != gateReport.afterStats.edges ||
            gateReport.roundtripStats.shells != gateReport.afterStats.shells ||
            gateReport.roundtripStats.solids != gateReport.afterStats.solids ||
            gateReport.roundtripFreeEdges != gateReport.afterFreeEdges ||
            gateReport.roundtripMultipleEdges != gateReport.afterMultipleEdges ||
            gateReport.roundtripBRepCheckValid != gateReport.afterBRepCheckValid);
    append_warning(report_, gateReport.warningMessage);
    if (!gateReport.passed) {
        mark_final_external_cad_diagnostic_after_gate(report_, false);
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
    mark_final_external_cad_diagnostic_after_gate(report_, true);
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

const ShapeDocument& PatchReplacementCommand::afterDocument() const {
    return afterDocument_;
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
