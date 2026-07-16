#pragma once

#include "app/PatchPreviewRunLogger.h"
#include "app/ProcessStatus.h"
#include "brep/ShapeDocument.h"
#include "external/geomagic/GeomagicAutoSurfaceConfig.h"
#include "external/geomagic/GeomagicAutoSurfaceResult.h"
#include "merge/MergeCandidate.h"
#include "stl/StlRegionExtractor.h"
#include "stl/StpSampledFittingMeshBuilder.h"

#include <filesystem>
#include <functional>
#include <string>

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
    GeomagicFittingInputMode fittingInputMode =
        GeomagicFittingInputMode::LegacyStlCrop;
    StpSampledFittingReport stpSampledReport;
    std::filesystem::path patchPreviewRunLogPath;
    std::string message;
};

using PatchPreviewProgressCallback =
    std::function<void(ProcessStatusSnapshot)>;

class PatchPreviewWorkflow {
public:
    static StlCandidateCropResult crop(
        const ShapeDocument& document,
        const StlMesh& sourceMesh,
        const MergeCandidate& candidate,
        const std::filesystem::path& outputPath,
        const StlRegionExtractorOptions& options);

    static PatchPreviewPipelineResult cropAndRunGeomagic(
        const ShapeDocument& document,
        const StlMesh& sourceMesh,
        const MergeCandidate& candidate,
        const std::filesystem::path& workspaceRoot,
        GeomagicAutoSurfaceConfig config,
        const StlRegionExtractorOptions& options);

    static PatchPreviewPipelineResult generateFittingStlAndRunGeomagic(
        const ShapeDocument& document,
        const StlMesh& sourceMesh,
        const MergeCandidate& candidate,
        const std::filesystem::path& workspaceRoot,
        GeomagicAutoSurfaceConfig config,
        GeomagicFittingInputMode fittingMode,
        const StlRegionExtractorOptions& cropOptions,
        const StpSampledFittingOptions& samplingOptions,
        PatchPreviewProgressCallback progress,
        PatchPreviewRunLogger runLogger);
};

}
