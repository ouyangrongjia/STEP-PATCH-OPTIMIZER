#include "app/AppController.h"
#include "app/PatchPreviewWorkflow.h"

#include "command/DetectFeatureCommand.h"
#include "command/ExportStepCommand.h"
#include "command/LoadStepCommand.h"
#include "command/LockedEdgeRef.h"
#include "command/LockEdgeCommand.h"
#include "command/MergePatchCommand.h"
#include "command/PatchReplacementCommand.h"
#include "command/ReduceSelectedFacesCommand.h"
#include "command/UnlockEdgeCommand.h"
#include "command/ValidateShapeCommand.h"
#include "external/geomagic/GeomagicAutoSurfaceBackend.h"
#include "external/geomagic/GeomagicOutputPathResolver.h"
#include "io/StlReader.h"
#include "io/StlWriter.h"
#include "merge/MergePlanner.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/CropBoundaryDiagnostics.h"
#include "patch/PatchImportService.h"
#include "merge/BoundaryWireBuilder.h"
#include "stl/StlCutChainCutter.h"

#include <BRep_Tool.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace spo {

namespace {

std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension;
}

void publish_apply_failure(
    PatchReplacementReport* outReport,
    PatchReplacementFailureReason reason,
    const MergeCandidate* candidate,
    std::string message) {
    if (outReport == nullptr) {
        return;
    }
    *outReport = {};
    outReport->success = false;
    outReport->failureReason = reason;
    outReport->candidateId = candidate != nullptr ? candidate->candidate_id : -1;
    outReport->sourceFaceCount = candidate != nullptr
        ? (candidate->face_count > 0 ? candidate->face_count : static_cast<int>(candidate->faces.size()))
        : 0;
    outReport->sourceBoundaryEdgeCount = candidate != nullptr
        ? (candidate->boundary_edge_count > 0 ? candidate->boundary_edge_count : static_cast<int>(candidate->boundary_edges.size()))
        : 0;
    outReport->message = std::move(message);
}

}

const char* AppController::applicationName() const {
    return kApplicationName;
}

Result AppController::execute(std::unique_ptr<Command> command) {
    return history_.execute(std::move(command), context_);
}

Result AppController::undo() {
    const auto result = history_.undo(context_);
    if (result.success()) {
        clearCurrentPatchOverlay();
        clearCurrentSurfaceReductionPreview();
        patchWorkflow_.processStatus = makeProcessStatus(
            ProcessStage::CachedUndo,
            "cached undo applied; Geomagic / STL crop / patch import / repair were not rerun.");
    }
    return result;
}

Result AppController::redo() {
    const auto result = history_.redo(context_);
    if (result.success()) {
        clearCurrentPatchOverlay();
        clearCurrentSurfaceReductionPreview();
        patchWorkflow_.processStatus = makeProcessStatus(
            ProcessStage::CachedRedo,
            "cached redo applied; Geomagic / STL crop / patch import / repair were not rerun.");
    }
    return result;
}

bool AppController::canUndo() const {
    return history_.canUndo();
}

bool AppController::canRedo() const {
    return history_.canRedo();
}

Result AppController::openStepFile(const std::filesystem::path& path) {
    const auto result = execute(std::make_unique<LoadStepCommand>(path));
    if (result.success()) {
        history_.clear();
        context_.lockedEdges.clear();
        sourceStlMesh_.clear();
        sourceStlPath_.clear();
        clearCurrentPatchOverlay();
        clearCurrentSurfaceReductionPreview();
        patchWorkflow_.processStatus = makeProcessStatus(ProcessStage::Idle, "STEP/STP loaded; patch preview cleared.");
    }
    return result;
}

Result AppController::exportStepFile(const std::filesystem::path& path) {
    return execute(std::make_unique<ExportStepCommand>(path));
}

Result AppController::verifyStepFileReadable(const std::filesystem::path& path) {
    CommandContext readContext;
    LoadStepCommand command(path);
    return command.execute(readContext);
}

Result AppController::openStlFile(const std::filesystem::path& path) {
    const auto result = StlReader().read(path);
    if (!result.success) {
        return Result::error(result.message);
    }

    sourceStlMesh_ = result.mesh;
    sourceStlPath_ = path;
    patchWorkflow_.processStatus = makeProcessStatus(ProcessStage::Idle, "Source STL loaded.");
    return Result::ok();
}

