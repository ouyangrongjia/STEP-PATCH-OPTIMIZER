#include "reduction/region_refit/RegionalSurfaceRefitEngine.h"

#include "reduction/region_refit/RegionConstraintExtractor.h"
#include "reduction/region_refit/JointBSplineNetworkFitter.h"
#include "reduction/region_refit/NativeRegionPatchNetworkBuilder.h"
#include "reduction/region_refit/QualityBalancedRegionRemesher.h"
#include "reduction/region_refit/RegionPatchLayout.h"
#include "reduction/region_refit/RegionReferenceMesh.h"
#include "reduction/region_refit/RegionRefitResourceMonitor.h"
#include "reduction/region_refit/RegionalSurfaceRefitCache.h"
#include "reduction/region_refit/StepAdaptiveRegionSampler.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/PatchReplacementAssembler.h"
#include "patch/PatchReplacementRepair.h"
#include "validate/StrictTopologyGate.h"

#include <Precision.hxx>

#include <chrono>
#include <algorithm>

namespace spo {

namespace {

RegionBoundaryAnalysis boundaryAnalysisFromConstraints(
    const RegionTopologyConstraints& constraints) {
    RegionBoundaryAnalysis boundary;
    boundary.valid = !constraints.outerWire.IsNull() && !constraints.orderedBoundaryEdges.empty();
    boundary.connected_component_count = 1;
    boundary.outer_wire_count = 1;
    boundary.inner_wire_count = 0;
    boundary.boundary_closed = true;
    boundary.has_holes = false;
    boundary.has_non_manifold_edges = false;
    boundary.has_branching_boundary = false;
    boundary.ordered_boundary_edges = constraints.orderedBoundaryEdges;
    boundary.boundary_loops = {constraints.orderedBoundaryEdges};
    boundary.message = boundary.valid
        ? "Constructed the native replacement boundary contract."
        : "Native replacement constraints have no valid outer boundary wire.";
    return boundary;
}

BoundaryConstrainedPatchBuildResult replacementBuildResult(
    const RegionTopologyConstraints& constraints,
    const NativeRegionPatchNetworkBuildResult& native) {
    BoundaryConstrainedPatchBuildResult replacement;
    replacement.success = native.success();
    replacement.replacementShape = native.replacementShape;
    replacement.replacementFaces = native.replacementFaces;
    replacement.internalPatchEdges = native.internalSeamEdges;
    replacement.sourceFaceIds = constraints.selectedFaces;
    replacement.usedMultiFaceFragment = native.replacementFaces.size() > 1;
    replacement.attemptedMultiSurfaceBoundaryShell = true;
    replacement.usedMultiSurfaceBoundaryShell = native.success();
    replacement.sourceFaceCount = static_cast<int>(constraints.selectedFaces.size());
    replacement.patchFaceCount = static_cast<int>(native.replacementFaces.size());
    replacement.replacementFaceCount = static_cast<int>(native.replacementFaces.size());
    replacement.internalPatchEdgeCount = static_cast<int>(native.internalSeamEdges.size());
    replacement.multiSurfaceAssignedBoundarySegmentCount =
        native.reusedOriginalBoundaryEdgeCount + native.splitBoundarySegmentCount;
    replacement.multiSurfaceSplitBoundaryEdgeCount = native.splitBoundarySegmentCount;
    replacement.multiSurfaceBuiltFaceCount = replacement.replacementFaceCount;
    replacement.multiSurfaceClosedWireCount = replacement.replacementFaceCount;
    replacement.multiSurfaceBoundaryEdgePcurveRebuildAttemptCount = native.pcurveProjectionCount;
    replacement.multiSurfaceBoundaryEdgePcurveRebuildSuccessCount =
        native.pcurveProjectionCount - native.sameParameterFailureCount;
    replacement.multiSurfaceBoundaryEdgePcurveRebuildFailureCount =
        native.sameParameterFailureCount;
    replacement.multiSurfaceBoundaryEdgeSameParameterCheckCount =
        native.sameParameterCheckCount;
    replacement.multiSurfaceBoundaryEdgeSameParameterFailureCount =
        native.sameParameterFailureCount;
    replacement.multiSurfaceBoundaryEdgeMaxSameParameterDeviation =
        native.maximumSameParameterDeviation;
    replacement.multiSurfaceSplitBoundarySegments = native.splitBoundarySegments;
    replacement.message = native.message;
    return replacement;
}

void copyStrictTopologyReport(
    RegionRefitReport& target,
    const StrictTopologyGateReport& source) {
    target.strictTopologyPassed = source.passed;
    target.strictTopologyFailureReason = toString(source.failureReason);
    target.afterBRepCheckValid = source.afterBRepCheckValid;
    target.stepExportOk = source.stepExportOk;
    target.stepRoundtripOk = source.stepRoundtripOk;
    target.roundtripBRepCheckValid = source.roundtripBRepCheckValid;
    target.beforeSolidCount = source.beforeStats.solids;
    target.afterSolidCount = source.afterStats.solids;
    target.roundtripSolidCount = source.roundtripStats.solids;
    target.beforeFaceCount = source.beforeStats.faces;
    target.afterFaceCount = source.afterStats.faces;
    target.roundtripFaceCount = source.roundtripStats.faces;
    target.beforeFreeEdgeCount = source.beforeFreeEdges;
    target.afterFreeEdgeCount = source.afterFreeEdges;
    target.roundtripFreeEdgeCount = source.roundtripFreeEdges;
    target.beforeMultipleEdgeCount = source.beforeMultipleEdges;
    target.afterMultipleEdgeCount = source.afterMultipleEdges;
    target.roundtripMultipleEdgeCount = source.roundtripMultipleEdges;
}

bool finishStageResources(
    RegionRefitStageReport& stage,
    const RegionProcessResourceSnapshot& before,
    RegionRefitReport& report,
    const RegionRefitOptions& options) {
    const auto after = captureRegionProcessResources();
    stage.cpuMilliseconds = std::max(
        0.0, after.processCpuMilliseconds - before.processCpuMilliseconds);
    stage.peakWorkingSetBytes = after.peakWorkingSetBytes;
    stage.peakPrivateBytes = std::max(report.peakPrivateBytes, after.privateBytes);
    report.peakWorkingSetBytes = std::max(
        report.peakWorkingSetBytes, after.peakWorkingSetBytes);
    report.peakPrivateBytes = std::max(report.peakPrivateBytes, after.privateBytes);
    const auto budgetBytes = static_cast<std::uint64_t>(options.maximumMemoryMiB) *
        1024ull * 1024ull;
    return after.workingSetBytes > budgetBytes || after.privateBytes > budgetBytes;
}

void failMemoryBudget(
    RegionRefitResult& result,
    const RegionRefitStage stage) {
    result.report.status = RegionRefitStatus::MemoryBudgetExceeded;
    result.report.failureStage = stage;
    result.report.message = "Native regional refit exceeded the process memory budget.";
}

void markNoProductResult(
    RegionRefitResult& result,
    const std::string& reason) {
    result.report.selectionOutcome = "NoProductValidatedResult";
    result.report.unattainableReason = reason;
}

StepAdaptiveRegionSamplingResult remeshLayoutCarrier(
    const ShapeDocument& document,
    const RegionTopologyConstraints& constraints,
    const RegionRefitOptions& options,
    const RegionReferenceMesh& reference,
    const std::chrono::steady_clock::time_point deadline) {
    auto balanced = QualityBalancedRegionRemesher().remesh(
        document, constraints, options, reference, deadline);
    StepAdaptiveRegionSamplingResult result;
    result.mesh = std::move(balanced.mesh);
    auto& report = result.report;
    report.status = balanced.report.success
        ? RegionRefitStatus::Success
        : (balanced.report.timeBudgetExceeded
            ? RegionRefitStatus::TimeBudgetExceeded
            : RegionRefitStatus::Failed);
    report.message = balanced.report.message;
    report.sourceFaceCount = constraints.selectedFaces.size();
    report.vertexCount = result.mesh.vertices.size();
    report.triangleCount = result.mesh.triangles.size();
    report.boundarySampleCount = result.mesh.boundarySamples.size();
    report.edgeLengthP01 = balanced.report.edgeLengthP01;
    report.edgeLengthP50 = balanced.report.edgeLengthP50;
    report.edgeLengthP99 = balanced.report.edgeLengthP99;
    report.edgeLengthP99P01Ratio = balanced.report.edgeLengthP99P01Ratio;
    report.minimumAngleP05Degrees = balanced.report.minimumAngleP05Degrees;
    report.minimumAngleP50Degrees = balanced.report.minimumAngleP50Degrees;
    report.aspectRatioP50 = balanced.report.aspectRatioP50;
    report.aspectRatioP95 = balanced.report.aspectRatioP95;
    report.aspectRatioP99 = balanced.report.aspectRatioP99;
    report.normalizedQualityP05 = balanced.report.normalizedQualityP05;
    report.normalizedQualityP50 = balanced.report.normalizedQualityP50;
    report.zeroAreaTriangleCount = balanced.report.zeroAreaTriangleCount;
    report.nonManifoldEdgeCount = balanced.report.nonManifoldEdgeCount;
    report.remeshSplitCount = balanced.report.splitCount;
    report.remeshCollapseCount = balanced.report.collapseCount;
    report.remeshFlipCount = balanced.report.flipCount;
    report.remeshRelocationCount = balanced.report.relocationCount;
    if (!result.mesh.empty()) report.meshFingerprint = result.mesh.deterministicFingerprint();
    return result;
}

}

RegionRefitResult RegionalSurfaceRefitEngine::run(
    const ShapeDocument& document,
    const RegionRefitRequest& request) const {
    RegionRefitResult result;
    result.report.inputFaceCount = static_cast<int>(request.selectedFaces.size());
    result.report.requestedPatchCount = request.layoutMode == RegionRefitLayoutMode::TargetPatchCount
        ? request.options.requestedPatchCount
        : (request.layoutMode == RegionRefitLayoutMode::FixedPatchCount
            ? request.options.fixedPatchCount : 0);
    result.report.selectionOutcome = "NotSelected";

    if (request.options.maximumWallClockMilliseconds <= 0 ||
        request.options.constraintsBudgetMilliseconds <= 0 ||
        request.options.maximumMemoryMiB == 0 ||
        request.options.maximumSampleTriangles == 0 ||
         ((request.options.samplingMode == RegionRefitSamplingMode::QualityBalanced ||
              request.options.samplingMode == RegionRefitSamplingMode::UniformUv) &&
            (request.options.targetSampleTriangles == 0 ||
                 request.options.targetSampleTriangles > request.options.maximumSampleTriangles)) ||
        request.options.targetLayoutCarrierTriangles == 0 ||
        request.options.targetLayoutCarrierTriangles > request.options.maximumSampleTriangles ||
        (request.options.samplingMode == RegionRefitSamplingMode::QualityBalanced &&
            (request.options.maximumRemeshSweeps <= 0 ||
                request.options.maximumLocalSizeRatio <= 1.0)) ||
        request.options.maximumFullFitCount <= 0 ||
        request.options.maximumAdaptivePatchCount <= 0 ||
        request.options.maximumParameterDistortion <= 0.0 ||
        request.options.maximumControlPointsPerSide < 6 ||
        (request.layoutMode == RegionRefitLayoutMode::FixedPatchCount &&
            request.options.fixedPatchCount <= 0) ||
        (request.layoutMode == RegionRefitLayoutMode::TargetPatchCount &&
            (request.options.requestedPatchCount <= 0 ||
                request.options.patchCountSearchRadius < 0))) {
        result.report.status = RegionRefitStatus::InvalidRequest;
        result.report.failureStage = RegionRefitStage::Constraints;
        result.report.message = "Region refit resource and layout budgets must be positive.";
        return result;
    }

    const auto initialResources = captureRegionProcessResources();
    result.report.peakWorkingSetBytes = initialResources.peakWorkingSetBytes;
    result.report.peakPrivateBytes = initialResources.privateBytes;
    const auto memoryBudgetBytes =
        static_cast<std::uint64_t>(request.options.maximumMemoryMiB) * 1024ull * 1024ull;
    if (initialResources.workingSetBytes > memoryBudgetBytes ||
        initialResources.privateBytes > memoryBudgetBytes) {
        failMemoryBudget(result, RegionRefitStage::Constraints);
        return result;
    }
    const RegionalSurfaceRefitCache cache(request);
    result.report.cacheKey = cache.key();

    const auto constraintResources = captureRegionProcessResources();
    const auto started = std::chrono::steady_clock::now();
    const auto extraction = RegionConstraintExtractor().extract(document, request);
    const auto ended = std::chrono::steady_clock::now();
    RegionRefitStageReport stage;
    stage.stage = RegionRefitStage::Constraints;
    stage.success = extraction.success();
    stage.wallMilliseconds = std::chrono::duration<double, std::milli>(ended - started).count();
    stage.message = extraction.message;
    const auto constraintMemoryExceeded = finishStageResources(
        stage, constraintResources, result.report, request.options);
    result.report.stages.push_back(stage);
    result.report.totalWallMilliseconds = stage.wallMilliseconds;
    if (cache.enabled()) {
        result.report.cacheEvents.push_back(cache.recordConstraintResult(extraction.success()));
    }

    if (!extraction.success()) {
        result.report.status = extraction.status;
        result.report.failureStage = RegionRefitStage::Constraints;
        result.report.message = extraction.message;
        return result;
    }
    if (constraintMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::Constraints);
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

    const auto samplingResources = captureRegionProcessResources();
    const auto samplingStarted = std::chrono::steady_clock::now();
    StepAdaptiveRegionSamplingResult sampling;
    StepAdaptiveRegionSamplingResult layoutCarrierSampling;
    RegionRefitCacheEvent samplingCacheEvent;
    const auto samplingCacheHit = cache.loadSampling(sampling, samplingCacheEvent);
    if (!samplingCacheHit) {
        sampling = StepAdaptiveRegionSampler().sample(
            document,
            result.constraints,
            request.options,
            samplingStarted + std::chrono::milliseconds(
                request.options.samplingBudgetMilliseconds));
        if (sampling.report.success() && cache.enabled()) {
            const auto writeEvent = cache.saveSampling(sampling);
            samplingCacheEvent.writeMilliseconds = writeEvent.writeMilliseconds;
            if (!writeEvent.message.empty()) {
                samplingCacheEvent.message += " " + writeEvent.message;
            }
        }
    }
    if (sampling.report.success()) {
        if (request.options.layoutCarrierSamplingMode == request.options.samplingMode &&
            request.options.targetLayoutCarrierTriangles == request.options.targetSampleTriangles) {
            layoutCarrierSampling = sampling;
        } else {
            auto carrierOptions = request.options;
            carrierOptions.samplingMode = request.options.layoutCarrierSamplingMode;
            carrierOptions.targetSampleTriangles = request.options.targetLayoutCarrierTriangles;
            const auto carrierBudgetMilliseconds = std::max<std::int64_t>(
                1, request.options.samplingBudgetMilliseconds - 1000);
            const auto carrierDeadline = samplingStarted + std::chrono::milliseconds(
                carrierBudgetMilliseconds);
            layoutCarrierSampling = request.options.layoutCarrierSamplingMode ==
                    RegionRefitSamplingMode::QualityBalanced
                ? remeshLayoutCarrier(
                    document, result.constraints, carrierOptions, sampling.mesh, carrierDeadline)
                : StepAdaptiveRegionSampler().sample(
                    document, result.constraints, carrierOptions, carrierDeadline);
        }
    }
    const auto samplingEnded = std::chrono::steady_clock::now();
    RegionRefitStageReport samplingStage;
    samplingStage.stage = RegionRefitStage::Sampling;
    samplingStage.success = sampling.report.success() && layoutCarrierSampling.report.success();
    samplingStage.wallMilliseconds = std::chrono::duration<double, std::milli>(
        samplingEnded - samplingStarted).count();
    samplingStage.message = samplingCacheHit
        ? "Loaded the rich STEP reference mesh from algorithm cache."
        : (sampling.report.success()
            ? sampling.report.message + " Layout carrier: " + layoutCarrierSampling.report.message
            : sampling.report.message);
    const auto samplingMemoryExceeded = finishStageResources(
        samplingStage, samplingResources, result.report, request.options);
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
    result.report.samplingGeometricLimitsPassed = sampling.report.geometricLimitsPassed;
    result.report.samplingEdgeLengthP01 = sampling.report.edgeLengthP01;
    result.report.samplingEdgeLengthP50 = sampling.report.edgeLengthP50;
    result.report.samplingEdgeLengthP99 = sampling.report.edgeLengthP99;
    result.report.samplingEdgeLengthP99P01Ratio = sampling.report.edgeLengthP99P01Ratio;
    result.report.samplingMinimumAngleP05Degrees = sampling.report.minimumAngleP05Degrees;
    result.report.samplingMinimumAngleP50Degrees = sampling.report.minimumAngleP50Degrees;
    result.report.samplingAspectRatioP50 = sampling.report.aspectRatioP50;
    result.report.samplingAspectRatioP95 = sampling.report.aspectRatioP95;
    result.report.samplingAspectRatioP99 = sampling.report.aspectRatioP99;
    result.report.samplingNormalizedQualityP05 = sampling.report.normalizedQualityP05;
    result.report.samplingNormalizedQualityP50 = sampling.report.normalizedQualityP50;
    result.report.samplingZeroAreaTriangleCount = sampling.report.zeroAreaTriangleCount;
    result.report.samplingNonManifoldEdgeCount = sampling.report.nonManifoldEdgeCount;
    result.report.samplingRemeshSplitCount = sampling.report.remeshSplitCount;
    result.report.samplingRemeshCollapseCount = sampling.report.remeshCollapseCount;
    result.report.samplingRemeshFlipCount = sampling.report.remeshFlipCount;
    result.report.samplingRemeshRelocationCount = sampling.report.remeshRelocationCount;
    result.report.referenceMeshFingerprint = sampling.report.meshFingerprint;
    result.report.layoutCarrierVertexCount = layoutCarrierSampling.report.vertexCount;
    result.report.layoutCarrierTriangleCount = layoutCarrierSampling.report.triangleCount;
    result.report.layoutCarrierBoundarySampleCount = layoutCarrierSampling.report.boundarySampleCount;
    result.report.layoutCarrierZeroAreaTriangleCount = layoutCarrierSampling.report.zeroAreaTriangleCount;
    result.report.layoutCarrierNonManifoldEdgeCount = layoutCarrierSampling.report.nonManifoldEdgeCount;
    result.report.layoutCarrierMinimumAngleP05Degrees =
        layoutCarrierSampling.report.minimumAngleP05Degrees;
    result.report.layoutCarrierAspectRatioP95 = layoutCarrierSampling.report.aspectRatioP95;
    result.report.layoutCarrierMeshFingerprint = layoutCarrierSampling.report.meshFingerprint;
    if (cache.enabled()) result.report.cacheEvents.push_back(samplingCacheEvent);

    if (!sampling.mesh.empty()) {
        result.referenceMesh = std::make_shared<RegionReferenceMesh>(std::move(sampling.mesh));
    }
    if (!layoutCarrierSampling.mesh.empty()) {
        result.layoutCarrierMesh = std::make_shared<RegionReferenceMesh>(
            std::move(layoutCarrierSampling.mesh));
    }

    if (!sampling.report.success() || !layoutCarrierSampling.report.success()) {
        const auto& failed = sampling.report.success() ? layoutCarrierSampling.report : sampling.report;
        result.report.status = failed.status;
        result.report.failureStage = RegionRefitStage::Sampling;
        result.report.message = failed.message;
        result.report.selectionOutcome = "NoCandidateGenerated";
        result.report.unattainableReason = failed.message;
        return result;
    }
    if (samplingMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::Sampling);
        return result;
    }
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

