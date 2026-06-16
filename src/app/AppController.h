#pragma once

#include "command/Command.h"
#include "command/CommandContext.h"
#include "command/CommandHistory.h"
#include "common/Config.h"
#include "common/GeometryTypes.h"
#include "common/Result.h"
#include "feature/FeatureEdgeDetector.h"
#include "merge/MergePlanner.h"
#include "merge/SameDomainUnifier.h"
#include "external/geomagic/GeomagicAutoSurfaceConfig.h"
#include "external/geomagic/GeomagicAutoSurfaceResult.h"
#include "app/PatchPreviewRunLogger.h"
#include "app/ProcessStatus.h"
#include "patch/ImportedPatchInfo.h"
#include "patch/CropBoundaryDiagnostics.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchApplyState.h"
#include "patch/PatchPreviewReport.h"
#include "patch/PatchReplacementReport.h"
#include "stl/StlRegionExtractor.h"
#include "stl/StpSampledFittingMeshBuilder.h"
#include "validate/ShapeValidator.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace spo {

struct StlCandidateCropResult {
    bool success = false;
    StlRegionExtractResult extract;
    std::filesystem::path outputPath;
    std::string message;
};

struct PatchPreviewPipelineResult {
    bool success = false;
    StlCandidateCropResult crop;
    GeomagicAutoSurfaceResult geomagic;
    GeomagicFittingInputMode fittingInputMode = GeomagicFittingInputMode::LegacyStlCrop;
    StpSampledFittingReport stpSampledReport;
    std::filesystem::path patchPreviewRunLogPath;
    std::string message;
};

using PatchPreviewProgressCallback = std::function<void(ProcessStatusSnapshot)>;

class AppController {
public:
    const char* applicationName() const;
    Result execute(std::unique_ptr<Command> command);
    Result undo();
    Result redo();
    bool canUndo() const;
    bool canRedo() const;
    Result openStepFile(const std::filesystem::path& path);
    Result exportStepFile(const std::filesystem::path& path);
    Result verifyStepFileReadable(const std::filesystem::path& path);
    Result openStlFile(const std::filesystem::path& path);
    bool hasSourceStl() const;
    const std::filesystem::path& sourceStlPath() const;
    const StlMesh& sourceStlMesh() const;
    std::size_t sourceStlTriangleCount() const;
    StlBoundingBox sourceStlBoundingBox() const;
    StlCandidateCropResult cropStlForCandidate(
        const MergeCandidate& candidate,
        const std::filesystem::path& outputPath,
        const StlRegionExtractorOptions& options = {}) const;
    static StlCandidateCropResult cropStlForCandidateData(
        const ShapeDocument& document,
        const StlMesh& sourceMesh,
        const MergeCandidate& candidate,
        const std::filesystem::path& outputPath,
        const StlRegionExtractorOptions& options = {});
    static PatchPreviewPipelineResult cropAndRunGeomagicForCandidateData(
        const ShapeDocument& document,
        const StlMesh& sourceMesh,
        const MergeCandidate& candidate,
        const std::filesystem::path& workspaceRoot,
        GeomagicAutoSurfaceConfig config = {},
        const StlRegionExtractorOptions& options = {});
    static PatchPreviewPipelineResult generateFittingStlAndRunGeomagicForCandidateData(
        const ShapeDocument& document,
        const StlMesh& sourceMesh,
        const MergeCandidate& candidate,
        const std::filesystem::path& workspaceRoot,
        GeomagicAutoSurfaceConfig config = {},
        GeomagicFittingInputMode fittingMode = GeomagicFittingInputMode::LegacyStlCrop,
        const StlRegionExtractorOptions& cropOptions = {},
        const StpSampledFittingOptions& samplingOptions = {},
        PatchPreviewProgressCallback progress = {},
        PatchPreviewRunLogger runLogger = {});
    FeatureEdgeDetectionResult detectFeatureEdges(double angularThresholdDegrees, double minEdgeLength = 0.0);
    MergePlannerResult previewMergeCandidates(
        double angularThresholdDegrees,
        double minEdgeLength,
        const MergePlannerOptions& options);
    SameDomainUnifyResult unifySameDomain(
        double angularThresholdDegrees,
        double minEdgeLength,
        double linearTolerance,
        bool concatBsplines);
    Result importPatchForCurrentCandidateFromLocalStl(
        const std::filesystem::path& localStlPath,
        const MergeCandidate* candidate = nullptr);
    Result importPatchResultForCurrentCandidate(
        const GeomagicAutoSurfaceResult& result,
        const MergeCandidate* candidate = nullptr);
    Result importPatchFromFileForCurrentCandidate(
        const std::filesystem::path& patchPath,
        const MergeCandidate* candidate = nullptr);
    void clearCurrentPatchOverlay();
    bool patchPreviewReady() const;
    const PatchArtifactPaths& currentPatchArtifactPaths() const;
    const ImportedPatchInfo& currentImportedPatchInfo() const;
    const PatchPreviewReport& currentPatchPreviewReport() const;
    RegionPatchStatus currentPatchStatus() const;
    const std::string& currentPatchStatusMessage() const;
    const ProcessStatusSnapshot& currentProcessStatus() const;
    void updateProcessStatus(ProcessStatusSnapshot status);
    PatchApplyDecision currentPatchApplyDecision() const;
    Result requestApplyCurrentPatchPreview();
    Result applyCurrentPatchToCurrentCandidate(
        const MergeCandidate& candidate,
        PatchReplacementReport* outReport = nullptr,
        PatchPreviewProgressCallback progress = {});
    CropBoundaryDiagnosticsReport diagnoseCropBoundaryForCurrentPatch(
        const MergeCandidate& candidate,
        const StlMesh* localStlMesh = nullptr,
        const CropBoundaryDiagnosticsOptions& options = {}) const;
    static CropBoundaryDiagnosticsReport diagnoseCropBoundaryData(
        const ShapeDocument& document,
        const MergeCandidate& candidate,
        const StlMesh* localStlMesh,
        const TopoDS_Shape& importedPatchShape,
        const CropBoundaryDiagnosticsOptions& options = {},
        const StlMesh* sourceStlMesh = nullptr);
    bool hasDocument() const;
    const ShapeDocument& document() const;
    const FeatureEdgeDetectionResult& featureEdges() const;
    ShapeValidationReport validateShape();
    Result lockEdges(const std::vector<EdgeId>& edgeIds);
    Result unlockEdges(const std::vector<EdgeId>& edgeIds);
    std::set<EdgeId> lockedEdges() const;
    const CommandHistory& history() const;

private:
    CommandContext context_;
    CommandHistory history_;
    StlMesh sourceStlMesh_;
    std::filesystem::path sourceStlPath_;
    PatchArtifactPaths currentPatchArtifactPaths_;
    ImportedPatchInfo currentImportedPatchInfo_;
    PatchPreviewReport currentPatchPreviewReport_;
    bool patchPreviewReady_ = false;
    RegionPatchStatus currentPatchStatus_ = RegionPatchStatus::NotGenerated;
    std::string currentPatchStatusMessage_;
    ProcessStatusSnapshot processStatus_ = makeProcessStatus(ProcessStage::Idle, "Idle.");

    void updateCurrentPatchApplyState();
    void publishProcessStatusForCandidate(
        ProcessStage stage,
        const MergeCandidate* candidate,
        std::string message,
        std::string warning = {});
    void publishProcessStatusFromReplacementReport(
        ProcessStage stage,
        const PatchReplacementReport& report,
        std::string fallbackMessage = {});
};

}