bool AppController::hasSourceStl() const {
    return !sourceStlMesh_.empty();
}

const std::filesystem::path& AppController::sourceStlPath() const {
    return sourceStlPath_;
}

const StlMesh& AppController::sourceStlMesh() const {
    return sourceStlMesh_;
}

std::size_t AppController::sourceStlTriangleCount() const {
    return sourceStlMesh_.triangleCount();
}

StlBoundingBox AppController::sourceStlBoundingBox() const {
    return sourceStlMesh_.boundingBox();
}

StlCandidateCropResult AppController::cropStlForCandidate(
    const MergeCandidate& candidate,
    const std::filesystem::path& outputPath,
    const StlRegionExtractorOptions& options) const {
    return cropStlForCandidateData(context_.document, sourceStlMesh_, candidate, outputPath, options);
}

StlCandidateCropResult AppController::cropStlForCandidateData(
    const ShapeDocument& document,
    const StlMesh& sourceMesh,
    const MergeCandidate& candidate,
    const std::filesystem::path& outputPath,
    const StlRegionExtractorOptions& options) {
    return PatchPreviewWorkflow::crop(
        document,
        sourceMesh,
        candidate,
        outputPath,
        options);
}

SurfaceReductionResult AppController::reduceSelectedFacesData(
    const ShapeDocument& document,
    const SurfaceReductionRequest& request) {
    return SurfaceReductionEngine().reduce(document, request);
}

void AppController::setCurrentSurfaceReductionPreview(SurfaceReductionResult result) {
    currentSurfaceReductionResult_ = std::move(result);
}

void AppController::clearCurrentSurfaceReductionPreview() {
    currentSurfaceReductionResult_ = {};
}

bool AppController::surfaceReductionPreviewReady() const {
    return currentSurfaceReductionResult_.canApply();
}

const SurfaceReductionResult& AppController::currentSurfaceReductionResult() const {
    return currentSurfaceReductionResult_;
}

Result AppController::applyCurrentSurfaceReduction() {
    if (!surfaceReductionPreviewReady()) {
        return Result::error("当前曲面减面预览未通过全部质量门。");
    }
    const auto result = execute(std::make_unique<ReduceSelectedFacesCommand>(currentSurfaceReductionResult_));
    if (result.success()) {
        clearCurrentSurfaceReductionPreview();
        clearCurrentPatchOverlay();
    }
    return result;
}

PatchPreviewPipelineResult AppController::cropAndRunGeomagicForCandidateData(
    const ShapeDocument& document,
    const StlMesh& sourceMesh,
    const MergeCandidate& candidate,
    const std::filesystem::path& workspaceRoot,
    GeomagicAutoSurfaceConfig config,
    const StlRegionExtractorOptions& options) {
    return PatchPreviewWorkflow::cropAndRunGeomagic(
        document,
        sourceMesh,
        candidate,
        workspaceRoot,
        std::move(config),
        options);
}

PatchPreviewPipelineResult AppController::generateFittingStlAndRunGeomagicForCandidateData(
    const ShapeDocument& document,
    const StlMesh& sourceMesh,
    const MergeCandidate& candidate,
    const std::filesystem::path& workspaceRoot,
    GeomagicAutoSurfaceConfig config,
    GeomagicFittingInputMode fittingMode,
    const StlRegionExtractorOptions& cropOptions,
    const StpSampledFittingOptions& samplingOptions,
    PatchPreviewProgressCallback progress,
    PatchPreviewRunLogger runLogger) {
    return PatchPreviewWorkflow::generateFittingStlAndRunGeomagic(
        document,
        sourceMesh,
        candidate,
        workspaceRoot,
        std::move(config),
        fittingMode,
        cropOptions,
        samplingOptions,
        std::move(progress),
        std::move(runLogger));
}

FeatureEdgeDetectionResult AppController::detectFeatureEdges(double angularThresholdDegrees, double minEdgeLength) {
    const auto result = execute(std::make_unique<DetectFeatureCommand>(angularThresholdDegrees, minEdgeLength));
    if (!result.success()) {
        return {};
    }
    return context_.featureEdges;
}

