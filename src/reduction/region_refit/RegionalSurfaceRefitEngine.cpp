#include "reduction/region_refit/RegionalSurfaceRefitEngine.h"

#include "reduction/region_refit/RegionConstraintExtractor.h"
#include "reduction/region_refit/RegionPatchLayout.h"
#include "reduction/region_refit/RegionReferenceMesh.h"
#include "reduction/region_refit/StepAdaptiveRegionSampler.h"

#include <chrono>

namespace spo {

RegionRefitResult RegionalSurfaceRefitEngine::run(
    const ShapeDocument& document,
    const RegionRefitRequest& request) const {
    RegionRefitResult result;
    result.report.inputFaceCount = static_cast<int>(request.selectedFaces.size());

    if (request.options.maximumWallClockMilliseconds <= 0 ||
        request.options.constraintsBudgetMilliseconds <= 0 ||
        request.options.maximumMemoryMiB == 0 ||
        request.options.maximumSampleTriangles == 0 ||
        request.options.maximumFullFitCount <= 0 ||
        request.options.maximumAdaptivePatchCount <= 0 ||
        request.options.maximumParameterDistortion <= 0.0 ||
        (request.layoutMode == RegionRefitLayoutMode::FixedPatchCount &&
            request.options.fixedPatchCount <= 0)) {
        result.report.status = RegionRefitStatus::InvalidRequest;
        result.report.failureStage = RegionRefitStage::Constraints;
        result.report.message = "Region refit resource and layout budgets must be positive.";
        return result;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto extraction = RegionConstraintExtractor().extract(document, request);
    const auto ended = std::chrono::steady_clock::now();
    RegionRefitStageReport stage;
    stage.stage = RegionRefitStage::Constraints;
    stage.success = extraction.success();
    stage.wallMilliseconds = std::chrono::duration<double, std::milli>(ended - started).count();
    stage.message = extraction.message;
    result.report.stages.push_back(stage);
    result.report.totalWallMilliseconds = stage.wallMilliseconds;

    if (!extraction.success()) {
        result.report.status = extraction.status;
        result.report.failureStage = RegionRefitStage::Constraints;
        result.report.message = extraction.message;
        return result;
    }
    if (stage.wallMilliseconds > static_cast<double>(request.options.constraintsBudgetMilliseconds) ||
        stage.wallMilliseconds > static_cast<double>(request.options.maximumWallClockMilliseconds)) {
        result.report.status = RegionRefitStatus::TimeBudgetExceeded;
        result.report.failureStage = RegionRefitStage::Constraints;
        result.report.message = "Constraint extraction exceeded its wall-clock budget.";
        return result;
    }

    result.constraints = extraction.constraints;
    result.report.inputFaceCount = static_cast<int>(result.constraints.selectedFaces.size());
    if (request.diagnosticStopAfterStage == RegionRefitStage::Constraints) {
        result.report.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.report.failureStage = RegionRefitStage::Sampling;
        result.report.message = "Constraint-only diagnostic completed.";
        return result;
    }

    const auto samplingStarted = std::chrono::steady_clock::now();
    auto sampling = StepAdaptiveRegionSampler().sample(
        document,
        result.constraints,
        request.options);
    const auto samplingEnded = std::chrono::steady_clock::now();
    RegionRefitStageReport samplingStage;
    samplingStage.stage = RegionRefitStage::Sampling;
    samplingStage.success = sampling.report.success();
    samplingStage.wallMilliseconds = std::chrono::duration<double, std::milli>(
        samplingEnded - samplingStarted).count();
    samplingStage.message = sampling.report.message;
    result.report.stages.push_back(samplingStage);
    result.report.totalWallMilliseconds += samplingStage.wallMilliseconds;
    result.report.sampleVertexCount = sampling.report.vertexCount;
    result.report.sampleTriangleCount = sampling.report.triangleCount;
    result.report.boundarySampleCount = sampling.report.boundarySampleCount;
    result.report.weldedInternalSeamVertexCount = sampling.report.weldedInternalSeamVertexCount;
    result.report.normalDiscontinuityVertexCount = sampling.report.normalDiscontinuityVertexCount;
    result.report.maximumSamplingChordError = sampling.report.maximumInteriorChordError;
    result.report.maximumSamplingNormalErrorDegrees = sampling.report.maximumInteriorNormalErrorDegrees;
    result.report.maximumBoundaryChordError = sampling.report.maximumBoundaryChordError;
    result.report.referenceMeshFingerprint = sampling.report.meshFingerprint;

    if (!sampling.report.success()) {
        result.report.status = sampling.report.status;
        result.report.failureStage = RegionRefitStage::Sampling;
        result.report.message = sampling.report.message;
        return result;
    }
    result.referenceMesh = std::make_shared<RegionReferenceMesh>(std::move(sampling.mesh));
    if (samplingStage.wallMilliseconds > static_cast<double>(request.options.samplingBudgetMilliseconds) ||
        result.report.totalWallMilliseconds > static_cast<double>(request.options.maximumWallClockMilliseconds)) {
        result.report.status = RegionRefitStatus::TimeBudgetExceeded;
        result.report.failureStage = RegionRefitStage::Sampling;
        result.report.message = "Adaptive STEP sampling exceeded its wall-clock budget.";
        return result;
    }

    if (request.diagnosticStopAfterStage == RegionRefitStage::Sampling) {
        result.report.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.report.failureStage = RegionRefitStage::Layout;
        result.report.message = "Sampling-only diagnostic completed.";
        return result;
    }

    const auto layoutStarted = std::chrono::steady_clock::now();
    auto layouts = RegionPatchLayoutGenerator().generate(
        *result.referenceMesh,
        request.layoutMode,
        request.options);
    const auto layoutEnded = std::chrono::steady_clock::now();
    RegionRefitStageReport layoutStage;
    layoutStage.stage = RegionRefitStage::Layout;
    layoutStage.success = layouts.success();
    layoutStage.wallMilliseconds = std::chrono::duration<double, std::milli>(
        layoutEnded - layoutStarted).count();
    layoutStage.message = layouts.message;
    result.report.stages.push_back(layoutStage);
    result.report.totalWallMilliseconds += layoutStage.wallMilliseconds;
    result.report.layoutCandidates = layouts.candidates;
    if (!layouts.success()) {
        result.report.status = layouts.status;
        result.report.failureStage = RegionRefitStage::Layout;
        result.report.message = layouts.message;
        return result;
    }
    result.selectedLayout = std::make_shared<RegionPatchLayout>(
        std::move(layouts.selectedCandidates.front()));
    result.report.selectedPatchCount = result.selectedLayout->patchCount;
    result.report.selectedLayoutHash = result.selectedLayout->hash;
    if (layoutStage.wallMilliseconds > static_cast<double>(request.options.layoutBudgetMilliseconds) ||
        result.report.totalWallMilliseconds > static_cast<double>(request.options.maximumWallClockMilliseconds)) {
        result.report.status = RegionRefitStatus::TimeBudgetExceeded;
        result.report.failureStage = RegionRefitStage::Layout;
        result.report.message = "Layout generation exceeded its wall-clock budget.";
        return result;
    }
    if (request.diagnosticStopAfterStage == RegionRefitStage::Layout) {
        result.report.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.report.failureStage = RegionRefitStage::Fitting;
        result.report.message = "Layout-only diagnostic completed.";
        return result;
    }

    result.report.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
    result.report.failureStage = RegionRefitStage::Fitting;
    result.report.message = "Layout screening passed; joint fitting is not complete yet.";
    return result;
}

}
