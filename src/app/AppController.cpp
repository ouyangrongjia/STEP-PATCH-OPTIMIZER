#include "app/AppController.h"

#include "command/DetectFeatureCommand.h"
#include "command/ExportStepCommand.h"
#include "command/LoadStepCommand.h"
#include "command/LockedEdgeRef.h"
#include "command/LockEdgeCommand.h"
#include "command/MergePatchCommand.h"
#include "command/PatchReplacementCommand.h"
#include "command/PlaneRegionBatchMergeCommand.h"
#include "command/PlaneRegionMergeCommand.h"
#include "command/SphereRegionBatchMergeCommand.h"
#include "command/SphereRegionMergeCommand.h"
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

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace spo {

namespace {

StlCandidateCropResult crop_error(
    const std::filesystem::path& outputPath,
    StlRegionExtractResult extract,
    std::string message) {
    StlCandidateCropResult result;
    result.outputPath = outputPath;
    result.extract = std::move(extract);
    result.message = std::move(message);
    return result;
}

std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension;
}

std::filesystem::path document_output_stem(const ShapeDocument& document) {
    const auto stem = document.sourcePath().stem();
    if (!stem.empty()) {
        return stem;
    }
    return std::filesystem::path("document");
}

std::filesystem::path candidate_patch_filename(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const char* extension) {
    std::ostringstream stream;
    stream << "_candidate_" << std::setfill('0') << std::setw(4) << candidate.candidate_id << extension;
    auto filename = document_output_stem(document);
    filename += stream.str();
    return filename;
}

std::filesystem::path default_pipeline_local_stl_path(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const std::filesystem::path& workspaceRoot) {
    const auto root = workspaceRoot.empty()
        ? std::filesystem::absolute(std::filesystem::current_path()).lexically_normal()
        : std::filesystem::absolute(workspaceRoot).lexically_normal();
    return root / "data" / "crop_stl" / document_output_stem(document) / candidate_patch_filename(document, candidate, ".stl");
}

std::filesystem::path sidecar_path(const std::filesystem::path& outputStepPath, const char* suffix) {
    return outputStepPath.parent_path() / (outputStepPath.stem().wstring() + std::wstring(suffix, suffix + std::strlen(suffix)));
}

bool ensure_parent_directory(const std::filesystem::path& path, std::string& message) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        message = "Could not create output directory: " + error.message();
        return false;
    }
    return true;
}

PatchPreviewPipelineResult pipeline_error(
    StlCandidateCropResult crop,
    GeomagicAutoSurfaceResult geomagic,
    std::string message) {
    PatchPreviewPipelineResult result;
    result.crop = std::move(crop);
    result.geomagic = std::move(geomagic);
    result.message = std::move(message);
    return result;
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
        processStatus_ = makeProcessStatus(
            ProcessStage::CachedUndo,
            "cached undo applied; Geomagic / STL crop / patch import / repair were not rerun.");
    }
    return result;
}

