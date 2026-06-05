#include "command/PatchReplacementCommand.h"

#include "command/CommandContext.h"
#include "patch/BoundaryConstrainedPatchBuilder.h"
#include "patch/MultiFacePatchAnalyzer.h"
#include "validate/StrictTopologyGate.h"

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>

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

}

PatchReplacementCommand::PatchReplacementCommand(
    PatchReplacementInput input,
    PatchReplacementReport* outReport)
    : outReport_(outReport) {
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
    append_warning(report_, buildResult.warningMessage);

    if (!buildResult.success) {
        report_.success = false;
        report_.failureReason = map_build_failure(buildResult.failureReason);
        report_.message = buildResult.message;
        publishReport();
        return Result::error(report_.message);
    }

    afterDocument_ = ShapeDocument(input_.importedPatch->shape, beforeDocument_.sourcePath());
    append_warning(
        report_,
        "T6.3 uses the imported patch top-level shape as a gated afterDocument candidate; source-face deletion, sewing, ShapeFix, and SameParameter are deferred.");
    if (!afterDocument_.hasShape()) {
        report_.success = false;
        report_.failureReason = PatchReplacementFailureReason::BuildFailed;
        report_.message = "Patch replacement command did not construct an after document.";
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

    const auto gateReport = StrictTopologyGate().evaluate(gateInput);
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
