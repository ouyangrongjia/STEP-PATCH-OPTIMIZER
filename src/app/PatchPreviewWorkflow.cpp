#include "app/PatchPreviewWorkflow.h"

#include "external/geomagic/GeomagicAutoSurfaceBackend.h"
#include "external/geomagic/GeomagicOutputPathResolver.h"
#include "io/StlWriter.h"
#include "merge/BoundaryWireBuilder.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "stl/StlCutChainCutter.h"

#include <BRep_Tool.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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

struct PatchPreviewProgressPaths {
    std::filesystem::path localStlPath;
    std::filesystem::path patchStepPath;
    std::filesystem::path patchIgesPath;
    std::filesystem::path fitRegionLogPath;
    std::filesystem::path patchPreviewRunLogPath;
};

void publish_patch_preview_progress(
    const PatchPreviewProgressCallback& progress,
    ProcessStage stage,
    const MergeCandidate& candidate,
    std::string message,
    const PatchPreviewProgressPaths& paths = {},
    std::string warning = {}) {
    if (!progress) {
        return;
    }

    auto status = makeProcessStatus(stage, std::move(message));
    status.candidateId = candidate.candidate_id;
    status.sourceFaceCount = candidate.face_count > 0
        ? candidate.face_count
        : static_cast<int>(candidate.faces.size());
    status.boundaryEdgeCount = candidate.boundary_edge_count > 0
        ? candidate.boundary_edge_count
        : static_cast<int>(candidate.boundary_edges.size());
    status.localStlPath = paths.localStlPath;
    status.patchStepPath = paths.patchStepPath;
    status.patchIgesPath = paths.patchIgesPath;
    status.fitRegionLogPath = paths.fitRegionLogPath;
    status.patchPreviewRunLogPath = paths.patchPreviewRunLogPath;
    status.latestWarning = std::move(warning);
    progress(std::move(status));
}


}

StlCandidateCropResult PatchPreviewWorkflow::crop(
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

    // ---- GlobalCutChain mode ----
    if (options.mode == StlCropMode::GlobalCutChain) {
        // Build ordered boundary wire
        auto wireResult = BoundaryWireBuilder().buildOuterWire(document, boundary);
        if (!wireResult.success || wireResult.wire.IsNull()) {
            StlRegionExtractResult emptyExtract;
            return crop_error(outputPath, std::move(emptyExtract),
                "Failed to build boundary wire for global cut chain: " + wireResult.message);
        }

        // Sample boundary loop points from the wire
        std::vector<gp_Pnt> boundaryPoints;
        const auto& topology = document.topology();
        for (const auto edgeId : boundary.ordered_boundary_edges) {
            if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) continue;
            const auto& edge = topology.edge(edgeId);
            double first = 0.0, last = 0.0;
            auto curve = BRep_Tool::Curve(edge, first, last);
            if (curve.IsNull()) continue;
            const int nPts = 30;
            for (int k = 0; k < nPts; ++k) {
                double t = first + (last - first) * static_cast<double>(k) / static_cast<double>(nPts);
                boundaryPoints.push_back(curve->Value(t));
            }
        }

        if (boundaryPoints.size() < 3) {
            StlRegionExtractResult emptyExtract;
            return crop_error(outputPath, std::move(emptyExtract),
                "Boundary loop has too few sample points for global cut chain.");
        }

        // Compute seed points from candidate face centers
        std::vector<gp_Pnt> seedPoints;
        for (const auto faceId : candidate.faces) {
            if (faceId >= topology.faceCount()) continue;
            const auto& face = topology.face(faceId);
            try {
                GProp_GProps props;
                BRepGProp::SurfaceProperties(face, props);
                seedPoints.push_back(props.CentreOfMass());
            } catch (...) {}
        }

        // Run global cut chain cutter
        StlCutChainOptions cutOpts;
        cutOpts.samplesPerEdge = 30;
        cutOpts.minComponentFaceCount = 1;

        StlCutChainCutter cutter;
        auto cutResult = cutter.cut(
            sourceMesh, boundaryPoints,
            seedPoints.empty() ? std::optional<std::vector<gp_Pnt>>{} : seedPoints,
            cutOpts);

        if (!cutResult.success || cutResult.patchMesh.empty()) {
            StlRegionExtractResult emptyExtract;
            return crop_error(outputPath, std::move(emptyExtract),
                "Global cut chain failed: " + cutResult.message);
        }

        // Write the patch STL
        const auto write = StlWriter().write(cutResult.patchMesh, outputPath);
        if (!write.success) {
            return crop_error(outputPath, {}, write.message);
        }

        StlCandidateCropResult result;
        result.success = true;
        result.extract.localMesh = std::move(cutResult.patchMesh);
        result.extract.report.success = true;
        result.extract.report.candidate_id = candidate.candidate_id;
        result.extract.report.source_triangle_count = static_cast<int>(sourceMesh.triangleCount());
        result.extract.report.output_triangle_count = cutResult.patchFaceCount;
        result.extract.report.output_bbox = result.extract.localMesh.boundingBox();
        result.extract.report.message = cutResult.message;
        result.extract.success = true;
        result.outputPath = outputPath;
        result.message = "Global cut chain patch extracted (" + std::to_string(cutResult.patchFaceCount) + " triangles).";
        return result;
    }

    // ---- Default / ConservativeBoundaryBand modes ----
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

