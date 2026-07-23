#pragma once

#include "common/GeometryTypes.h"

#include <Geom2d_Curve.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace spo {

class RegionReferenceMesh;
struct RegionPatchLayout;
struct RegionFittedPatchNetwork;

enum class RegionRefitLayoutMode {
    FixedPatchCount,
    TargetPatchCount,
    Adaptive
};

enum class RegionRefitAdaptiveObjective {
    MinimumQualified
};

enum class RegionRefitSamplingMode {
    AdaptiveOcct,
    QualityBalanced,
    UniformUv
};

enum class RegionRefitExecutionMode {
    ValidatedOnly,
    BestEffortThroughSewing
};

enum class RegionRefitStatus {
    NotStarted,
    Success,
    InvalidRequest,
    UnsupportedTopology,
    TimeBudgetExceeded,
    MemoryBudgetExceeded,
    SamplingBudgetExceeded,
    NoValidatedLayoutWithinBudget,
    NoSewableGeometry,
    Failed
};

enum class RegionRefitStage {
    None,
    Constraints,
    Sampling,
    Layout,
    Fitting,
    Retrim,
    Sewing,
    StrictTopology,
    StepRoundtrip,
    ExternalCad
};

enum class RegionBoundaryContinuity {
    Free,
    HardCrease,
    Smooth,
    Locked
};

enum class RegionRefitCachePolicy {
    Disabled,
    ReadWrite,
    ReadOnly,
    Refresh
};

const char* toString(RegionRefitLayoutMode mode);
const char* toString(RegionRefitAdaptiveObjective objective);
const char* toString(RegionRefitSamplingMode mode);
const char* toString(RegionRefitExecutionMode mode);
const char* toString(RegionRefitStatus status);
const char* toString(RegionRefitStage stage);
const char* toString(RegionBoundaryContinuity continuity);
const char* toString(RegionRefitCachePolicy policy);

struct RegionRefitOptions {
    RegionRefitSamplingMode samplingMode = RegionRefitSamplingMode::AdaptiveOcct;
    RegionRefitSamplingMode layoutCarrierSamplingMode = RegionRefitSamplingMode::QualityBalanced;
    double tolerance3d = 0.03;
    double normalToleranceDegrees = 15.0;
    double curveOnSurfaceTolerance = 0.001;
    std::size_t maximumSampleTriangles = 200000;
    std::size_t targetSampleTriangles = 80000;
    std::size_t targetLayoutCarrierTriangles = 80000;
    int maximumRemeshSweeps = 8;
    double maximumLocalSizeRatio = 1.5;
    int fixedPatchCount = 0;
    int requestedPatchCount = 0;
    int patchCountSearchRadius = 0;
    RegionRefitAdaptiveObjective adaptiveObjective = RegionRefitAdaptiveObjective::MinimumQualified;
    int maximumAdaptivePatchCount = 12;
    int maximumFullFitCount = 3;
    int maximumFitIterations = 20;
    std::int64_t maximumWallClockMilliseconds = 900000;
    std::int64_t inputReadBudgetMilliseconds = 60000;
    std::int64_t constraintsBudgetMilliseconds = 10000;
    std::int64_t samplingBudgetMilliseconds = 30000;
    std::int64_t layoutBudgetMilliseconds = 60000;
    std::int64_t singleFitBudgetMilliseconds = 90000;
    std::int64_t totalFitBudgetMilliseconds = 270000;
    std::int64_t retrimBudgetMilliseconds = 45000;
    std::int64_t finalValidationBudgetMilliseconds = 210000;
    std::size_t maximumMemoryMiB = 8192;
    std::size_t solverMemoryPreflightMiB = 7168;
    double maximumParameterDistortion = 100.0;
    int maximumControlPointsPerSide = 25;
    // Relative to the mean data-and-fairness normal-matrix diagonal.
    double constraintCorrectionAnchorRelativeWeight = 0.25;
    RegionRefitCachePolicy cachePolicy = RegionRefitCachePolicy::Disabled;
    std::filesystem::path cacheDirectory;
    std::string inputStepSha256;
    std::string algorithmFingerprint;
};