    const auto layoutResources = captureRegionProcessResources();
    const auto layoutStarted = std::chrono::steady_clock::now();
    RegionPatchLayoutGenerationResult layouts;
    RegionRefitCacheEvent layoutCacheEvent;
    const auto layoutCacheHit = cache.loadLayouts(layouts, layoutCacheEvent);
    if (!layoutCacheHit) {
        layouts = RegionPatchLayoutGenerator().generate(
            *result.layoutCarrierMesh,
            request.layoutMode,
            request.options);
        if (layouts.success() && cache.enabled()) {
            const auto writeEvent = cache.saveLayouts(layouts);
            layoutCacheEvent.writeMilliseconds = writeEvent.writeMilliseconds;
            if (!writeEvent.message.empty()) {
                layoutCacheEvent.message += " " + writeEvent.message;
            }
        }
    }
    const auto layoutEnded = std::chrono::steady_clock::now();
    RegionRefitStageReport layoutStage;
    layoutStage.stage = RegionRefitStage::Layout;
    layoutStage.success = layouts.success();
    layoutStage.wallMilliseconds = std::chrono::duration<double, std::milli>(
        layoutEnded - layoutStarted).count();
    layoutStage.message = layoutCacheHit
        ? "Loaded deterministic regional layouts from algorithm cache."
        : layouts.message;
    const auto layoutMemoryExceeded = finishStageResources(
        layoutStage, layoutResources, result.report, request.options);
    result.report.stages.push_back(layoutStage);
    result.report.totalWallMilliseconds += layoutStage.wallMilliseconds;
    result.report.layoutCandidates = layouts.candidates;
    if (cache.enabled()) result.report.cacheEvents.push_back(layoutCacheEvent);
    if (!layouts.success()) {
        result.report.status = layouts.status;
        result.report.failureStage = RegionRefitStage::Layout;
        result.report.message = layouts.message;
        result.report.selectionOutcome = "NoValidatedLayout";
        result.report.unattainableReason = layouts.message;
        return result;
    }
    if (layoutMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::Layout);
        return result;
    }
    result.selectedLayout = std::make_shared<RegionPatchLayout>(
        layouts.selectedCandidates.front());
    result.report.selectedPatchCount = result.selectedLayout->patchCount;
    result.report.selectedLayoutHash = result.selectedLayout->hash;
    if (result.report.totalWallMilliseconds > static_cast<double>(request.options.maximumWallClockMilliseconds)) {
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

    const auto fittingResources = captureRegionProcessResources();
    const auto fittingStarted = std::chrono::steady_clock::now();
    JointBSplineNetworkFitResult fitted;
    RegionRefitCacheEvent fitCacheEvent;
    const auto fitCacheHit = request.executionMode == RegionRefitExecutionMode::ValidatedOnly &&
        cache.loadFit(layouts.selectedCandidates, fitted, fitCacheEvent);
    if (!fitCacheHit) {
        const auto reservedMilliseconds = request.options.retrimBudgetMilliseconds +
            request.options.finalValidationBudgetMilliseconds;
        const auto fitBudgetMilliseconds = std::min(
            request.options.totalFitBudgetMilliseconds,
            request.options.maximumWallClockMilliseconds -
                static_cast<std::int64_t>(result.report.totalWallMilliseconds) -
                reservedMilliseconds);
        if (fitBudgetMilliseconds <= 0) {
            fitted.status = RegionRefitStatus::TimeBudgetExceeded;
            fitted.message = "Insufficient wall-clock budget remains for fitting and reserved final validation.";
        } else {
            fitted = JointBSplineNetworkFitter().fit(
                *result.referenceMesh,
                *result.layoutCarrierMesh,
                result.constraints,
                layouts.selectedCandidates,
                request.options,
                fittingStarted + std::chrono::milliseconds(fitBudgetMilliseconds));
            if (cache.enabled() && request.executionMode == RegionRefitExecutionMode::ValidatedOnly) {
                const auto writeEvent = cache.saveFit(fitted);
                fitCacheEvent.negativeEntry = writeEvent.negativeEntry;
                fitCacheEvent.writeMilliseconds = writeEvent.writeMilliseconds;
                if (!writeEvent.message.empty()) {
                    fitCacheEvent.message += " " + writeEvent.message;
                }
            }
        }
    }
    const auto fittingEnded = std::chrono::steady_clock::now();
    RegionRefitStageReport fittingStage;
    fittingStage.stage = RegionRefitStage::Fitting;
    fittingStage.success = fitted.success();
    fittingStage.wallMilliseconds = std::chrono::duration<double, std::milli>(
        fittingEnded - fittingStarted).count();
    fittingStage.message = fitCacheHit
        ? (fitCacheEvent.negativeEntry
            ? "Loaded a validated negative joint-fit cache entry."
            : "Loaded a fitted B-spline network from algorithm cache.")
        : fitted.message;
    const auto fittingMemoryExceeded = finishStageResources(
        fittingStage, fittingResources, result.report, request.options);
    result.report.stages.push_back(fittingStage);
    result.report.totalWallMilliseconds += fittingStage.wallMilliseconds;
    result.report.fitAttempts = fitted.attempts;
    result.report.fullFitCount = fitCacheHit
        ? 0
        : static_cast<int>(fitted.attempts.size());
    result.report.cachedFitAttemptCount = fitCacheHit
        ? static_cast<int>(fitted.attempts.size())
        : 0;
    if (cache.enabled()) result.report.cacheEvents.push_back(fitCacheEvent);
    result.report.fitGatePassed = fitted.success();
    const auto useBestEffort = !fitted.success() &&
        fitted.status == RegionRefitStatus::NoValidatedLayoutWithinBudget &&
        request.executionMode == RegionRefitExecutionMode::BestEffortThroughSewing &&
        fitted.bestEffortNetwork != nullptr;
    result.report.bestEffortUsed = useBestEffort;
    if (!fitted.success() && !useBestEffort) {
        result.report.status = fitted.status;
        result.report.failureStage = RegionRefitStage::Fitting;
        result.report.message = fitted.message;
        result.report.patchCountDeviation = result.report.requestedPatchCount > 0
            ? result.report.selectedPatchCount - result.report.requestedPatchCount
            : 0;
        result.report.exactTargetReached = result.report.requestedPatchCount > 0 &&
            result.report.patchCountDeviation == 0;
        markNoProductResult(result, fitted.message);
        return result;
    }
    if (fittingMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::Fitting);
        return result;
    }
    result.fittedNetwork = fitted.success() ? fitted.network : fitted.bestEffortNetwork;
    result.selectedLayout = std::make_shared<RegionPatchLayout>(result.fittedNetwork->layout);
    result.fittedPatchNetwork = result.fittedNetwork->inspectionShape;
    result.report.selectedPatchCount = result.fittedNetwork->layout.patchCount;
    result.report.patchCountDeviation = result.report.requestedPatchCount > 0
        ? result.report.selectedPatchCount - result.report.requestedPatchCount
        : 0;
    result.report.exactTargetReached = result.report.requestedPatchCount > 0 &&
        result.report.patchCountDeviation == 0;
    result.report.selectedLayoutHash = result.fittedNetwork->layout.hash;
    if (request.diagnosticStopAfterStage == RegionRefitStage::Fitting) {
        result.report.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.report.failureStage = RegionRefitStage::Retrim;
        result.report.message = "Fitting-only diagnostic completed.";
        return result;
    }

    const auto retrimResources = captureRegionProcessResources();
    const auto retrimStarted = std::chrono::steady_clock::now();
    result.report.retrimAttempted = true;
    const auto nativeBuild = NativeRegionPatchNetworkBuilder().build(
        *result.layoutCarrierMesh,
        result.constraints,
        *result.fittedNetwork,
        request.options.curveOnSurfaceTolerance,
        request.executionMode == RegionRefitExecutionMode::BestEffortThroughSewing);
    const auto boundary = boundaryAnalysisFromConstraints(result.constraints);
    PatchReplacementAssemblyResult assembly;
    if (nativeBuild.constructible() && boundary.valid) {
        auto replacement = replacementBuildResult(result.constraints, nativeBuild);
        replacement.success = true;
        assembly = PatchReplacementAssembler().assemble(
            document,
            boundary,
            replacement,
            true);
    }
    const auto retrimEnded = std::chrono::steady_clock::now();
    RegionRefitStageReport retrimStage;
    retrimStage.stage = RegionRefitStage::Retrim;
    retrimStage.success = nativeBuild.success() && boundary.valid && assembly.success;
    retrimStage.wallMilliseconds = std::chrono::duration<double, std::milli>(
        retrimEnded - retrimStarted).count();
    retrimStage.message = !nativeBuild.success()
        ? nativeBuild.message
        : (!boundary.valid ? boundary.message : assembly.message);
    const auto retrimMemoryExceeded = finishStageResources(
        retrimStage, retrimResources, result.report, request.options);
    result.report.stages.push_back(retrimStage);
    result.report.totalWallMilliseconds += retrimStage.wallMilliseconds;
    result.report.retrimReplacementFaceCount =
        static_cast<int>(nativeBuild.replacementFaces.size());
    result.report.retrimInternalSeamCount = nativeBuild.internalSeamCount;
    result.report.retrimReusedBoundaryEdgeCount = nativeBuild.reusedOriginalBoundaryEdgeCount;
    result.report.retrimSplitBoundarySegmentCount = nativeBuild.splitBoundarySegmentCount;
    result.report.retrimPcurveProjectionCount = nativeBuild.pcurveProjectionCount;
    result.report.retrimSameParameterCheckCount = nativeBuild.sameParameterCheckCount;
    result.report.retrimSameParameterFailureCount = nativeBuild.sameParameterFailureCount;
    result.report.retrimMaximumSameParameterDeviation = nativeBuild.maximumSameParameterDeviation;
    result.report.edgeConsistency = nativeBuild.edgeConsistency;
    result.report.assemblyPassed = assembly.success;
    result.report.retrimValidationPassed = retrimStage.success;
    const auto retrimConstructed = nativeBuild.constructible() && boundary.valid && assembly.success &&
        !assembly.shape.IsNull();
    if (!retrimConstructed) {
        result.report.status = RegionRefitStatus::Failed;
        result.report.failureStage = RegionRefitStage::Retrim;
        result.report.message = retrimStage.message;
        result.report.continuationStopReason = retrimStage.message;
        markNoProductResult(result, retrimStage.message);
        return result;
    }
    if (retrimMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::Retrim);
        return result;
    }
    if (retrimStage.wallMilliseconds > static_cast<double>(request.options.retrimBudgetMilliseconds) ||
        result.report.totalWallMilliseconds > static_cast<double>(request.options.maximumWallClockMilliseconds)) {
        result.report.status = RegionRefitStatus::TimeBudgetExceeded;
        result.report.failureStage = RegionRefitStage::Retrim;
        result.report.message = "Native retrimming exceeded its wall-clock budget.";
        return result;
    }
    if (request.diagnosticStopAfterStage == RegionRefitStage::Retrim) {
        result.report.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.report.failureStage = RegionRefitStage::Sewing;
        result.report.message = "Retrim-only diagnostic completed.";
        return result;
    }

    result.report.sewingAttempted = true;
    const auto sewingResources = captureRegionProcessResources();
    const auto sewingStarted = std::chrono::steady_clock::now();
    PatchReplacementRepairOptions repairOptions;
    repairOptions.runUnifySameDomain = false;
    repairOptions.runAdaptiveSewing = false;
    repairOptions.keepInternalPatchSeams = true;
    repairOptions.sewingTolerance = std::max(
        request.options.curveOnSurfaceTolerance, static_cast<double>(Precision::Confusion()));
    repairOptions.preferredSewingTolerance = repairOptions.sewingTolerance;
    repairOptions.minSewingTolerance = repairOptions.sewingTolerance;
    repairOptions.maxSewingTolerance = repairOptions.sewingTolerance;
    const auto repaired = repairPatchReplacementShape(assembly.shape, repairOptions);
    const auto sewingEnded = std::chrono::steady_clock::now();
    RegionRefitStageReport sewingStage;
    sewingStage.stage = RegionRefitStage::Sewing;
    sewingStage.success = repaired.success && !repaired.shape.IsNull();
    sewingStage.wallMilliseconds = std::chrono::duration<double, std::milli>(
        sewingEnded - sewingStarted).count();
    sewingStage.message = repaired.report.message;
    const auto sewingMemoryExceeded = finishStageResources(
        sewingStage, sewingResources, result.report, request.options);
    result.report.stages.push_back(sewingStage);
    result.report.totalWallMilliseconds += sewingStage.wallMilliseconds;
    result.report.repairPassed = repaired.success;
    result.report.sewingConstructedShape = !repaired.shape.IsNull();
    if (!repaired.shape.IsNull()) result.diagnosticSewnShape = repaired.shape;
    result.report.selectedSewingTolerance = repaired.report.selectedSewingTolerance;
    result.report.sewingAttemptCount = repaired.report.sewingAttemptCount;
    result.report.freeEdgesBeforeRepair = repaired.report.freeEdgesBeforeRepair;
    result.report.freeEdgesAfterRepair = repaired.report.freeEdgesAfterRepair;
    result.report.multipleEdgesBeforeRepair = repaired.report.multipleEdgesBeforeRepair;
    result.report.multipleEdgesAfterRepair = repaired.report.multipleEdgesAfterRepair;
    result.report.diagnosticPipelineCompleted = true;
    if (!sewingStage.success && repaired.shape.IsNull()) {
        result.report.status = RegionRefitStatus::Failed;
        result.report.failureStage = RegionRefitStage::Sewing;
        result.report.message = sewingStage.message;
        result.report.continuationStopReason = sewingStage.message;
        markNoProductResult(result, sewingStage.message);
        return result;
    }
    if (sewingMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::Sewing);
        return result;
    }
    if (result.report.totalWallMilliseconds > static_cast<double>(request.options.maximumWallClockMilliseconds)) {
        result.report.status = RegionRefitStatus::TimeBudgetExceeded;
        result.report.failureStage = RegionRefitStage::Sewing;
        result.report.message = "Replacement sewing exceeded the total wall-clock budget.";
        return result;
    }
    if (request.diagnosticStopAfterStage == RegionRefitStage::Sewing) {
        result.report.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.report.failureStage = RegionRefitStage::StrictTopology;
        result.report.message = "Sewing-only diagnostic completed.";
        return result;
    }

    const ShapeDocument afterDocument(result.diagnosticSewnShape, document.sourcePath());
    StrictTopologyGateInput strictInput;
    strictInput.beforeDocument = &document;
    strictInput.afterDocument = &afterDocument;
    strictInput.replacementFaceCount = nativeBuild.replacementFaces.size();
    strictInput.requireStepRoundtrip = false;
    strictInput.allowMultiFaceReplacement = true;
    strictInput.allowFaceCountIncrease = false;
    strictInput.requireWatertightSolid = document.stats().solids > 0;
    strictInput.requireZeroFreeEdges = true;
    strictInput.requireZeroMultipleEdges = true;
    strictInput.requireRoundtripWatertight = true;
    const auto strictResources = captureRegionProcessResources();
    const auto strictStarted = std::chrono::steady_clock::now();
    auto strictReport = StrictTopologyGate().evaluate(strictInput);
    if (strictReport.passed && strictReport.afterStats.faces >= strictReport.beforeStats.faces) {
        strictReport.passed = false;
        strictReport.failureReason = StrictTopologyFailureReason::ReplacementStatsInvalid;
        strictReport.message = "Native regional refit did not reduce the final whole-part face count.";
    }
    const auto strictEnded = std::chrono::steady_clock::now();
    RegionRefitStageReport strictStage;
    strictStage.stage = RegionRefitStage::StrictTopology;
    strictStage.success = strictReport.passed;
    strictStage.wallMilliseconds = std::chrono::duration<double, std::milli>(
        strictEnded - strictStarted).count();
    strictStage.message = strictReport.message;
    const auto strictMemoryExceeded = finishStageResources(
        strictStage, strictResources, result.report, request.options);
    result.report.stages.push_back(strictStage);
    result.report.totalWallMilliseconds += strictStage.wallMilliseconds;
    copyStrictTopologyReport(result.report, strictReport);
    if (!strictReport.passed && request.executionMode == RegionRefitExecutionMode::ValidatedOnly) {
        result.report.status = RegionRefitStatus::Failed;
        result.report.failureStage = RegionRefitStage::StrictTopology;
        result.report.message = strictReport.message;
        markNoProductResult(result, strictReport.message);
        return result;
    }
    if (strictMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::StrictTopology);
        return result;
    }
    if (request.diagnosticStopAfterStage == RegionRefitStage::StrictTopology) {
        result.report.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.report.failureStage = RegionRefitStage::StepRoundtrip;
        result.report.message = "Strict-topology-only diagnostic completed.";
        return result;
    }

    strictInput.requireStepRoundtrip = true;
    const auto roundtripResources = captureRegionProcessResources();
    const auto roundtripStarted = std::chrono::steady_clock::now();
    const auto roundtripReport = StrictTopologyGate().evaluate(strictInput);
    const auto roundtripEnded = std::chrono::steady_clock::now();
    RegionRefitStageReport roundtripStage;
    roundtripStage.stage = RegionRefitStage::StepRoundtrip;
    roundtripStage.success = roundtripReport.passed;
    roundtripStage.wallMilliseconds = std::chrono::duration<double, std::milli>(
        roundtripEnded - roundtripStarted).count();
    roundtripStage.message = roundtripReport.message;
    const auto roundtripMemoryExceeded = finishStageResources(
        roundtripStage, roundtripResources, result.report, request.options);
    result.report.stages.push_back(roundtripStage);
    result.report.totalWallMilliseconds += roundtripStage.wallMilliseconds;
    copyStrictTopologyReport(result.report, roundtripReport);
    if (!roundtripReport.passed && request.executionMode == RegionRefitExecutionMode::ValidatedOnly) {
        result.report.status = RegionRefitStatus::Failed;
        result.report.failureStage = RegionRefitStage::StepRoundtrip;
        result.report.message = roundtripReport.message;
        markNoProductResult(result, roundtripReport.message);
        return result;
    }
    if (roundtripMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::StepRoundtrip);
        return result;
    }
    const auto finalValidationMilliseconds = sewingStage.wallMilliseconds +
        strictStage.wallMilliseconds + roundtripStage.wallMilliseconds;
    if (finalValidationMilliseconds >
            static_cast<double>(request.options.finalValidationBudgetMilliseconds) ||
        result.report.totalWallMilliseconds >
            static_cast<double>(request.options.maximumWallClockMilliseconds)) {
        result.report.status = RegionRefitStatus::TimeBudgetExceeded;
        result.report.failureStage = RegionRefitStage::StepRoundtrip;
        result.report.message = "Sewing, topology, and STEP roundtrip exceeded the final validation budget.";
        return result;
    }

    const auto fullyValidated = result.report.fitGatePassed && result.report.retrimValidationPassed &&
        result.report.repairPassed && strictReport.passed && roundtripReport.passed;
    result.report.outputFaceCount = roundtripReport.afterStats.faces;
    if (fullyValidated) {
        if (request.layoutMode == RegionRefitLayoutMode::TargetPatchCount) {
            result.report.selectionOutcome = result.report.exactTargetReached
                ? "ExactTargetValidated" : "ClosestValidated";
            if (!result.report.exactTargetReached) {
                result.report.unattainableReason = "The requested patch count did not produce a fully validated layout within the configured search radius.";
            }
        } else if (request.layoutMode == RegionRefitLayoutMode::Adaptive) {
            result.report.minimumQualifiedProven = true;
            for (int patchCount = 1; patchCount < result.report.selectedPatchCount; ++patchCount) {
                const auto layout = std::find_if(
                    result.report.layoutCandidates.begin(),
                    result.report.layoutCandidates.end(),
                    [&](const auto& candidate) { return candidate.patchCount == patchCount; });
                const auto fit = std::find_if(
                    result.report.fitAttempts.begin(),
                    result.report.fitAttempts.end(),
                    [&](const auto& attempt) { return attempt.patchCount == patchCount; });
                const auto hardRejected = layout != result.report.layoutCandidates.end() &&
                    (!layout->topologyPassed || !layout->parameterizationPassed ||
                        !layout->coarseFitPassed);
                const auto fullyRejected = fit != result.report.fitAttempts.end() && !fit->success;
                if (!hardRejected && !fullyRejected) {
                    result.report.minimumQualifiedProven = false;
                    break;
                }
            }
            result.report.selectionOutcome = result.report.minimumQualifiedProven
                ? "MinimumQualified" : "BestValidatedWithinBudget";
            if (!result.report.minimumQualifiedProven) {
                result.report.unattainableReason = "At least one smaller patch count was not conclusively rejected before the full-fit budget was exhausted.";
            }
        } else {
            result.report.selectionOutcome = "FixedValidated";
        }
        result.finalShape = result.diagnosticSewnShape;
        result.report.status = RegionRefitStatus::Success;
        result.report.failureStage = RegionRefitStage::None;
        result.report.message = "Native regional multi-surface refit passed retrimming, sewing, strict topology, and STEP roundtrip gates.";
    } else {
        result.report.selectionOutcome = "NoProductValidatedResult";
        result.report.unattainableReason = "A fitted or sewn diagnostic candidate did not pass every product geometry and topology gate.";
        result.report.status = RegionRefitStatus::Failed;
        result.report.failureStage = !result.report.fitGatePassed
            ? RegionRefitStage::Fitting
            : (!result.report.retrimValidationPassed
                ? RegionRefitStage::Retrim
                : (!result.report.repairPassed
                    ? RegionRefitStage::Sewing
                    : (!strictReport.passed ? RegionRefitStage::StrictTopology : RegionRefitStage::StepRoundtrip)));
        result.report.message = "Best-effort regional refit completed through sewing and validation but did not pass all product gates.";
        result.report.continuationStopReason = result.report.message;
    }
    return result;
}

}