MergePlannerResult AppController::previewMergeCandidates(
    double angularThresholdDegrees,
    double minEdgeLength,
    const MergePlannerOptions& options) {
    if (!context_.document.hasShape()) {
        return {};
    }

    FeatureEdgeDetector detector;
    context_.featureEdges = detector.detect(context_.document.topology(), angularThresholdDegrees, minEdgeLength);

    MergePlanner planner;
    return planner.plan(context_.document, context_.featureEdges, lockedEdges(), options);
}

SameDomainUnifyResult AppController::unifySameDomain(
    double angularThresholdDegrees,
    double minEdgeLength,
    double linearTolerance,
    bool concatBsplines) {
    auto command = std::make_unique<MergePatchCommand>(
        angularThresholdDegrees,
        minEdgeLength,
        linearTolerance,
        concatBsplines);
    auto* commandPtr = command.get();
    const auto status = execute(std::move(command));
    if (!status.success()) {
        return {};
    }
    const auto result = commandPtr->result();
    if (result.document.hasShape()) {
        clearCurrentPatchOverlay();
        clearCurrentSurfaceReductionPreview();
    }
    return result;
}

Result AppController::importPatchForCurrentCandidateFromLocalStl(
    const std::filesystem::path& localStlPath,
    const MergeCandidate* candidate) {
    ProcessStatusSnapshot importing = makeProcessStatus(ProcessStage::ImportingPatch, "Importing patch from local STL artifacts.");
    importing.localStlPath = localStlPath;
    if (candidate != nullptr) {
        importing.candidateId = candidate->candidate_id;
        importing.sourceFaceCount = candidate->face_count > 0
            ? candidate->face_count
            : static_cast<int>(candidate->faces.size());
        importing.boundaryEdgeCount = candidate->boundary_edge_count > 0
            ? candidate->boundary_edge_count
            : static_cast<int>(candidate->boundary_edges.size());
    }
    patchWorkflow_.processStatus = std::move(importing);

    const auto artifacts = PatchArtifactLocator().locateFromLocalStl(localStlPath);
    if (!artifacts.success) {
        clearCurrentPatchOverlay();
        publishProcessStatusForCandidate(ProcessStage::ImportingPatch, candidate, artifacts.message);
        return Result::error(artifacts.message);
    }

    const auto importPath = artifacts.foundStep
        ? artifacts.patchStepPath
        : artifacts.patchIgesSidecarPath;
    auto imported = PatchImportService().importPatch(importPath);
    if (!imported.success) {
        clearCurrentPatchOverlay();
        const auto message = imported.errorMessage.empty() ? imported.message : imported.errorMessage;
        publishProcessStatusForCandidate(ProcessStage::ImportingPatch, candidate, message);
        return Result::error(message);
    }

    patchWorkflow_.artifactPaths = artifacts;
    patchWorkflow_.importedPatch = std::move(imported);
    patchWorkflow_.previewReport = buildPatchPreviewReport(
        hasDocument() ? &context_.document : nullptr,
        candidate,
        patchWorkflow_.importedPatch,
        patchWorkflow_.artifactPaths);
    patchWorkflow_.previewReady = patchWorkflow_.previewReport.success;
    updateCurrentPatchApplyState();
    publishProcessStatusForCandidate(
        ProcessStage::PreviewReady,
        candidate,
        patchWorkflow_.statusMessage,
        patchWorkflow_.previewReport.warningMessage);
    return Result::ok();
}