Result AppController::redo() {
    const auto result = history_.redo(context_);
    if (result.success()) {
        clearCurrentPatchOverlay();
        processStatus_ = makeProcessStatus(
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
        processStatus_ = makeProcessStatus(ProcessStage::Idle, "STEP/STP loaded; patch preview cleared.");
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
    processStatus_ = makeProcessStatus(ProcessStage::Idle, "Source STL loaded.");
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
    if (!document.hasShape()) {
        return crop_error(outputPath, {}, "Open a STEP/STP document before cropping STL.");
    }
    if (sourceMesh.empty()) {
        return crop_error(outputPath, {}, "Open the source STL before cropping.");
    }
    if (outputPath.empty()) {
        return crop_error(outputPath, {}, "Output STL path is empty.");
    }
    if (candidate.candidate_type != MergeCandidateType::FeatureBoundedRefit) {
        return crop_error(outputPath, {}, "Current candidate is not FeatureBoundedRefit.");
    }
    if (candidate.status == MergeCandidateStatus::Rejected || candidate.status == MergeCandidateStatus::Hidden) {
        return crop_error(outputPath, {}, "Current candidate is rejected or hidden.");
    }

    const auto boundary = RegionBoundaryAnalyzer().analyze(document, candidate);
    if (!boundary.valid) {
        return crop_error(outputPath, {}, "Candidate boundary is not a valid single closed loop: " + boundary.message);
    }

    auto extract = StlRegionExtractor().extract(document, candidate, sourceMesh, options);
    if (!extract.success) {
        const auto message = extract.report.message.empty()
            ? std::string("STL crop failed.")
            : extract.report.message;
        return crop_error(outputPath, std::move(extract), message);
    }

    const auto write = StlWriter().write(extract.localMesh, outputPath);
    if (!write.success) {
        return crop_error(outputPath, std::move(extract), write.message);
    }

    StlCandidateCropResult result;
    result.success = true;
    result.extract = std::move(extract);
    result.outputPath = outputPath;
    return result;
}

PatchPreviewPipelineResult AppController::cropAndRunGeomagicForCandidateData(
    const ShapeDocument& document,
    const StlMesh& sourceMesh,
    const MergeCandidate& candidate,
    const std::filesystem::path& workspaceRoot,
    GeomagicAutoSurfaceConfig config,
    const StlRegionExtractorOptions& options) {
    const auto localStlPath = default_pipeline_local_stl_path(document, candidate, workspaceRoot);

    std::string directoryMessage;
    if (!ensure_parent_directory(localStlPath, directoryMessage)) {
        return pipeline_error({}, {}, directoryMessage);
    }

    auto crop = cropStlForCandidateData(document, sourceMesh, candidate, localStlPath, options);
    if (!crop.success) {
        return pipeline_error(std::move(crop), {}, crop.message);
    }

    const auto root = workspaceRoot.empty()
        ? std::filesystem::absolute(std::filesystem::current_path()).lexically_normal()
        : std::filesystem::absolute(workspaceRoot).lexically_normal();
    const auto outputPaths = resolveGeomagicOutputPathsFromCropStl(
        crop.outputPath,
        root / "data" / "crop_stl",
        root / "data" / "crop_stp",
        root / "data" / "crop_igs");
    if (!outputPaths.success) {
        return pipeline_error(std::move(crop), {}, outputPaths.message);
    }

    config.inputStlPath = crop.outputPath;
    config.outputStepPath = outputPaths.outputStepPath;
    config.outputIgesPath = outputPaths.outputIgesPath;
    config.workDir = root;
    config.fitRegionLogPath = sidecar_path(outputPaths.outputStepPath, "_fit_region.log");
    config.geometry = "Mechanical";
    config.autoMerge = true;
    config.adaptiveFit = false;
    config.strictPatchTarget = false;

    auto geomagic = GeomagicAutoSurfaceBackend().run(config);
    if (!geomagic.success) {
        const auto message = geomagic.errorMessage.empty() ? geomagic.message : geomagic.errorMessage;
        return pipeline_error(std::move(crop), std::move(geomagic), message.empty() ? "Geomagic AutoSurface failed." : message);
    }

    PatchPreviewPipelineResult result;
    result.success = true;
    result.crop = std::move(crop);
    result.geomagic = std::move(geomagic);
    result.message = "Patch preview pipeline completed.";
    return result;
}

PatchPreviewPipelineResult AppController::generateFittingStlAndRunGeomagicForCandidateData(
    const ShapeDocument& document,
    const StlMesh& sourceMesh,
    const MergeCandidate& candidate,
    const std::filesystem::path& workspaceRoot,
    GeomagicAutoSurfaceConfig config,
    GeomagicFittingInputMode fittingMode,
    const StlRegionExtractorOptions& cropOptions,
    const StpSampledFittingOptions& samplingOptions) {
    const auto localStlPath = default_pipeline_local_stl_path(document, candidate, workspaceRoot);

    std::string directoryMessage;
    if (!ensure_parent_directory(localStlPath, directoryMessage)) {
        return pipeline_error({}, {}, directoryMessage);
    }

    const auto root = workspaceRoot.empty()
        ? std::filesystem::absolute(std::filesystem::current_path()).lexically_normal()
        : std::filesystem::absolute(workspaceRoot).lexically_normal();
    const auto outputPaths = resolveGeomagicOutputPathsFromCropStl(
        localStlPath,
        root / "data" / "crop_stl",
        root / "data" / "crop_stp",
        root / "data" / "crop_igs");
    if (!outputPaths.success) {
        return pipeline_error({}, {}, outputPaths.message);
    }

    StlCandidateCropResult crop;
    StpSampledFittingReport sampledReport;

    if (fittingMode == GeomagicFittingInputMode::StpSampledCandidateSurface) {
        // Generate synthetic STL from STP candidate faces
        StlMesh syntheticMesh;
        StpSampledFittingMeshBuilder builder;
        sampledReport = builder.build(document, candidate, samplingOptions, syntheticMesh);
        if (!sampledReport.success) {
            PatchPreviewPipelineResult result;
            result.success = false;
            result.fittingInputMode = fittingMode;
            result.stpSampledReport = std::move(sampledReport);
            result.message = sampledReport.message;
            return result;
        }

        sampledReport.outputPath = localStlPath;
        StlWriter writer;
        const auto writeResult = writer.write(syntheticMesh, localStlPath);
        if (!writeResult.success) {
            sampledReport.success = false;
            sampledReport.message = writeResult.message;
            PatchPreviewPipelineResult result;
            result.success = false;
            result.fittingInputMode = fittingMode;
            result.stpSampledReport = std::move(sampledReport);
            result.message = writeResult.message;
            return result;
        }

        crop.success = true;
        crop.outputPath = localStlPath;
        crop.extract.localMesh = std::move(syntheticMesh);
        crop.extract.report.output_triangle_count = sampledReport.outputTriangleCount;
        crop.extract.report.output_bbox = sampledReport.output_bbox;
        crop.extract.report.success = true;
        crop.extract.report.candidate_id = candidate.candidate_id;
        crop.message = "STP-sampled fitting STL written.";
    } else {
        // Legacy STL crop path
        StlRegionExtractorOptions effectiveCropOptions = cropOptions;
        if (fittingMode == GeomagicFittingInputMode::ConservativeBoundaryBandStlCrop) {
            effectiveCropOptions.mode = StlCropMode::ConservativeBoundaryBand;
        } else {
            effectiveCropOptions.mode = StlCropMode::CentroidOnly;
        }

        crop = cropStlForCandidateData(document, sourceMesh, candidate, localStlPath, effectiveCropOptions);
        if (!crop.success) {
            PatchPreviewPipelineResult result;
            result.success = false;
            result.crop = std::move(crop);
            result.fittingInputMode = fittingMode;
            result.message = crop.message;
            return result;
        }
    }

    config.inputStlPath = localStlPath;
    config.outputStepPath = outputPaths.outputStepPath;
    config.outputIgesPath = outputPaths.outputIgesPath;
    config.workDir = root;
    config.fitRegionLogPath = sidecar_path(outputPaths.outputStepPath, "_fit_region.log");
    config.geometry = "Mechanical";
    config.autoMerge = true;
    config.adaptiveFit = false;
    config.strictPatchTarget = false;

    auto geomagic = GeomagicAutoSurfaceBackend().run(config);
    if (!geomagic.success) {
        const auto message = geomagic.errorMessage.empty() ? geomagic.message : geomagic.errorMessage;
        PatchPreviewPipelineResult result;
        result.success = false;
        result.crop = std::move(crop);
        result.geomagic = std::move(geomagic);
        result.fittingInputMode = fittingMode;
        result.stpSampledReport = std::move(sampledReport);
        result.message = message.empty() ? "Geomagic AutoSurface failed." : message;
        return result;
    }

    PatchPreviewPipelineResult result;
    result.success = true;
    result.crop = std::move(crop);
    result.geomagic = std::move(geomagic);
    result.fittingInputMode = fittingMode;
    result.stpSampledReport = std::move(sampledReport);
    result.message = "Patch preview pipeline completed.";
    return result;
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
    }
    return result;
}

RegionMergeResult AppController::mergePlaneCandidate(
    const MergeCandidate& candidate,
    const PlaneRegionMergeOptions& options) {
    RegionMergeResult result;
    auto command = std::make_unique<PlaneRegionMergeCommand>(candidate, options, &result);
    const auto status = execute(std::move(command));
    if (!status.success()) {
        return result;
    }
    if (result.success) {
        clearCurrentPatchOverlay();
    }
    return result;
}

RegionMergeResult AppController::mergePlaneCandidates(
    const std::vector<MergeCandidate>& candidates,
    const PlaneRegionMergeOptions& options) {
    RegionMergeResult result;
    auto command = std::make_unique<PlaneRegionBatchMergeCommand>(candidates, options, &result);
    const auto status = execute(std::move(command));
    if (!status.success()) {
        return result;
    }
    if (result.success) {
        clearCurrentPatchOverlay();
    }
    return result;
}

RegionMergeResult AppController::mergeSphereCandidate(
    const MergeCandidate& candidate,
    const SphereRegionMergeOptions& options) {
    RegionMergeResult result;
    auto command = std::make_unique<SphereRegionMergeCommand>(candidate, options, &result);
    const auto status = execute(std::move(command));
    if (!status.success()) {
        return result;
    }
    if (result.success) {
        clearCurrentPatchOverlay();
    }
    return result;
}

RegionMergeResult AppController::mergeSphereCandidates(
    const std::vector<MergeCandidate>& candidates,
    const SphereRegionMergeOptions& options) {
    RegionMergeResult result;
    auto command = std::make_unique<SphereRegionBatchMergeCommand>(candidates, options, &result);
    const auto status = execute(std::move(command));
    if (!status.success()) {
        return result;
    }
    if (result.success) {
        clearCurrentPatchOverlay();
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
    processStatus_ = std::move(importing);

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

    currentPatchArtifactPaths_ = artifacts;
    currentImportedPatchInfo_ = std::move(imported);
    currentPatchPreviewReport_ = buildPatchPreviewReport(
        hasDocument() ? &context_.document : nullptr,
        candidate,
        currentImportedPatchInfo_,
        currentPatchArtifactPaths_);
    patchPreviewReady_ = currentPatchPreviewReport_.success;
    updateCurrentPatchApplyState();
    publishProcessStatusForCandidate(
        ProcessStage::PreviewReady,
        candidate,
        currentPatchStatusMessage_,
        currentPatchPreviewReport_.warningMessage);
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
    processStatus_ = std::move(importing);

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

    currentPatchArtifactPaths_ = artifacts;
    currentImportedPatchInfo_ = std::move(imported);
    currentPatchPreviewReport_ = buildPatchPreviewReport(
        hasDocument() ? &context_.document : nullptr,
        candidate,
        currentImportedPatchInfo_,
        currentPatchArtifactPaths_);
    patchPreviewReady_ = currentPatchPreviewReport_.success;
    updateCurrentPatchApplyState();
    publishProcessStatusForCandidate(
        ProcessStage::PreviewReady,
        candidate,
        currentPatchStatusMessage_,
        currentPatchPreviewReport_.warningMessage);
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
    processStatus_ = std::move(importing);

    auto imported = PatchImportService().importPatch(patchPath);
    if (!imported.success) {
        clearCurrentPatchOverlay();
        const auto message = imported.errorMessage.empty() ? imported.message : imported.errorMessage;
        publishProcessStatusForCandidate(ProcessStage::ImportingPatch, candidate, message);
        return Result::error(message);
    }

    currentPatchArtifactPaths_ = artifacts;
    currentImportedPatchInfo_ = std::move(imported);
    currentPatchPreviewReport_ = buildPatchPreviewReport(
        hasDocument() ? &context_.document : nullptr,
        candidate,
        currentImportedPatchInfo_,
        currentPatchArtifactPaths_);
    patchPreviewReady_ = currentPatchPreviewReport_.success;
    updateCurrentPatchApplyState();
    publishProcessStatusForCandidate(
        ProcessStage::PreviewReady,
        candidate,
        currentPatchStatusMessage_,
        currentPatchPreviewReport_.warningMessage);
    return Result::ok();
}

void AppController::clearCurrentPatchOverlay() {
    currentPatchArtifactPaths_ = {};
    currentImportedPatchInfo_ = {};
    currentPatchPreviewReport_ = {};
    patchPreviewReady_ = false;
    currentPatchStatus_ = RegionPatchStatus::NotGenerated;
    currentPatchStatusMessage_ = "Patch preview cleared.";
    processStatus_ = makeProcessStatus(ProcessStage::Idle, currentPatchStatusMessage_);
}

bool AppController::patchPreviewReady() const {
    return patchPreviewReady_;
}

const PatchArtifactPaths& AppController::currentPatchArtifactPaths() const {
    return currentPatchArtifactPaths_;
}

const ImportedPatchInfo& AppController::currentImportedPatchInfo() const {
    return currentImportedPatchInfo_;
}

const PatchPreviewReport& AppController::currentPatchPreviewReport() const {
    return currentPatchPreviewReport_;
}

RegionPatchStatus AppController::currentPatchStatus() const {
    return currentPatchStatus_;
}

const std::string& AppController::currentPatchStatusMessage() const {
    return currentPatchStatusMessage_;
}

const ProcessStatusSnapshot& AppController::currentProcessStatus() const {
    return processStatus_;
}

void AppController::updateProcessStatus(ProcessStatusSnapshot status) {
    processStatus_ = std::move(status);
}

PatchApplyDecision AppController::currentPatchApplyDecision() const {
    return evaluatePatchApplyReadiness(patchPreviewReady_, currentPatchPreviewReport_);
}

Result AppController::requestApplyCurrentPatchPreview() {
    const auto decision = currentPatchApplyDecision();
    if (!decision.canRequestApply) {
        currentPatchStatus_ = decision.status;
        currentPatchStatusMessage_ = decision.reason;
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, nullptr, decision.reason);
        return Result::error(decision.reason);
    }

    currentPatchStatus_ = RegionPatchStatus::ApplyFailed;
    currentPatchStatusMessage_ = "Patch Apply requires an explicit current candidate.";
    publishProcessStatusForCandidate(ProcessStage::ApplyFailed, nullptr, currentPatchStatusMessage_);
    return Result::error("Patch Apply requires an explicit current candidate.");
}

Result AppController::applyCurrentPatchToCurrentCandidate(
    const MergeCandidate& candidate,
    PatchReplacementReport* outReport) {
    publishProcessStatusForCandidate(
        ProcessStage::ApplyingPatch,
        &candidate,
        "Patch Apply started.");

    if (!hasDocument()) {
        const std::string message = "Open a STEP/STP document before applying a patch.";
        currentPatchStatus_ = RegionPatchStatus::ApplyFailed;
        currentPatchStatusMessage_ = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::MissingDocument, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        return Result::error(message);
    }

    const auto decision = currentPatchApplyDecision();
    if (!decision.canRequestApply) {
        currentPatchStatus_ = decision.status;
        currentPatchStatusMessage_ = decision.reason;
        publish_apply_failure(outReport, PatchReplacementFailureReason::MissingPreviewReport, &candidate, decision.reason);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, decision.reason);
        return Result::error(decision.reason);
    }
    if (candidate.faces.empty()) {
        const std::string message = "Current candidate has no source faces.";
        currentPatchStatus_ = RegionPatchStatus::ApplyFailed;
        currentPatchStatusMessage_ = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::MissingCandidate, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        return Result::error(message);
    }
    if (candidate.candidate_type != MergeCandidateType::FeatureBoundedRefit) {
        const std::string message = "Current candidate is not FeatureBoundedRefit.";
        currentPatchStatus_ = RegionPatchStatus::ApplyFailed;
        currentPatchStatusMessage_ = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::UnsupportedCandidate, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        return Result::error(message);
    }
    if (candidate.status == MergeCandidateStatus::Rejected || candidate.status == MergeCandidateStatus::Hidden) {
        const std::string message = "Current candidate is rejected or hidden.";
        currentPatchStatus_ = RegionPatchStatus::ApplyFailed;
        currentPatchStatusMessage_ = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::UnsupportedCandidate, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        return Result::error(message);
    }

    const auto candidateFaceCount = candidate.face_count > 0
        ? candidate.face_count
        : static_cast<int>(candidate.faces.size());
    if (currentPatchPreviewReport_.candidateId != candidate.candidate_id ||
        currentPatchPreviewReport_.sourceFaceCount != candidateFaceCount) {
        const std::string message = "Patch preview does not match current candidate.";
        currentPatchStatus_ = RegionPatchStatus::ApplyFailed;
        currentPatchStatusMessage_ = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::UnsupportedCandidate, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        return Result::error(message);
    }

    publishProcessStatusForCandidate(
        ProcessStage::AnalyzingBoundary,
        &candidate,
        "Analyzing original CAD boundary before Patch Apply.");
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
        currentPatchStatus_ = RegionPatchStatus::ApplyFailed;
        currentPatchStatusMessage_ = message;
        publish_apply_failure(outReport, PatchReplacementFailureReason::InvalidBoundary, &candidate, message);
        publishProcessStatusForCandidate(ProcessStage::ApplyFailed, &candidate, message);
        return Result::error(message);
    }

    PatchReplacementInput input;
    input.document = &context_.document;
    input.candidate = &candidate;
    input.boundary = &boundary;
    input.importedPatch = &currentImportedPatchInfo_;
    input.artifactPaths = &currentPatchArtifactPaths_;
    input.previewReport = &currentPatchPreviewReport_;

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
    const auto result = execute(std::move(command));
    const auto& finalReport = *reportTarget;
    if (!result.success()) {
        currentPatchStatus_ = RegionPatchStatus::ApplyFailed;
        currentPatchStatusMessage_ = !finalReport.message.empty()
            ? finalReport.message
            : result.message();
        publishProcessStatusFromReplacementReport(ProcessStage::ApplyFailed, finalReport, currentPatchStatusMessage_);
        return result;
    }

    publishProcessStatusFromReplacementReport(
        ProcessStage::Applied,
        finalReport,
        "Patch Apply completed and committed after StrictTopologyGate passed.");
    currentPatchArtifactPaths_ = {};
    currentImportedPatchInfo_ = {};
    currentPatchPreviewReport_ = {};
    patchPreviewReady_ = false;
    currentPatchStatus_ = RegionPatchStatus::Applied;
    currentPatchStatusMessage_ = "Patch Apply completed and committed after StrictTopologyGate passed.";
    return Result::ok();
}