struct RegionRefitRequest {
    std::vector<FaceId> selectedFaces;
    std::vector<EdgeId> lockedEdges;
    RegionRefitLayoutMode layoutMode = RegionRefitLayoutMode::Adaptive;
    RegionRefitExecutionMode executionMode = RegionRefitExecutionMode::ValidatedOnly;
    bool forceDiagnosticSewing = false;
    RegionRefitStage diagnosticStopAfterStage = RegionRefitStage::ExternalCad;
    RegionRefitOptions options;
};

struct RegionBoundaryEdgeConstraint {
    EdgeId edgeId = 0;
    FaceId selectedFaceId = 0;
    std::optional<FaceId> adjacentFaceId;
    TopoDS_Edge originalEdge;
    TopoDS_Face selectedFace;
    TopoDS_Face adjacentFace;
    TopAbs_Orientation loopOrientation = TopAbs_FORWARD;
    TopAbs_Orientation selectedFaceOrientation = TopAbs_FORWARD;
    TopAbs_Orientation adjacentFaceOrientation = TopAbs_FORWARD;
    Handle(Geom2d_Curve) selectedPcurve;
    Handle(Geom2d_Curve) adjacentPcurve;
    double selectedFirstParameter = 0.0;
    double selectedLastParameter = 0.0;
    double adjacentFirstParameter = 0.0;
    double adjacentLastParameter = 0.0;
    RegionBoundaryContinuity continuity = RegionBoundaryContinuity::HardCrease;
    bool hardPositionConstraint = true;
    bool softAdjacentNormalConstraint = false;
};

struct RegionTopologyConstraints {
    std::vector<FaceId> selectedFaces;
    std::vector<EdgeId> internalEdges;
    std::vector<EdgeId> lockedInternalEdges;
    std::vector<EdgeId> orderedBoundaryEdges;
    std::vector<RegionBoundaryEdgeConstraint> boundaryEdges;
    TopoDS_Wire outerWire;
};

struct RegionRefitStageReport {
    RegionRefitStage stage = RegionRefitStage::None;
    bool success = false;
    double wallMilliseconds = 0.0;
    double cpuMilliseconds = 0.0;
    std::uint64_t peakWorkingSetBytes = 0;
    std::uint64_t peakPrivateBytes = 0;
    std::string message;
};

struct RegionLayoutCandidateReport {
    int patchCount = 0;
    bool topologyPassed = false;
    bool parameterizationPassed = false;
    bool constructibleParameterization = false;
    bool coarseFitPassed = false;
    bool selectedForFullFit = false;
    double proxyError = 0.0;
    double maximumDistortion = 0.0;
    int flippedTriangleCount = 0;
    std::size_t boundaryChainCount = 0;
    int trimCornerCount = 0;
    double minimumSignedUvArea = 0.0;
    std::string domainType;
    std::size_t matrixRows = 0;
    std::size_t matrixNonZeros = 0;
    std::uint64_t estimatedSolverBytes = 0;
    std::uint64_t estimatedOutputEntities = 0;
    double wallMilliseconds = 0.0;
    std::string layoutHash;
    std::string rejectionReason;
};

struct RegionFitIterationReport {
    int iteration = 0;
    int uControlCount = 0;
    int vControlCount = 0;
    std::size_t matrixRows = 0;
    std::size_t matrixColumns = 0;
    std::size_t matrixNonZeros = 0;
    std::uint64_t estimatedSolverBytes = 0;
    int symbolicFactorizationReuseCount = 0;
    std::size_t projectionCount = 0;
    std::size_t uvIterationCount = 0;
    std::size_t projectionFallbackCount = 0;
    std::size_t boundaryHardConstraintCount = 0;
    std::size_t seamG0ConstraintCount = 0;
    std::size_t seamG1SoftConstraintCount = 0;
    std::size_t adjacentNormalSoftConstraintCount = 0;
    std::size_t nonZeroBoundaryRhsCount = 0;
    std::array<double, 3> boundaryTargetMinimum {};
    std::array<double, 3> boundaryTargetMaximum {};
    double maximumHardConstraintResidual = 0.0;
    double constraintCorrectionAnchorScale = 0.0;
    double initialTrainingMaximumDistance = 0.0;
    double initialBoundaryMaximumDistance = 0.0;
    double correctedTrainingMaximumDistance = 0.0;
    double correctedBoundaryMaximumDistance = 0.0;
    double trainingMaximumDistance = 0.0;
    double selectionMaximumDistance = 0.0;
    double holdoutMaximumDistance = 0.0;
    double holdoutMaximumNormalDegrees = 0.0;
    double holdout95Distance = 0.0;
    double holdout95NormalDegrees = 0.0;
    std::size_t holdoutWorstDistanceVertex = 0;
    std::size_t holdoutWorstNormalVertex = 0;
    int holdoutWorstDistancePatch = -1;
    int holdoutWorstNormalPatch = -1;
    bool holdoutWorstDistanceInternalSeam = false;
    bool holdoutWorstNormalInternalSeam = false;
    double boundaryMaximumDistance = 0.0;
    double seamMaximumDistance = 0.0;
    double wallMilliseconds = 0.0;
    std::string stopReason;
};