Result AppController::importPatchResultForCurrentCandidate(
    const GeomagicAutoSurfaceResult& result,
    const MergeCandidate* candidate) {
    ProcessStatusSnapshot importing = makeProcessStatus(ProcessStage::ImportingPatch, "Importing patch from Geomagic result.");
    importing.localStlPath = result.inputStlPath;
    importing.patchStepPath = result.outputStepPath;
    importing.patchIgesPath = result.outputIgesPath;
    importing.fitRegionLogPath = result.fitRegionLogPath;
    if (candidate != nullptr) {
        importing.candidateId = candidate->candidate_id;
        importing.sourceFaceCount = candidate->face_count > 0
            ? candidate->face_count
            : static_cast<int>(candidate->faces.size());
        importing.boundaryEdgeCount = candidate->boundary_edge_count > 0
            ? candidate->boundary_edge_count
            : static_cast<int>(candidate->boundary_edges.size());
    }
    patchWorkflow_.processStatus = std::move(importing);

    const auto artifacts = PatchArtifactLocator().locateFromResult(result);
    if (!artifacts.success) {
        clearCurrentPatchOverlay();
        publishProcessStatusForCandidate(ProcessStage::ImportingPatch, candidate, artifacts.message);
        return Result::error(artifacts.message);
    }

    const auto importPath = artifacts.foundStep
        ? artifacts.patchStepPath
        : artifacts.patchIgesSidecarPath;
    auto imported = PatchImportService().importPatch(importPath);
    if (!imported.success) {
        clearCurrentPatchOverlay();
        const auto message = imported.errorMessage.empty() ? imported.message : imported.errorMessage;
        publishProcessStatusForCandidate(ProcessStage::ImportingPatch, candidate, message);
        return Result::error(message);
    }

    patchWorkflow_.artifactPaths = artifacts;
    patchWorkflow_.importedPatch = std::move(imported);
    patchWorkflow_.previewReport = buildPatchPreviewReport(
        hasDocument() ? &context_.document : nullptr,
        candidate,
        patchWorkflow_.importedPatch,
        patchWorkflow_.artifactPaths);
    patchWorkflow_.previewReady = patchWorkflow_.previewReport.success;
    updateCurrentPatchApplyState();
    publishProcessStatusForCandidate(
        ProcessStage::PreviewReady,
        candidate,
        patchWorkflow_.statusMessage,
        patchWorkflow_.previewReport.warningMessage);
    return Result::ok();
}

Result AppController::importPatchFromFileForCurrentCandidate(
    const std::filesystem::path& patchPath,
    const MergeCandidate* candidate) {
    ProcessStatusSnapshot importing = makeProcessStatus(ProcessStage::ImportingPatch, "Importing patch from selected file.");
    if (candidate != nullptr) {
        importing.candidateId = candidate->candidate_id;
        importing.sourceFaceCount = candidate->face_count > 0
            ? candidate->face_count
            : static_cast<int>(candidate->faces.size());
        importing.boundaryEdgeCount = candidate->boundary_edge_count > 0
            ? candidate->boundary_edge_count
            : static_cast<int>(candidate->boundary_edges.size());
    }

    PatchArtifactPaths artifacts;
    artifacts.success = true;
    const auto extension = lowercase_extension(patchPath);
    if (extension == ".igs" || extension == ".iges") {
        artifacts.patchIgesSidecarPath = patchPath;
        artifacts.foundIgesSidecar = true;
        importing.patchIgesPath = patchPath;
    } else {
        artifacts.patchStepPath = patchPath;
        artifacts.foundStep = true;
        importing.patchStepPath = patchPath;
    }
    patchWorkflow_.processStatus = std::move(importing);

    auto imported = PatchImportService().importPatch(patchPath);
    if (!imported.success) {
        clearCurrentPatchOverlay();
        const auto message = imported.errorMessage.empty() ? imported.message : imported.errorMessage;
        publishProcessStatusForCandidate(ProcessStage::ImportingPatch, candidate, message);
        return Result::error(message);
    }

    patchWorkflow_.artifactPaths = artifacts;
    patchWorkflow_.importedPatch = std::move(imported);
    patchWorkflow_.previewReport = buildPatchPreviewReport(
        hasDocument() ? &context_.document : nullptr,
        candidate,
        patchWorkflow_.importedPatch,
        patchWorkflow_.artifactPaths);
    patchWorkflow_.previewReady = patchWorkflow_.previewReport.success;
    updateCurrentPatchApplyState();
    publishProcessStatusForCandidate(
        ProcessStage::PreviewReady,
        candidate,
        patchWorkflow_.statusMessage,
        patchWorkflow_.previewReport.warningMessage);
    return Result::ok();
}

void AppController::clearCurrentPatchOverlay() {
    patchWorkflow_.clear();
}

bool AppController::patchPreviewReady() const {
    return patchWorkflow_.previewReady;
}

const PatchArtifactPaths& AppController::currentPatchArtifactPaths() const {
    return patchWorkflow_.artifactPaths;
}