CropBoundaryDiagnosticsReport AppController::diagnoseCropBoundaryForCurrentPatch(
    const MergeCandidate& candidate,
    const StlMesh* localStlMesh,
    const CropBoundaryDiagnosticsOptions& options) const {
    if (!patchPreviewReady_ || currentImportedPatchInfo_.shape.IsNull()) {
        CropBoundaryDiagnosticsReport report;
        report.message = "Crop boundary diagnostics requires a ready imported patch preview.";
        return report;
    }
    return diagnoseCropBoundaryData(
        context_.document,
        candidate,
        localStlMesh,
        currentImportedPatchInfo_.shape,
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
    return execute(std::make_unique<LockEdgeCommand>(edgeIds));
}

Result AppController::unlockEdges(const std::vector<EdgeId>& edgeIds) {
    return execute(std::make_unique<UnlockEdgeCommand>(edgeIds));
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
    snapshot.localStlPath = currentPatchArtifactPaths_.localStlPath;
    snapshot.patchStepPath = currentPatchArtifactPaths_.patchStepPath;
    snapshot.patchIgesPath = currentPatchArtifactPaths_.patchIgesSidecarPath;
    snapshot.fitRegionLogPath = currentPatchArtifactPaths_.fitRegionLogPath;

    if (candidate != nullptr) {
        snapshot.candidateId = candidate->candidate_id;
        snapshot.sourceFaceCount = candidate->face_count > 0
            ? candidate->face_count
            : static_cast<int>(candidate->faces.size());
        snapshot.boundaryEdgeCount = candidate->boundary_edge_count > 0
            ? candidate->boundary_edge_count
            : static_cast<int>(candidate->boundary_edges.size());
    } else if (currentPatchPreviewReport_.candidateId >= 0) {
        snapshot.candidateId = currentPatchPreviewReport_.candidateId;
        snapshot.sourceFaceCount = currentPatchPreviewReport_.sourceFaceCount;
        snapshot.boundaryEdgeCount = currentPatchPreviewReport_.sourceBoundaryEdgeCount;
    }

    processStatus_ = std::move(snapshot);
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
    snapshot.localStlPath = currentPatchArtifactPaths_.localStlPath;
    snapshot.patchStepPath = currentPatchArtifactPaths_.patchStepPath;
    snapshot.patchIgesPath = currentPatchArtifactPaths_.patchIgesSidecarPath;
    snapshot.fitRegionLogPath = currentPatchArtifactPaths_.fitRegionLogPath;

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

    processStatus_ = std::move(snapshot);
}

void AppController::updateCurrentPatchApplyState() {
    const auto decision = currentPatchApplyDecision();
    currentPatchStatus_ = decision.status;
    currentPatchStatusMessage_ = decision.canRequestApply ? decision.message : decision.reason;
}

}