struct RegionFitAttemptReport {
    int patchCount = 0;
    std::string layoutHash;
    bool success = false;
    double wallMilliseconds = 0.0;
    std::string stopReason;
    std::vector<RegionFitIterationReport> iterations;
};

struct RegionEdgeConsistencyReport {
    int edgeIndex = -1;
    bool internalSeam = false;
    bool sharedSameEdge = false;
    int expectedFaceUseCount = 0;
    int actualFaceUseCount = 0;
    bool freeEdge = false;
    bool multipleEdge = false;
    bool sameRange = false;
    bool sameParameter = false;
    double curveOnSurfaceMaximumDeviation = 0.0;
    double curveOnSurfaceP95Deviation = 0.0;
    double curveOnSurfaceRmsDeviation = 0.0;
    double adjacentSurfaceMaximumSeparation = 0.0;
    double adjacentSurfaceMaximumNormalDegrees = 0.0;
    double worstParameter = 0.0;
    std::vector<double> triangulationDeflections;
    std::vector<double> triangulationPolylineSeparations;
    std::string classification;
};

std::string classifyRegionEdgeConsistency(
    const RegionEdgeConsistencyReport& report,
    double tolerance);

struct RegionRefitCacheEvent {
    std::string layer;
    bool hit = false;
    bool negativeEntry = false;
    double readMilliseconds = 0.0;
    double writeMilliseconds = 0.0;
    std::string message;
};

