#pragma once

#include "brep/ShapeDocument.h"
#include "common/GeometryTypes.h"
#include "reduction/RestrictedBSplinePatchFitter.h"
#include "reduction/StlHybridParameterization.h"
#include "stl/StlMesh.h"
#include "validate/CommercialCadQualityGate.h"
#include "validate/StrictTopologyGate.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <set>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace spo {

enum class SurfaceReductionOutcome {
    InvalidSelection,
    NoAcceptableReduction,
    Reduced,
    TargetAchieved
};

const char* toString(SurfaceReductionOutcome outcome);

enum class SurfaceReductionMergeMethod {
    OriginalFace,
    ExactSameDomain,
    RestrictedBSpline
};

enum class SurfaceReductionCacheMode {
    Disabled,
    ReadWrite,
    ReadOnlyRequired,
    Refresh
};

const char* toString(SurfaceReductionCacheMode mode);

struct SurfaceReductionExecutionOptions {
    SurfaceReductionCacheMode cacheMode = SurfaceReductionCacheMode::Disabled;
    std::filesystem::path cacheDirectory = "data/cache/surface-reduction/v1";
    std::string sourceFingerprint;
    std::string scanFingerprint;
    std::string algorithmFingerprint;
    int fitWorkerCount = 1;
    int maximumProposalGroupSize = 0;
    bool detailedTiming = false;
};

const char* toString(SurfaceReductionMergeMethod method);

enum class SurfaceReductionFailureStage {
    None,
    Parameterization,
    BoundaryConstraints,
    UvIteration,
    ModelSelection,
    Holdout,
    Jacobian,
    Retrim,
    BoundaryHoldout,
    BrepCheck,
    Seam,
    CommercialCad,
    Topology,
    StepRoundtrip
};

const char* toString(SurfaceReductionFailureStage stage);

struct SurfaceReductionOptions {
    double tolerance3d = 0.03;
    double normalToleranceDegrees = 15.0;
    double smoothSeamToleranceDegrees = 5.0;
    double targetOutputRatio = 0.5;
    int maximumControlPoints = 1024;
    int maximumBoundaryConstraintRefinements = 8;
    int maximumUvIterations = 5;
    int maximumUvBacktrackingSteps = 8;
    double uvConvergenceTolerance = 1.0e-8;
    int beamWidth = 8;
    int maximumPairExpansionsPerState = 16;
    int maximumRestrictedFitEvaluations = 512;
    double scanReferenceWeight = 0.25;
    double maximumScanReferenceDistance = 0.2;
};

struct SurfaceReductionRequest {
    std::vector<FaceId> selectedFaces;
    std::set<EdgeId> lockedEdges;
    SurfaceReductionOptions options;
    std::shared_ptr<const StlMesh> scanReferenceStl;
    ScanReferenceCoverage scanReferenceCoverage = ScanReferenceCoverage::None;
};

struct SurfaceReductionRejectedGroup {
    std::vector<FaceId> faces;
    SurfaceReductionFailureStage stage = SurfaceReductionFailureStage::None;
    std::string reason;
    RestrictedBSplinePatchFitReport fit;
};

struct SurfaceReductionGroupReport {
    std::vector<FaceId> faces;
    SurfaceReductionMergeMethod method = SurfaceReductionMergeMethod::OriginalFace;
    RestrictedBSplinePatchFitReport fit;
};

struct SurfaceReductionStageTiming {
    double topologyPrecomputationMs = 0.0;
    double pairQueueMs = 0.0;
    double exactMergeMs = 0.0;
    double restrictedFitMs = 0.0;
    double seamValidationMs = 0.0;
    double assemblyAndCommercialCadMs = 0.0;
    double strictTopologyAndStepRoundtripMs = 0.0;
    double totalMs = 0.0;
};

struct SurfaceReductionFitProfile {
    std::vector<FaceId> faces;
    bool success = false;
    SurfaceReductionFailureStage failureStage = SurfaceReductionFailureStage::None;
    double totalMs = 0.0;
    int solveCount = 0;
    int boundaryProjectionCount = 0;
    int scanIndexCandidateTriangleCount = 0;
    bool persistentCacheHit = false;
};

struct SurfaceReductionReport {
    SurfaceReductionOutcome outcome = SurfaceReductionOutcome::InvalidSelection;
    int inputFaceCount = 0;
    int outputFaceCount = 0;
    double reductionRatio = 0.0;
    bool targetAchieved = false;
    bool allQualityGatesPassed = false;
    bool selectionConnected = false;
    bool parameterizationValid = false;
    double maxDistance = 0.0;
    double maxNormalAngleDegrees = 0.0;
    double maxSmoothSeamAngleDegrees = 0.0;
    int attemptedMergeCount = 0;
    int acceptedMergeCount = 0;
    int expandedStateCount = 0;
    int generatedStateCount = 0;
    int deduplicatedStateCount = 0;
    int fitCacheHitCount = 0;
    int fitCacheMissCount = 0;
    int persistentFitCacheHitCount = 0;
    int persistentFitCacheMissCount = 0;
    int persistentFitCacheWriteCount = 0;
    bool persistentFitCacheRequiredMiss = false;
    int fitWorkerCount = 1;
    int scanIndexCandidateTriangleCount = 0;
    double fitDurationP50Ms = 0.0;
    double fitDurationP95Ms = 0.0;
    double fitDurationP99Ms = 0.0;
    SurfaceReductionStageTiming timing;
    bool searchBudgetExhausted = false;
    bool stepParameterizationUsed = false;
    ScanReferenceCoverage scanReferenceCoverage = ScanReferenceCoverage::None;
    int scanReferenceSampleCount = 0;
    double maxStepToScanDistance = 0.0;
    std::vector<std::vector<FaceId>> outputGroups;
    std::vector<SurfaceReductionGroupReport> groupReports;
    std::vector<SurfaceReductionRejectedGroup> rejectedGroups;
    std::vector<CommercialCadQualityGateReport> commercialCadReports;
    std::vector<SurfaceReductionFitProfile> fitProfiles;
    StrictTopologyGateReport topologyReport;
    std::string message;
};

struct SurfaceReductionReplacement {
    std::vector<FaceId> sourceFaces;
    TopoDS_Face replacementFace;
};

struct SurfaceReductionResult {
    TopoDS_Shape sourceShape;
    TopoDS_Shape previewShape;
    TopoDS_Shape reducedShape;
    SurfaceReductionReport report;
    std::vector<SurfaceReductionReplacement> replacements;

    bool canApply() const {
        return (report.outcome == SurfaceReductionOutcome::Reduced ||
                report.outcome == SurfaceReductionOutcome::TargetAchieved) &&
            report.allQualityGatesPassed && !reducedShape.IsNull();
    }
};

class SurfaceReductionEngine {
public:
    SurfaceReductionResult reduce(
        const ShapeDocument& document,
        const SurfaceReductionRequest& request,
        const SurfaceReductionExecutionOptions& execution = {}) const;
};

}