const ImportedPatchInfo& AppController::currentImportedPatchInfo() const {
    return patchWorkflow_.importedPatch;
}

const PatchPreviewReport& AppController::currentPatchPreviewReport() const {
    return patchWorkflow_.previewReport;
}

RegionPatchStatus AppController::currentPatchStatus() const {
    return patchWorkflow_.status;
}

const std::string& AppController::currentPatchStatusMessage() const {
    return patchWorkflow_.statusMessage;
}

const ProcessStatusSnapshot& AppController::currentProcessStatus() const {
    return patchWorkflow_.processStatus;
}

void AppController::updateProcessStatus(ProcessStatusSnapshot status) {
    patchWorkflow_.processStatus = std::move(status);
}

PatchApplyDecision AppController::currentPatchApplyDecision() const {
    return patchWorkflow_.applyDecision();
}

Result AppController::requestApplyCurrentPatchPreview() {
    const auto decision = currentPatchApplyDecision();
    if (!decision.canRequestApply) {
        patchWorkflow_.status = decision.status;
        patchWorkflow_.statusMessage = decision.reason;
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, nullptr, decision.reason);
        return Result::error(decision.reason);
    }

    patchWorkflow_.status = RegionPatchStatus::ApplyFailed;
    patchWorkflow_.statusMessage = "Patch Apply requires an explicit current candidate.";
    publishProcessStatusForCandidate(ProcessStage::ApplyFailed, nullptr, patchWorkflow_.statusMessage);
    return Result::error("Patch Apply requires an explicit current candidate.");
}