struct RegionRefitReport {
    RegionRefitStatus status = RegionRefitStatus::NotStarted;
    RegionRefitStage failureStage = RegionRefitStage::None;
    std::string message;
    int inputFaceCount = 0;
    int outputFaceCount = 0;
    int selectedPatchCount = 0;
    int requestedPatchCount = 0;
    int patchCountDeviation = 0;
    bool exactTargetReached = false;
    bool minimumQualifiedProven = false;
    std::string selectionOutcome;
    std::string unattainableReason;
    int fullFitCount = 0;
    int cachedFitAttemptCount = 0;
    std::string selectedLayoutHash;
    std::size_t sampleVertexCount = 0;
    std::size_t sampleTriangleCount = 0;
    std::size_t boundarySampleCount = 0;
    std::size_t weldedInternalSeamVertexCount = 0;
    std::size_t normalDiscontinuityVertexCount = 0;
    double maximumSamplingChordError = 0.0;
    double maximumSamplingNormalErrorDegrees = 0.0;
    double maximumBoundaryChordError = 0.0;
    bool samplingGeometricLimitsPassed = false;
    double samplingEdgeLengthP01 = 0.0;
    double samplingEdgeLengthP50 = 0.0;
    double samplingEdgeLengthP99 = 0.0;
    double samplingEdgeLengthP99P01Ratio = 0.0;
    double samplingMinimumAngleP05Degrees = 0.0;
    double samplingMinimumAngleP50Degrees = 0.0;
    double samplingAspectRatioP50 = 0.0;
    double samplingAspectRatioP95 = 0.0;
    double samplingAspectRatioP99 = 0.0;
    double samplingNormalizedQualityP05 = 0.0;
    double samplingNormalizedQualityP50 = 0.0;
    std::size_t samplingZeroAreaTriangleCount = 0;
    std::size_t samplingNonManifoldEdgeCount = 0;
    std::size_t samplingRemeshSplitCount = 0;
    std::size_t samplingRemeshCollapseCount = 0;
    std::size_t samplingRemeshFlipCount = 0;
    std::size_t samplingRemeshRelocationCount = 0;
    std::string referenceMeshFingerprint;
    std::size_t layoutCarrierVertexCount = 0;
    std::size_t layoutCarrierTriangleCount = 0;
    std::size_t layoutCarrierBoundarySampleCount = 0;
    std::size_t layoutCarrierZeroAreaTriangleCount = 0;
    std::size_t layoutCarrierNonManifoldEdgeCount = 0;
    double layoutCarrierMinimumAngleP05Degrees = 0.0;
    double layoutCarrierAspectRatioP95 = 0.0;
    std::string layoutCarrierMeshFingerprint;
    std::vector<RegionLayoutCandidateReport> layoutCandidates;
    std::vector<RegionFitAttemptReport> fitAttempts;
    bool layoutGatePassed = false;
    bool fitGatePassed = false;
    bool bestEffortUsed = false;
    bool forceDiagnosticSewingRequested = false;
    bool forceDiagnosticSewingAttempted = false;
    bool sewableGeometryAvailable = false;
    bool productValidated = false;
    int diagnosticSewingCandidateCount = 0;
    int diagnosticCandidatePatchCount = 0;
    std::string diagnosticSelectionReason;
    std::string firstProductFailureGate;
    std::string cacheKey;
    std::vector<RegionRefitCacheEvent> cacheEvents;
    int retrimReplacementFaceCount = 0;
    bool retrimAttempted = false;
    bool retrimValidationPassed = false;
    int retrimInternalSeamCount = 0;
    int retrimReusedBoundaryEdgeCount = 0;
    int retrimSplitBoundarySegmentCount = 0;
    int retrimPcurveProjectionCount = 0;
    int retrimSameParameterCheckCount = 0;
    int retrimSameParameterFailureCount = 0;
    double retrimMaximumSameParameterDeviation = 0.0;
    std::vector<RegionEdgeConsistencyReport> edgeConsistency;
    bool assemblyPassed = false;
    bool repairPassed = false;
    bool sewingAttempted = false;
    bool sewingConstructedShape = false;
    bool diagnosticPipelineCompleted = false;
    std::string continuationStopReason;
    double selectedSewingTolerance = 0.0;
    int sewingAttemptCount = 0;
    int freeEdgesBeforeRepair = 0;
    int freeEdgesAfterRepair = 0;
    int multipleEdgesBeforeRepair = 0;
    int multipleEdgesAfterRepair = 0;
    bool strictTopologyPassed = false;
    std::string strictTopologyFailureReason;
    bool afterBRepCheckValid = false;
    bool stepExportOk = false;
    bool stepRoundtripOk = false;
    bool roundtripBRepCheckValid = false;
    int beforeSolidCount = 0;
    int afterSolidCount = 0;
    int roundtripSolidCount = 0;
    int beforeFaceCount = 0;
    int afterFaceCount = 0;
    int roundtripFaceCount = 0;
    int beforeFreeEdgeCount = 0;
    int afterFreeEdgeCount = 0;
    int roundtripFreeEdgeCount = 0;
    int beforeMultipleEdgeCount = 0;
    int afterMultipleEdgeCount = 0;
    int roundtripMultipleEdgeCount = 0;
    double totalWallMilliseconds = 0.0;
    std::uint64_t peakWorkingSetBytes = 0;
    std::uint64_t peakPrivateBytes = 0;
    std::vector<RegionRefitStageReport> stages;
};

struct RegionRefitResult {
    RegionRefitReport report;
    RegionTopologyConstraints constraints;
    std::shared_ptr<RegionReferenceMesh> referenceMesh;
    std::shared_ptr<RegionReferenceMesh> layoutCarrierMesh;
    std::shared_ptr<RegionPatchLayout> selectedLayout;
    std::vector<std::shared_ptr<RegionPatchLayout>> generatedLayouts;
    std::shared_ptr<RegionFittedPatchNetwork> fittedNetwork;
    TopoDS_Shape fittedPatchNetwork;
    TopoDS_Shape diagnosticSewnShape;
    TopoDS_Shape finalShape;

    bool canApply() const;
};

}
