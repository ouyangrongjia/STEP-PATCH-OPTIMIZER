#include "reduction/region_refit/RegionalSurfaceRefitEngine.h"

#include "reduction/region_refit/RegionConstraintExtractor.h"
#include "reduction/region_refit/RegionCurveNetwork.h"
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
    replacement.multiSurfaceBoundaryEdgePcurveRebuildAttemptCount =
        native.storedPcurveAttachmentCount;
    replacement.multiSurfaceBoundaryEdgePcurveRebuildSuccessCount =
        native.storedPcurveAttachmentCount -
        native.sameParameterFailureCount;
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
    result.report.forceDiagnosticSewingRequested = request.forceDiagnosticSewing;
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
        request.options.curveProjectionTolerance <= 0.0 ||
        request.options.curveValidationSampleCount < 97 ||
        request.options.curveFairingIterations < 0 ||
        request.options.curveFairingWeight < 0.0 ||
        request.options.curveFairingWeight > 1.0 ||
        request.options.g1ToleranceDegrees <= 0.0 ||
        request.options.g2CurvatureTolerance <= 0.0 ||
        request.options.seamG1RelativeWeight <= 0.0 ||
        request.options.seamG2RelativeWeight <= 0.0 ||
        request.options.maximumControlPointsPerSide < 6 ||
        request.options.initialControlPriorRelativeWeight <= 0.0 ||
        request.options.constraintCorrectionAnchorRelativeWeight <= 0.0 ||
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
        result.report.failureStage = RegionRefitStage::CurveNetwork;
        result.report.message = "Sampling-only diagnostic completed.";
        return result;
    }

    const auto curveNetworkResources = captureRegionProcessResources();
    const auto curveNetworkStarted = std::chrono::steady_clock::now();
    RegionCurveNetworkGenerationResult curveNetworks;
    RegionRefitCacheEvent curveNetworkCacheEvent;
    const auto curveNetworkCacheHit = cache.loadCurveNetworks(
        result.constraints,
        curveNetworks,
        curveNetworkCacheEvent);
    if (!curveNetworkCacheHit) {
        curveNetworks = RegionCurveNetworkGenerator().generate(
            *result.layoutCarrierMesh,
            result.constraints,
            request.layoutMode,
            request.options,
            true);
        if (curveNetworks.success() && cache.enabled()) {
            const auto writeEvent = cache.saveCurveNetworks(curveNetworks);
            curveNetworkCacheEvent.writeMilliseconds =
                writeEvent.writeMilliseconds;
            if (!writeEvent.message.empty()) {
                curveNetworkCacheEvent.message += " " + writeEvent.message;
            }
        }
    }
    const auto curveNetworkEnded = std::chrono::steady_clock::now();
    RegionRefitStageReport curveNetworkStage;
    curveNetworkStage.stage = RegionRefitStage::CurveNetwork;
    curveNetworkStage.success = curveNetworks.success();
    curveNetworkStage.wallMilliseconds =
        std::chrono::duration<double, std::milli>(
            curveNetworkEnded - curveNetworkStarted).count();
    curveNetworkStage.message = curveNetworkCacheHit
        ? "Loaded frozen regional curve networks from algorithm cache."
        : curveNetworks.message;
    const auto curveNetworkMemoryExceeded = finishStageResources(
        curveNetworkStage,
        curveNetworkResources,
        result.report,
        request.options);
    result.report.stages.push_back(curveNetworkStage);
    result.report.totalWallMilliseconds +=
        curveNetworkStage.wallMilliseconds;
    result.report.layoutCandidates = curveNetworks.layoutReports;
    if (cache.enabled()) {
        result.report.cacheEvents.push_back(curveNetworkCacheEvent);
    }
    for (const auto& candidate : curveNetworks.constructibleCandidates) {
        result.generatedCurveNetworks.push_back(
            std::make_shared<RegionCurveNetwork>(candidate.network));
    }
    if (!curveNetworks.success()) {
        result.report.status = RegionRefitStatus::NoSewableGeometry;
        result.report.failureStage = RegionRefitStage::CurveNetwork;
        result.report.firstProductFailureGate = "CurveNetwork";
        result.report.selectionOutcome = "NoSewableGeometry";
        result.report.unattainableReason = curveNetworks.message;
        result.report.message = curveNetworks.message;
        return result;
    }
    result.selectedCurveNetwork = std::make_shared<RegionCurveNetwork>(
        curveNetworks.selectedCandidates.front().network);
    const auto& selectedCurveReport =
        result.selectedCurveNetwork->report;
    result.report.selectedCurveNetworkHash =
        result.selectedCurveNetwork->hash;
    result.report.curveNetworkNodeCount = selectedCurveReport.nodeCount;
    result.report.curveNetworkEdgeCount = selectedCurveReport.edgeCount;
    result.report.curveNetworkInternalEdgeCount =
        selectedCurveReport.internalEdgeCount;
    result.report.curveNetworkBoundaryEdgeCount =
        selectedCurveReport.boundaryEdgeCount;
    result.report.curveNetworkG0EdgeCount = selectedCurveReport.g0EdgeCount;
    result.report.curveNetworkG1EdgeCount = selectedCurveReport.g1EdgeCount;
    result.report.curveNetworkG2EdgeCount = selectedCurveReport.g2EdgeCount;
    result.report.curveNetworkRegularizedCubicEdgeCount =
        selectedCurveReport.regularizedCubicEdgeCount;
    result.report.curveNetworkDiagnosticPolylineEdgeCount =
        selectedCurveReport.diagnosticPolylineEdgeCount;
    result.report.curveNetworkContinuityDowngradeCount =
        selectedCurveReport.continuityDowngradeCount;
    result.report.curveNetworkMaximumProjectionError =
        selectedCurveReport.maximumProjectionError;
    result.report.curveNetworkMaximumSampleTurnDegrees =
        selectedCurveReport.maximumSampleTurnDegrees;
    result.report.curveNetworkMaximumBendingEnergy =
        selectedCurveReport.maximumBendingEnergy;
    result.report.curveNetworkSelfIntersectionCount =
        selectedCurveReport.selfIntersectionCount;
    result.report.curveNetworkUnexpectedIntersectionCount =
        selectedCurveReport.unexpectedCurveIntersectionCount;
    result.report.curveNetworkClosurePassed =
        selectedCurveReport.closurePassed;
    result.report.curveNetworkOwnershipPassed =
        selectedCurveReport.ownershipPassed;
    result.report.curveNetworkProjectionPassed =
        selectedCurveReport.projectionPassed;
    result.report.curveNetworkOrientationPassed =
        selectedCurveReport.orientationPassed;
    result.report.curveNetworkIntersectionPassed =
        selectedCurveReport.intersectionPassed;
    result.report.curveNetworkProductQualityPassed =
        selectedCurveReport.productQualityPassed;
    result.report.curveNetworkFrozen = selectedCurveReport.frozen;
    if (!selectedCurveReport.productQualityPassed) {
        result.report.firstProductFailureGate =
            "CurveNetworkQuality";
    }
    if (curveNetworkMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::CurveNetwork);
        return result;
    }
    if (request.diagnosticStopAfterStage == RegionRefitStage::CurveNetwork) {
        result.report.status =
            RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.report.failureStage = RegionRefitStage::Layout;
        result.report.message = "Curve-network-only diagnostic completed.";
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
            curveNetworks,
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
        result.report.status = RegionRefitStatus::NoSewableGeometry;
        result.report.failureStage = RegionRefitStage::Layout;
        if (result.report.firstProductFailureGate.empty()) {
            result.report.firstProductFailureGate = "Layout";
        }
        result.report.message = layouts.message;
        result.report.selectionOutcome = "NoSewableGeometry";
        result.report.unattainableReason = layouts.message;
        return result;
    }
    for (const auto& layout : layouts.constructibleCandidates) {
        result.generatedLayouts.push_back(std::make_shared<RegionPatchLayout>(layout));
    }
    if (layoutMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::Layout);
        return result;
    }
    result.selectedLayout = std::make_shared<RegionPatchLayout>(
        layouts.selectedCandidates.front());
    result.report.selectedPatchCount = result.selectedLayout->patchCount;
    result.report.selectedLayoutHash = result.selectedLayout->hash;
    const auto selectedLayoutReport = std::find_if(
        result.report.layoutCandidates.begin(),
        result.report.layoutCandidates.end(),
        [&](const auto& candidate) {
            return candidate.layoutHash == result.report.selectedLayoutHash;
        });
    result.report.layoutGatePassed =
        selectedLayoutReport != result.report.layoutCandidates.end() &&
        selectedLayoutReport->topologyPassed &&
        selectedLayoutReport->parameterizationPassed &&
        selectedLayoutReport->coarseFitPassed;
    if (!result.report.layoutGatePassed &&
        result.report.firstProductFailureGate.empty()) {
        result.report.firstProductFailureGate = "Layout";
    }
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
    std::vector<RegionCurveNetwork> fitCurveNetworks;
    fitCurveNetworks.reserve(curveNetworks.constructibleCandidates.size());
    for (const auto& candidate : curveNetworks.constructibleCandidates) {
        fitCurveNetworks.push_back(candidate.network);
    }
    JointBSplineNetworkFitResult fitted;
    RegionRefitCacheEvent fitCacheEvent;
    const auto fitCacheLoaded =
        request.executionMode == RegionRefitExecutionMode::ValidatedOnly &&
        cache.loadFit(layouts.selectedCandidates, fitted, fitCacheEvent);
    const auto fitCacheHit = fitCacheLoaded && fitted.success();
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
                fitCurveNetworks,
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
    const auto fittedProductConstructible =
        fitted.success() &&
        fitted.network != nullptr &&
        fitted.network->constructibleForBRep;
    result.report.fitGatePassed = fittedProductConstructible;
    const auto useBestEffort = !fitted.success() &&
        fitted.bestEffortNetwork != nullptr &&
        fitted.bestEffortNetwork->constructibleForBRep;
    result.report.bestEffortUsed = useBestEffort;
    if (!fitted.success() && !useBestEffort) {
        result.report.status = RegionRefitStatus::NoSewableGeometry;
        result.report.failureStage = RegionRefitStage::Fitting;
        result.report.message = fitted.message;
        result.report.patchCountDeviation = result.report.requestedPatchCount > 0
            ? result.report.selectedPatchCount - result.report.requestedPatchCount
            : 0;
        result.report.exactTargetReached = result.report.requestedPatchCount > 0 &&
            result.report.patchCountDeviation == 0;
        result.report.selectionOutcome = "NoSewableGeometry";
        result.report.unattainableReason =
            "No finite, non-flipped fitted support-surface network was available for B-rep construction.";
        if (result.report.firstProductFailureGate.empty()) {
            result.report.firstProductFailureGate = "Fitting";
        }
        return result;
    }
    if (fittingMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::Fitting);
        return result;
    }
    result.fittedNetwork =
        fittedProductConstructible ? fitted.network : fitted.bestEffortNetwork;
    result.report.sewableGeometryAvailable = result.fittedNetwork != nullptr;
    result.report.diagnosticCandidatePatchCount = result.fittedNetwork->layout.patchCount;
    result.report.diagnosticSelectionReason =
        "Selected the constructible fitted network by hard-gate count, normalized product-gate violation, patch count, proxy error, and stable hash.";
    result.report.diagnosticSewingCandidateCount = 1;
    result.selectedLayout = std::make_shared<RegionPatchLayout>(result.fittedNetwork->layout);
    result.fittedPatchNetwork = result.fittedNetwork->inspectionShape;
    result.report.selectedPatchCount = result.fittedNetwork->layout.patchCount;
    result.report.patchCountDeviation = result.report.requestedPatchCount > 0
        ? result.report.selectedPatchCount - result.report.requestedPatchCount
        : 0;
    result.report.exactTargetReached = result.report.requestedPatchCount > 0 &&
        result.report.patchCountDeviation == 0;
    result.report.selectedLayoutHash = result.fittedNetwork->layout.hash;
    for (const auto& patch : result.fittedNetwork->layout.patches) {
        result.report.pcurveSelfIntersectionCount +=
            patch.pcurveSelfIntersectionCount;
        result.report.pcurveNonAdjacentIntersectionCount +=
            patch.pcurveNonAdjacentIntersectionCount;
    }
    const auto selectedAttempt = std::find_if(
        fitted.attempts.begin(), fitted.attempts.end(),
        [&](const auto& attempt) {
            return attempt.layoutHash == result.report.selectedLayoutHash;
        });
    if (selectedAttempt != fitted.attempts.end() &&
        !selectedAttempt->iterations.empty()) {
        const auto& finalIteration = selectedAttempt->iterations.back();
        result.report.boundaryFeasibilityRefinementRounds =
            finalIteration.boundaryFeasibilityRefinementRounds;
        result.report.boundaryFeasibilityResidual =
            finalIteration.boundaryFeasibilityResidual;
        result.report.fittedMaximumUControlCount =
            finalIteration.uControlCount;
        result.report.fittedMaximumVControlCount =
            finalIteration.vControlCount;
    }
    const auto fittedCurveNetwork = std::find_if(
        fitCurveNetworks.begin(),
        fitCurveNetworks.end(),
        [&](const auto& candidate) {
            return candidate.hash ==
                result.fittedNetwork->layout.curveNetworkHash;
        });
    if (fittedCurveNetwork == fitCurveNetworks.end()) {
        result.report.status = RegionRefitStatus::NoSewableGeometry;
        result.report.failureStage = RegionRefitStage::CurveNetwork;
        result.report.firstProductFailureGate = "CurveNetwork";
        result.report.selectionOutcome = "NoSewableGeometry";
        result.report.unattainableReason =
            "The fitted layout lost its frozen curve-network identity.";
        result.report.message = result.report.unattainableReason;
        return result;
    }
    result.selectedCurveNetwork =
        std::make_shared<RegionCurveNetwork>(*fittedCurveNetwork);
    result.report.selectedCurveNetworkHash =
        result.selectedCurveNetwork->hash;
    const auto& fittedCurveReport = result.selectedCurveNetwork->report;
    result.report.curveNetworkNodeCount = fittedCurveReport.nodeCount;
    result.report.curveNetworkEdgeCount = fittedCurveReport.edgeCount;
    result.report.curveNetworkInternalEdgeCount =
        fittedCurveReport.internalEdgeCount;
    result.report.curveNetworkBoundaryEdgeCount =
        fittedCurveReport.boundaryEdgeCount;
    result.report.curveNetworkG0EdgeCount = fittedCurveReport.g0EdgeCount;
    result.report.curveNetworkG1EdgeCount = fittedCurveReport.g1EdgeCount;
    result.report.curveNetworkG2EdgeCount = fittedCurveReport.g2EdgeCount;
    result.report.curveNetworkRegularizedCubicEdgeCount =
        fittedCurveReport.regularizedCubicEdgeCount;
    result.report.curveNetworkDiagnosticPolylineEdgeCount =
        fittedCurveReport.diagnosticPolylineEdgeCount;
    result.report.curveNetworkContinuityDowngradeCount =
        fittedCurveReport.continuityDowngradeCount;
    result.report.curveNetworkMaximumProjectionError =
        fittedCurveReport.maximumProjectionError;
    result.report.curveNetworkMaximumSampleTurnDegrees =
        fittedCurveReport.maximumSampleTurnDegrees;
    result.report.curveNetworkMaximumBendingEnergy =
        fittedCurveReport.maximumBendingEnergy;
    result.report.curveNetworkSelfIntersectionCount =
        fittedCurveReport.selfIntersectionCount;
    result.report.curveNetworkUnexpectedIntersectionCount =
        fittedCurveReport.unexpectedCurveIntersectionCount;
    result.report.curveNetworkClosurePassed =
        fittedCurveReport.closurePassed;
    result.report.curveNetworkOwnershipPassed =
        fittedCurveReport.ownershipPassed;
    result.report.curveNetworkProjectionPassed =
        fittedCurveReport.projectionPassed;
    result.report.curveNetworkOrientationPassed =
        fittedCurveReport.orientationPassed;
    result.report.curveNetworkIntersectionPassed =
        fittedCurveReport.intersectionPassed;
    result.report.curveNetworkProductQualityPassed =
        fittedCurveReport.productQualityPassed;
    result.report.curveNetworkFrozen = fittedCurveReport.frozen;
    if (!fittedCurveReport.productQualityPassed &&
        result.report.firstProductFailureGate.empty()) {
        result.report.firstProductFailureGate =
            "CurveNetworkQuality";
    }
    const auto fittedLayoutReport = std::find_if(
        result.report.layoutCandidates.begin(),
        result.report.layoutCandidates.end(),
        [&](const auto& candidate) {
            return candidate.layoutHash == result.report.selectedLayoutHash;
        });
    result.report.layoutGatePassed =
        fittedLayoutReport != result.report.layoutCandidates.end() &&
        fittedLayoutReport->topologyPassed &&
        fittedLayoutReport->parameterizationPassed &&
        fittedLayoutReport->coarseFitPassed;
    if (!result.report.layoutGatePassed &&
        result.report.firstProductFailureGate.empty()) {
        result.report.firstProductFailureGate = "Layout";
    } else if (!fitted.success() &&
        result.report.firstProductFailureGate.empty()) {
        result.report.firstProductFailureGate = "Fitting";
    }
    if (request.diagnosticStopAfterStage == RegionRefitStage::Fitting) {
        result.report.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.report.failureStage = RegionRefitStage::Retrim;
        result.report.message = "Fitting-only diagnostic completed.";
        return result;
    }

    const auto retrimResources = captureRegionProcessResources();
    const auto retrimStarted = std::chrono::steady_clock::now();
    result.report.retrimAttempted = true;
    result.report.forceDiagnosticSewingAttempted = true;
    const auto nativeBuild = NativeRegionPatchNetworkBuilder().build(
        *result.layoutCarrierMesh,
        result.constraints,
        *result.fittedNetwork,
        *result.selectedCurveNetwork,
        request.options.curveOnSurfaceTolerance,
        true);
    if (nativeBuild.constructible()) {
        result.replacementQuilt = nativeBuild.replacementShape;
    }
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
    result.report.retrimStoredPcurveAttachmentCount =
        nativeBuild.storedPcurveAttachmentCount;
    result.report.retrimDiagnosticPcurveFallbackCount =
        nativeBuild.diagnosticPcurveFallbackCount;
    result.report.retrimSameParameterCheckCount = nativeBuild.sameParameterCheckCount;
    result.report.retrimSameParameterFailureCount = nativeBuild.sameParameterFailureCount;
    result.report.retrimMaximumSameParameterDeviation = nativeBuild.maximumSameParameterDeviation;
    result.report.unexpectedFaceIntersectionCount =
        nativeBuild.unexpectedFaceIntersectionCount;
    result.report.unexpectedFaceIntersectionTotalLength =
        nativeBuild.unexpectedFaceIntersectionTotalLength;
    result.report.replacementQuiltInternalFreeEdgeCount =
        nativeBuild.replacementQuiltInternalFreeEdgeCount;
    result.report.edgeConsistency = nativeBuild.edgeConsistency;
    result.report.assemblyPassed = assembly.success;
    result.report.retrimValidationPassed = retrimStage.success;
    if (!nativeBuild.success() &&
        result.report.firstProductFailureGate.empty()) {
        result.report.firstProductFailureGate = "Retrim";
    }
    const auto retrimConstructed = nativeBuild.constructible() && boundary.valid && assembly.success &&
        !assembly.shape.IsNull();
    if (!retrimConstructed) {
        result.report.status = RegionRefitStatus::NoSewableGeometry;
        result.report.failureStage = RegionRefitStage::Retrim;
        result.report.message = retrimStage.message;
        result.report.continuationStopReason = retrimStage.message;
        if (result.report.firstProductFailureGate.empty()) {
            result.report.firstProductFailureGate = "Retrim";
        }
        result.report.selectionOutcome = "NoSewableGeometry";
        result.report.sewableGeometryAvailable = false;
        result.report.unattainableReason =
            "The fitted surfaces could not be constructed as a patch-network B-rep.";
        return result;
    }
    if (retrimMemoryExceeded) {
        failMemoryBudget(result, RegionRefitStage::Retrim);
        return result;
    }
    const auto retrimTimeExceeded =
        retrimStage.wallMilliseconds >
            static_cast<double>(request.options.retrimBudgetMilliseconds) ||
        result.report.totalWallMilliseconds >
            static_cast<double>(request.options.maximumWallClockMilliseconds);
    if (retrimTimeExceeded &&
        request.executionMode !=
            RegionRefitExecutionMode::BestEffortThroughSewing &&
        !request.forceDiagnosticSewing) {
        result.report.status = RegionRefitStatus::TimeBudgetExceeded;
        result.report.failureStage = RegionRefitStage::Retrim;
        result.report.message = "Native retrimming exceeded its wall-clock budget.";
        return result;
    }
    if (retrimTimeExceeded) {
        result.report.continuationStopReason =
            "Native retrimming exceeded its quality budget; diagnostic sewing continued.";
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

    const auto fullyValidated =
        result.report.curveNetworkProductQualityPassed &&
        result.report.layoutGatePassed &&
        result.report.fitGatePassed && result.report.retrimValidationPassed &&
        result.report.repairPassed && strictReport.passed && roundtripReport.passed;
    result.report.productValidated = fullyValidated;
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
        if (result.report.firstProductFailureGate.empty()) {
            result.report.firstProductFailureGate = !result.report.retrimValidationPassed
                ? "Retrim"
                : (!result.report.repairPassed
                    ? "Sewing"
                    : (!strictReport.passed ? "StrictTopology" : "StepRoundtrip"));
        }
        result.report.selectionOutcome = "NoProductValidatedResult";
        result.report.unattainableReason = "A fitted or sewn diagnostic candidate did not pass every product geometry and topology gate.";
        result.report.status = RegionRefitStatus::Failed;
        result.report.failureStage =
            !result.report.curveNetworkProductQualityPassed
            ? RegionRefitStage::CurveNetwork
            : (!result.report.layoutGatePassed
                ? RegionRefitStage::Layout
                : (!result.report.fitGatePassed
                ? RegionRefitStage::Fitting
                : (!result.report.retrimValidationPassed
                    ? RegionRefitStage::Retrim
                    : (!result.report.repairPassed
                        ? RegionRefitStage::Sewing
                        : (!strictReport.passed
                            ? RegionRefitStage::StrictTopology
                            : RegionRefitStage::StepRoundtrip)))));
        result.report.message = "Best-effort regional refit completed through sewing and validation but did not pass all product gates.";
        result.report.continuationStopReason = result.report.message;
    }
    return result;
}

}