Result AppController::applyCurrentPatchToCurrentCandidate(
    const MergeCandidate& candidate,
    PatchReplacementReport* outReport,
    PatchPreviewProgressCallback progress) {
    auto emitProgress = [&]() {
        if (progress) {
            progress(patchWorkflow_.processStatus);
        }
    };

    publishProcessStatusForCandidate(
        ProcessStage::ApplyingPatch,
        &candidate,
        "Patch Apply started.");
    emitProgress();

    if (!hasDocument()) {
        const std::string message = "Open a STEP/STP document before applying a patch.";
        patchWorkflow_.status = RegionPatchStatus::ApplyFailed;
        patchWorkflow_.statusMessage = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::MissingDocument, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        emitProgress();
        return Result::error(message);
    }

    const auto decision = currentPatchApplyDecision();
    if (!decision.canRequestApply) {
        patchWorkflow_.status = decision.status;
        patchWorkflow_.statusMessage = decision.reason;
        publish_apply_failure(outReport, PatchReplacementFailureReason::MissingPreviewReport, &candidate, decision.reason);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, decision.reason);
        emitProgress();
        return Result::error(decision.reason);
    }
    if (candidate.faces.empty()) {
        const std::string message = "Current candidate has no source faces.";
        patchWorkflow_.status = RegionPatchStatus::ApplyFailed;
        patchWorkflow_.statusMessage = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::MissingCandidate, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        emitProgress();
        return Result::error(message);
    }
    if (candidate.candidate_type != MergeCandidateType::FeatureBoundedRefit) {
        const std::string message = "Current candidate is not FeatureBoundedRefit.";
        patchWorkflow_.status = RegionPatchStatus::ApplyFailed;
        patchWorkflow_.statusMessage = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::UnsupportedCandidate, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        emitProgress();
        return Result::error(message);
    }
    if (candidate.status == MergeCandidateStatus::Rejected || candidate.status == MergeCandidateStatus::Hidden) {
        const std::string message = "Current candidate is rejected or hidden.";
        patchWorkflow_.status = RegionPatchStatus::ApplyFailed;
        patchWorkflow_.statusMessage = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::UnsupportedCandidate, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        emitProgress();
        return Result::error(message);
    }

    const auto candidateFaceCount = candidate.face_count > 0
        ? candidate.face_count
        : static_cast<int>(candidate.faces.size());
    if (patchWorkflow_.previewReport.candidateId != candidate.candidate_id ||
        patchWorkflow_.previewReport.sourceFaceCount != candidateFaceCount) {
        const std::string message = "Patch preview does not match current candidate.";
        patchWorkflow_.status = RegionPatchStatus::ApplyFailed;
        patchWorkflow_.statusMessage = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::UnsupportedCandidate, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        emitProgress();
        return Result::error(message);
    }

    publishProcessStatusForCandidate(
        ProcessStage::AnalyzingBoundary,
        &candidate,
        "Analyzing original CAD boundary before Patch Apply.");
    emitProgress();
    const auto boundary = RegionBoundaryAnalyzer().analyze(context_.document, candidate);
    if (!boundary.valid ||
        boundary.connected_component_count != 1 ||
        boundary.outer_wire_count != 1 ||
        boundary.inner_wire_count != 0 ||
        !boundary.boundary_closed ||
        boundary.has_holes ||
        boundary.has_non_manifold_edges ||
        boundary.has_branching_boundary ||
        boundary.ordered_boundary_edges.empty()) {
        const auto message = boundary.message.empty()
            ? std::string("Candidate boundary is not a valid single closed outer wire.")
            : std::string("Candidate boundary is not valid for patch apply: ") + boundary.message;
        patchWorkflow_.status = RegionPatchStatus::ApplyFailed;
        patchWorkflow_.statusMessage = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::InvalidBoundary, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        emitProgress();
        return Result::error(message);
    }

    PatchReplacementInput input;
    input.document = &context_.document;
    input.candidate = &candidate;
    input.boundary = &boundary;
    input.importedPatch = &patchWorkflow_.importedPatch;
    input.artifactPaths = &patchWorkflow_.artifactPaths;
    input.previewReport = &patchWorkflow_.previewReport;
    input.strictOriginalBoundaryRetrim = true;

    PatchReplacementCommandOptions options;
    options.requireWatertightSolidGate = true;
    options.requireZeroFreeEdges = true;
    options.requireZeroMultipleEdges = true;
    options.requireRoundtripWatertight = true;

    PatchReplacementReport localReport;
    auto* reportTarget = outReport != nullptr ? outReport : &localReport;
    auto command = std::make_unique<PatchReplacementCommand>(input, reportTarget, options);
    publishProcessStatusForCandidate(
        ProcessStage::BuildingReplacement,
        &candidate,
        "Building replacement and running repair before StrictTopologyGate.");
    emitProgress();
    const auto result = execute(std::move(command));
    const auto& finalReport = *reportTarget;
    if (!result.success()) {
        patchWorkflow_.status = RegionPatchStatus::ApplyFailed;
        patchWorkflow_.statusMessage = !finalReport.message.empty()
            ? finalReport.message
            : result.message();
        publishProcessStatusFromReplacementReport(ProcessStage::ApplyFailed, finalReport, patchWorkflow_.statusMessage);
        emitProgress();
        return result;
    }

    publishProcessStatusFromReplacementReport(
        ProcessStage::Applied,
        finalReport,
        "Patch Apply completed and committed after StrictTopologyGate passed.");
    emitProgress();
    patchWorkflow_.artifactPaths = {};
    patchWorkflow_.importedPatch = {};
    patchWorkflow_.previewReport = {};
    patchWorkflow_.previewReady = false;
    patchWorkflow_.status = RegionPatchStatus::Applied;
    patchWorkflow_.statusMessage = "Patch Apply completed and committed after StrictTopologyGate passed.";
    clearCurrentSurfaceReductionPreview();
    return Result::ok();
}

CropBoundaryDiagnosticsReport AppController::diagnoseCropBoundaryForCurrentPatch(
    const MergeCandidate& candidate,
    const StlMesh* localStlMesh,
    const CropBoundaryDiagnosticsOptions& options) const {
    if (!patchWorkflow_.previewReady || patchWorkflow_.importedPatch.shape.IsNull()) {
        CropBoundaryDiagnosticsReport report;
        report.message = "Crop boundary diagnostics requires a ready imported patch preview.";
        return report;
    }
    return diagnoseCropBoundaryData(
        context_.document,
        candidate,
        localStlMesh,
        patchWorkflow_.importedPatch.shape,
        options,
        sourceStlMesh_.empty() ? nullptr : &sourceStlMesh_);
}