PatchPreviewPipelineResult PatchPreviewWorkflow::cropAndRunGeomagic(
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

    auto crop = PatchPreviewWorkflow::crop(
        document,
        sourceMesh,
        candidate,
        localStlPath,
        options);
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

PatchPreviewPipelineResult PatchPreviewWorkflow::generateFittingStlAndRunGeomagic(
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
    const auto root = workspaceRoot.empty()
        ? std::filesystem::absolute(std::filesystem::current_path()).lexically_normal()
        : std::filesystem::absolute(workspaceRoot).lexically_normal();
    if (runLogger.path().empty()) {
        runLogger = PatchPreviewRunLogger::create(root, candidate.candidate_id);
    }

    const auto localStlPath = default_pipeline_local_stl_path(document, candidate, root);
    PatchPreviewProgressPaths progressPaths;
    progressPaths.localStlPath = localStlPath;
    progressPaths.patchPreviewRunLogPath = runLogger.path();

    auto withRunLog = [&](PatchPreviewPipelineResult result) {
        result.patchPreviewRunLogPath = runLogger.path();
        return result;
    };

    auto publish = [&](ProcessStage stage, std::string message, std::string warning = {}) {
        const auto logMessage = warning.empty() ? message : message + " warning=" + warning;
        runLogger.log(toString(stage), logMessage);
        publish_patch_preview_progress(
            progress,
            stage,
            candidate,
            std::move(message),
            progressPaths,
            std::move(warning));
    };

    publish(
        ProcessStage::AnalyzingBoundary,
        "Patch preview pipeline preparing outputs: fitting_mode=" + std::string(toString(fittingMode)));

    std::string directoryMessage;
    if (!ensure_parent_directory(localStlPath, directoryMessage)) {
        publish(
            ProcessStage::CroppingStl,
            directoryMessage,
            directoryMessage);
        return withRunLog(pipeline_error({}, {}, directoryMessage));
    }

    const auto outputResolveStart = std::chrono::steady_clock::now();
    const auto outputPaths = resolveGeomagicOutputPathsFromCropStl(
        localStlPath,
        root / "data" / "crop_stl",
        root / "data" / "crop_stp",
        root / "data" / "crop_igs");
    if (!outputPaths.success) {
        publish(
            ProcessStage::CroppingStl,
            outputPaths.message,
            outputPaths.message);
        return withRunLog(pipeline_error({}, {}, outputPaths.message));
    }
    progressPaths.patchStepPath = outputPaths.outputStepPath;
    progressPaths.patchIgesPath = outputPaths.outputIgesPath;
    progressPaths.fitRegionLogPath = sidecar_path(outputPaths.outputStepPath, "_fit_region.log");
    runLogger.logDuration(
        "AnalyzingBoundary",
        "Resolved patch preview output paths.",
        outputResolveStart);

    StlCandidateCropResult crop;
    StpSampledFittingReport sampledReport;

    if (fittingMode == GeomagicFittingInputMode::StpSampledCandidateSurface) {
        publish(
            ProcessStage::CroppingStl,
            "Generating fitting STL from STP sampled candidate surface: fitting_mode=" +
                std::string(toString(fittingMode)));
        const auto fittingBuildStart = std::chrono::steady_clock::now();
        StlMesh syntheticMesh;
        StpSampledFittingMeshBuilder builder;
        sampledReport = builder.build(document, candidate, samplingOptions, syntheticMesh);
        if (!sampledReport.success) {
            publish(
                ProcessStage::CroppingStl,
                sampledReport.message,
                sampledReport.message);
            PatchPreviewPipelineResult result;
            result.success = false;
            result.fittingInputMode = fittingMode;
            result.stpSampledReport = std::move(sampledReport);
            result.message = sampledReport.message;
            return withRunLog(std::move(result));
        }
        runLogger.logDuration(
            "CroppingStl",
            "STP sampled fitting mesh built.",
            fittingBuildStart);

        sampledReport.outputPath = localStlPath;
        const auto stlWriteStart = std::chrono::steady_clock::now();
        StlWriter writer;
        const auto writeResult = writer.write(syntheticMesh, localStlPath);
        if (!writeResult.success) {
            sampledReport.success = false;
            sampledReport.message = writeResult.message;
            publish(
                ProcessStage::CroppingStl,
                writeResult.message,
                writeResult.message);
            PatchPreviewPipelineResult result;
            result.success = false;
            result.fittingInputMode = fittingMode;
            result.stpSampledReport = std::move(sampledReport);
            result.message = writeResult.message;
            return withRunLog(std::move(result));
        }
        runLogger.logDuration(
            "CroppingStl",
            "Fitting STL written from STP sampled mesh.",
            stlWriteStart);

        crop.success = true;
        crop.outputPath = localStlPath;
        crop.extract.localMesh = std::move(syntheticMesh);
        crop.extract.report.output_triangle_count = sampledReport.outputTriangleCount;
        crop.extract.report.output_bbox = sampledReport.output_bbox;
        crop.extract.report.success = true;
        crop.extract.report.candidate_id = candidate.candidate_id;
        crop.message = "STP-sampled fitting STL written.";
        publish(
            ProcessStage::CroppingStl,
            "Fitting STL ready: fitting_mode=" + std::string(toString(fittingMode)) +
                ", triangles=" + std::to_string(sampledReport.outputTriangleCount));
    } else {
        StlRegionExtractorOptions effectiveCropOptions = cropOptions;
        if (fittingMode == GeomagicFittingInputMode::ConservativeBoundaryBandStlCrop) {
            effectiveCropOptions.mode = StlCropMode::ConservativeBoundaryBand;
        } else {
            effectiveCropOptions.mode = StlCropMode::CentroidOnly;
        }

        publish(
            ProcessStage::CroppingStl,
            "Generating fitting STL from source STL: fitting_mode=" + std::string(toString(fittingMode)));
        const auto stlCropStart = std::chrono::steady_clock::now();
        crop = PatchPreviewWorkflow::crop(
            document,
            sourceMesh,
            candidate,
            localStlPath,
            effectiveCropOptions);
        if (!crop.success) {
            publish(
                ProcessStage::CroppingStl,
                crop.message,
                crop.message);
            PatchPreviewPipelineResult result;
            result.success = false;
            result.crop = std::move(crop);
            result.fittingInputMode = fittingMode;
            result.message = crop.message;
            return withRunLog(std::move(result));
        }
        runLogger.logDuration(
            "CroppingStl",
            "Source STL crop and fitting STL write finished.",
            stlCropStart);
        publish(
            ProcessStage::CroppingStl,
            "Fitting STL ready: fitting_mode=" + std::string(toString(fittingMode)) +
                ", triangles=" + std::to_string(crop.extract.localMesh.triangleCount()));
    }

    config.inputStlPath = localStlPath;
    config.outputStepPath = outputPaths.outputStepPath;
    config.outputIgesPath = outputPaths.outputIgesPath;
    config.workDir = root;
    config.fitRegionLogPath = progressPaths.fitRegionLogPath;
    config.geometry = "Mechanical";
    config.autoMerge = true;
    config.adaptiveFit = false;
    config.strictPatchTarget = false;

    publish(
        ProcessStage::RunningGeomagic,
        "Geomagic AutoSurface started: fitting_mode=" + std::string(toString(fittingMode)) +
            ", skip_remesh=" + std::string(config.skipRemesh ? "true" : "false"));
    const auto geomagicStart = std::chrono::steady_clock::now();
    auto geomagic = GeomagicAutoSurfaceBackend().run(config);
    if (!geomagic.success) {
        const auto message = geomagic.errorMessage.empty() ? geomagic.message : geomagic.errorMessage;
        const auto warning = message.empty() ? std::string("Geomagic AutoSurface failed.") : message;
        runLogger.logDuration(
            "RunningGeomagic",
            warning,
            geomagicStart);
        publish(
            ProcessStage::RunningGeomagic,
            warning,
            warning);
        PatchPreviewPipelineResult result;
        result.success = false;
        result.crop = std::move(crop);
        result.geomagic = std::move(geomagic);
        result.fittingInputMode = fittingMode;
        result.stpSampledReport = std::move(sampledReport);
        result.message = message.empty() ? "Geomagic AutoSurface failed." : message;
        return withRunLog(std::move(result));
    }
    runLogger.logDuration(
        "RunningGeomagic",
        "Geomagic AutoSurface finished.",
        geomagicStart);
    publish(
        ProcessStage::RunningGeomagic,
        "Geomagic AutoSurface finished.");

    PatchPreviewPipelineResult result;
    result.success = true;
    result.crop = std::move(crop);
    result.geomagic = std::move(geomagic);
    result.fittingInputMode = fittingMode;
    result.stpSampledReport = std::move(sampledReport);
    result.patchPreviewRunLogPath = runLogger.path();
    result.message = "Patch preview pipeline completed.";
    return result;
}


}