CropBoundaryDiagnosticsReport AppController::diagnoseCropBoundaryData(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const StlMesh* localStlMesh,
    const TopoDS_Shape& importedPatchShape,
    const CropBoundaryDiagnosticsOptions& options,
    const StlMesh* sourceStlMesh) {
    const auto boundary = RegionBoundaryAnalyzer().analyze(document, candidate);
    CropBoundaryDiagnosticsInput input;
    input.document = &document;
    input.candidate = &candidate;
    input.boundary = &boundary;
    input.localStlMesh = localStlMesh;
    input.sourceStlMesh = sourceStlMesh;
    input.importedPatchShape = &importedPatchShape;
    return CropBoundaryDiagnostics().analyze(input, options);
}

bool AppController::hasDocument() const {
    return context_.document.hasShape();
}

const ShapeDocument& AppController::document() const {
    return context_.document;
}

const FeatureEdgeDetectionResult& AppController::featureEdges() const {
    return context_.featureEdges;
}

ShapeValidationReport AppController::validateShape() {
    const auto result = execute(std::make_unique<ValidateShapeCommand>());
    if (!result.success()) {
        return {};
    }
    return context_.validationReport;
}

Result AppController::lockEdges(const std::vector<EdgeId>& edgeIds) {
    const auto result = execute(std::make_unique<LockEdgeCommand>(edgeIds));
    if (result.success()) {
        clearCurrentSurfaceReductionPreview();
    }
    return result;
}

Result AppController::unlockEdges(const std::vector<EdgeId>& edgeIds) {
    const auto result = execute(std::make_unique<UnlockEdgeCommand>(edgeIds));
    if (result.success()) {
        clearCurrentSurfaceReductionPreview();
    }
    return result;
}

std::set<EdgeId> AppController::lockedEdges() const {
    return lockedEdgeIds(context_.lockedEdges);
}

const CommandHistory& AppController::history() const {
    return history_;
}

void AppController::publishProcessStatusForCandidate(
    ProcessStage stage,
    const MergeCandidate* candidate,
    std::string message,
    std::string warning) {
    ProcessStatusSnapshot snapshot;
    snapshot.stage = stage;
    snapshot.latestMessage = std::move(message);
    snapshot.latestWarning = std::move(warning);
    snapshot.localStlPath = patchWorkflow_.artifactPaths.localStlPath;
    snapshot.patchStepPath = patchWorkflow_.artifactPaths.patchStepPath;
    snapshot.patchIgesPath = patchWorkflow_.artifactPaths.patchIgesSidecarPath;
    snapshot.fitRegionLogPath = patchWorkflow_.artifactPaths.fitRegionLogPath;

    if (candidate != nullptr) {
        snapshot.candidateId = candidate->candidate_id;
        snapshot.sourceFaceCount = candidate->face_count > 0
            ? candidate->face_count
            : static_cast<int>(candidate->faces.size());
        snapshot.boundaryEdgeCount = candidate->boundary_edge_count > 0
            ? candidate->boundary_edge_count
            : static_cast<int>(candidate->boundary_edges.size());
    } else if (patchWorkflow_.previewReport.candidateId >= 0) {
        snapshot.candidateId = patchWorkflow_.previewReport.candidateId;
        snapshot.sourceFaceCount = patchWorkflow_.previewReport.sourceFaceCount;
        snapshot.boundaryEdgeCount = patchWorkflow_.previewReport.sourceBoundaryEdgeCount;
    }

    patchWorkflow_.processStatus = std::move(snapshot);
}

void AppController::publishProcessStatusFromReplacementReport(
    ProcessStage stage,
    const PatchReplacementReport& report,
    std::string fallbackMessage) {
    ProcessStatusSnapshot snapshot;
    snapshot.stage = stage;
    snapshot.candidateId = report.candidateId;
    snapshot.sourceFaceCount = report.sourceFaceCount;
    snapshot.boundaryEdgeCount = report.sourceBoundaryEdgeCount;
    snapshot.localStlPath = patchWorkflow_.artifactPaths.localStlPath;
    snapshot.patchStepPath = patchWorkflow_.artifactPaths.patchStepPath;
    snapshot.patchIgesPath = patchWorkflow_.artifactPaths.patchIgesSidecarPath;
    snapshot.fitRegionLogPath = patchWorkflow_.artifactPaths.fitRegionLogPath;

    snapshot.selectedSewingTolerance = report.selectedSewingTolerance;
    snapshot.sewingAttemptCount = report.sewingAttemptCount;
    snapshot.sewingAttemptIndex = report.sewingAttemptCount;
    snapshot.bestFreeEdges = report.bestSewingFreeEdges;
    snapshot.bestMultipleEdges = report.bestSewingMultipleEdges;
    snapshot.bestFaceCount = report.bestSewingFaceCount;
    snapshot.bestEdgeCount = report.bestSewingEdgeCount;
    snapshot.bestShellCount = report.bestSewingShellCount;
    snapshot.bestSolidCount = report.bestSewingSolidCount;
    snapshot.bestBRepCheckValid = report.bestSewingBRepCheckValid;
    snapshot.repairRunCount = report.repairRunCount;
    snapshot.adaptiveSewingApplied = report.adaptiveSewingApplied;
    snapshot.repairApplied = report.repairApplied;
    snapshot.gateEvaluated = report.gateEvaluated;
    snapshot.gatePassed = report.gatePassed;
    snapshot.gateBeforeBRepCheckValid = report.gateBeforeBRepCheckValid;
    snapshot.gateAfterBRepCheckValid = report.gateAfterBRepCheckValid;
    snapshot.gateRoundtripBRepCheckValid = report.gateRoundtripBRepCheckValid;
    snapshot.gateBeforeFaceCount = report.gateBeforeFaceCount;
    snapshot.gateBeforeEdgeCount = report.gateBeforeEdgeCount;
    snapshot.gateBeforeShellCount = report.gateBeforeShellCount;
    snapshot.gateBeforeSolidCount = report.gateBeforeSolidCount;
    snapshot.gateAfterFaceCount = report.gateAfterFaceCount;
    snapshot.gateAfterEdgeCount = report.gateAfterEdgeCount;
    snapshot.gateAfterShellCount = report.gateAfterShellCount;
    snapshot.gateAfterSolidCount = report.gateAfterSolidCount;
    snapshot.gateRoundtripFaceCount = report.gateRoundtripFaceCount;
    snapshot.gateRoundtripEdgeCount = report.gateRoundtripEdgeCount;
    snapshot.gateRoundtripShellCount = report.gateRoundtripShellCount;
    snapshot.gateRoundtripSolidCount = report.gateRoundtripSolidCount;
    snapshot.gateBeforeFreeEdges = report.gateBeforeFreeEdges;
    snapshot.gateAfterFreeEdges = report.gateAfterFreeEdges;
    snapshot.gateRoundtripFreeEdges = report.gateRoundtripFreeEdges;
    snapshot.gateBeforeMultipleEdges = report.gateBeforeMultipleEdges;
    snapshot.gateAfterMultipleEdges = report.gateAfterMultipleEdges;
    snapshot.gateRoundtripMultipleEdges = report.gateRoundtripMultipleEdges;
    snapshot.latestGateFailureReason = report.gateFailureReason;
    if (snapshot.latestGateFailureReason.empty() &&
        report.failureReason == PatchReplacementFailureReason::GateFailed) {
        snapshot.latestGateFailureReason = toString(report.failureReason);
    }

    snapshot.latestMessage = !fallbackMessage.empty() ? std::move(fallbackMessage) : report.message;
    if (snapshot.latestMessage.empty()) {
        snapshot.latestMessage = report.gateMessage;
    }
    if (report.failureReason == PatchReplacementFailureReason::GateFailed &&
        snapshot.latestMessage.find("StrictTopologyGate") == std::string::npos) {
        const auto detail = snapshot.latestGateFailureReason.empty()
            ? std::string("Gate failed.")
            : snapshot.latestGateFailureReason;
        snapshot.latestMessage = "StrictTopologyGate failed: " + detail;
    }

    snapshot.latestWarning = report.warningMessage;
    if (snapshot.latestWarning.empty()) {
        snapshot.latestWarning = report.repairWarningMessage;
    }
    if (snapshot.latestWarning.empty()) {
        snapshot.latestWarning = report.gateWarningMessage;
    }

    patchWorkflow_.processStatus = std::move(snapshot);
}

void AppController::updateCurrentPatchApplyState() {
    patchWorkflow_.updateApplyState();
}

}
