#pragma once

#include "common/GeometryTypes.h"
#include "patch/PatchReplacementInput.h"
#include "patch/PatchTrimDiagnostics.h"

#include <string>
#include <vector>

namespace spo {

enum class PatchReplacementFailureReason {
    None,
    MissingDocument,
    MissingCandidate,
    MissingBoundary,
    MissingImportedPatch,
    MissingPreviewReport,
    InvalidBoundary,
    PreviewNotReady,
    PreviewHighRisk,
    ImportFailed,
    EmptyPatchShape,
    InvalidPatchBRep,
    InvalidPatchBBox,
    NoPatchFaces,
    UnsupportedCandidate,
    BuildFailed,
    GateFailed
};

const char* toString(PatchReplacementFailureReason reason);

struct PatchReplacementRepairOptions {
    bool runSameParameter = true;
    bool runShapeFixShape = true;
    bool runShapeFixFace = true;
    bool runShapeFixWire = true;
    bool runSewing = true;
    bool runUnifySameDomain = true;
    bool runAdaptiveSewing = true;
    bool runShellToSolid = true;
    double sewingTolerance = 1.0e-4;
    double preferredSewingTolerance = 0.007;
    double minSewingTolerance = 0.001;
    double maxSewingTolerance = 0.1;
    double collapseFaceRatio = 0.5;
    bool keepInternalPatchSeams = true;
};

struct PatchReplacementRepairReport {
    bool success = false;
    bool sameParameterApplied = false;
    bool shapeFixShapeApplied = false;
    bool shapeFixFaceApplied = false;
    bool shapeFixWireApplied = false;
    bool unifySameDomainApplied = false;
    bool sewingApplied = false;
    bool adaptiveSewingApplied = false;
    bool shellToSolidApplied = false;

    int faceCountBeforeRepair = 0;
    int edgeCountBeforeRepair = 0;
    int shellCountBeforeRepair = 0;
    int solidCountBeforeRepair = 0;
    int faceCountAfterRepair = 0;
    int edgeCountAfterRepair = 0;
    int shellCountAfterRepair = 0;
    int solidCountAfterRepair = 0;

    int freeEdgesBeforeRepair = 0;
    int freeEdgesAfterRepair = 0;
    int multipleEdgesBeforeRepair = 0;
    int multipleEdgesAfterRepair = 0;
    int degeneratedFreeEdgesBeforeRepair = 0;
    int degeneratedFreeEdgesAfterRepair = 0;

    double selectedSewingTolerance = 0.0;
    int sewingAttemptCount = 0;
    int bestSewingFreeEdges = 0;
    int bestSewingMultipleEdges = 0;
    int bestSewingDegeneratedFreeEdgeCount = 0;
    int bestSewingFaceCount = 0;
    int bestSewingEdgeCount = 0;
    int bestSewingShellCount = 0;
    int bestSewingSolidCount = 0;
    bool bestSewingBRepCheckValid = false;
    bool bestSewingCollapsed = false;

    std::string message;
    std::string warningMessage;
};

struct PatchReplacementFreeEdgeDiagnostic {
    bool afterRepair = false;
    bool appearedAfterRepair = false;
    int edgeIndex = -1;
    int adjacentFaceCount = 0;
    double edgeLength = 0.0;
    double edgeTolerance = 0.0;
    bool degenerated = false;

    bool midpointValid = false;
    double midpointX = 0.0;
    double midpointY = 0.0;
    double midpointZ = 0.0;
    bool startPointValid = false;
    double startX = 0.0;
    double startY = 0.0;
    double startZ = 0.0;
    bool endPointValid = false;
    double endX = 0.0;
    double endY = 0.0;
    double endZ = 0.0;

    int nearestOriginalBoundaryEdgeId = -1;
    double nearestOriginalBoundaryEdgeDistance = 0.0;
    double nearestOriginalBoundaryEdgeLength = 0.0;
    double nearestOriginalBoundaryEdgeTolerance = 0.0;
    bool nearestOriginalBoundaryStartPointValid = false;
    double nearestOriginalBoundaryStartX = 0.0;
    double nearestOriginalBoundaryStartY = 0.0;
    double nearestOriginalBoundaryStartZ = 0.0;
    bool nearestOriginalBoundaryMidpointValid = false;
    double nearestOriginalBoundaryMidpointX = 0.0;
    double nearestOriginalBoundaryMidpointY = 0.0;
    double nearestOriginalBoundaryMidpointZ = 0.0;
    bool nearestOriginalBoundaryEndPointValid = false;
    double nearestOriginalBoundaryEndX = 0.0;
    double nearestOriginalBoundaryEndY = 0.0;
    double nearestOriginalBoundaryEndZ = 0.0;
    bool nearestOriginalBoundaryParameterRangeValid = false;
    double nearestOriginalBoundaryFirstParameter = 0.0;
    double nearestOriginalBoundaryLastParameter = 0.0;
    int nearestOriginalBoundaryAdjacentFaceCount = 0;
    int nearestOriginalBoundaryPcurveAvailableFaceCount = 0;
    std::vector<int> nearestOriginalBoundaryAdjacentFaceIds;
    std::vector<std::string> nearestOriginalBoundaryAdjacentSurfaceTypes;

    bool matchedSplitBoundarySegment = false;
    double matchedSplitBoundaryFirstParameter = 0.0;
    double matchedSplitBoundaryLastParameter = 0.0;
    int patchFaceOwner = -1;
    int sameOriginalBoundaryEdgeSplitSegmentCount = 0;
    int sameOriginalBoundaryEdgeOwnerSwitchCount = 0;
    int sameOriginalBoundaryEdgeDegeneratedSegmentCount = 0;
    bool nearestSplitBoundarySegment = false;
    int nearestSplitBoundaryOriginalEdgeId = -1;
    double nearestSplitBoundarySegmentDistance = 0.0;
    double nearestSplitBoundaryFirstParameter = 0.0;
    double nearestSplitBoundaryLastParameter = 0.0;
    double nearestSplitBoundaryLength = 0.0;
    double nearestSplitBoundaryTolerance = 0.0;
    int nearestSplitBoundaryPatchFaceOwner = -1;

    int fittedPatchProjectionFaceCount = 0;
    int fittedPatchProjectionSampleCount = 0;
    int fittedPatchProjectionFailedCount = 0;
    double fittedPatchProjectionMinDistance = 0.0;
    double fittedPatchProjectionMaxDistance = 0.0;
    double fittedPatchProjectionAverageDistance = 0.0;
    int nearestFittedPatchFaceIndex = -1;
};

struct PatchExternalCadDiagnosticsReport {
    bool captured = false;

    bool rawPatchPreflightAvailable = false;
    bool rawPatchPreflightExecuted = false;
    std::string rawPatchPreflightInputPath;
    std::string rawPatchPreflightRole = "PatchPreflightOnly";
    std::string rawPatchPreflightStatus = "Skipped";
    std::string rawPatchPreflightMessage;

    std::string finalAppliedStepDiagnosticStage = "AppliedStepAfterSuccessfulApply";
    bool finalAppliedStepDiagnosticEligible = false;
    bool finalAppliedStepDiagnosticExecuted = false;
    std::string finalAppliedStepDiagnosticInputPath;
    std::string finalAppliedStepDiagnosticStatus = "Skipped";
    std::string finalAppliedStepDiagnosticSkippedReason;
    std::string finalAppliedStepDiagnosticMessage;
};

struct PatchReplacementReport {
    bool success = false;
    bool rollbackApplied = false;
    bool usedMultiFacePatch = false;
    bool sourceFacesReplaced = false;
    bool usedOriginalBoundarySurfaceRetrim = false;
    bool attemptedMultiSurfaceBoundaryShell = false;
    bool usedMultiSurfaceBoundaryShell = false;

    bool repairApplied = false;
    bool sameParameterApplied = false;
    bool shapeFixApplied = false;
    bool shapeFixShapeApplied = false;
    bool shapeFixFaceApplied = false;
    bool shapeFixWireApplied = false;
    bool unifySameDomainApplied = false;
    bool sewingApplied = false;
    bool adaptiveSewingApplied = false;
    bool shellToSolidApplied = false;
    int repairRunCount = 0;

    int candidateId = -1;
    int sourceFaceCount = 0;
    int sourceBoundaryEdgeCount = 0;

    int patchFaceCount = 0;
    int patchEdgeCount = 0;
    int patchShellCount = 0;
    int patchSolidCount = 0;

    int replacementFaceCount = 0;
    int replacementEdgeCount = 0;
    int replacementShellCount = 0;
    int replacementSolidCount = 0;

    int retrimSelectedPatchFaceIndex = -1;
    int retrimBoundarySampleCount = 0;
    int retrimProjectedSampleCount = 0;
    int retrimFailedProjectionCount = 0;
    double retrimMaxProjectionDistance = 0.0;
    double retrimAverageProjectionDistance = 0.0;
    int retrimSurfaceCoverageProjectedSampleCount = 0;
    int retrimSurfaceCoverageFailedProjectionCount = 0;
    double retrimSurfaceCoverageMaxProjectionDistance = 0.0;
    double retrimSurfaceCoverageAverageProjectionDistance = 0.0;
    std::vector<EdgeId> retrimSurfaceCoverageUncoveredEdgeIds;
    int retrimBoundaryEdgePcurveRebuildAttemptCount = 0;
    int retrimBoundaryEdgePcurveRebuildSuccessCount = 0;
    int retrimBoundaryEdgePcurveRebuildFailureCount = 0;
    int retrimBoundaryEdgeSameParameterCheckCount = 0;
    int retrimBoundaryEdgeSameParameterFailureCount = 0;
    double retrimBoundaryEdgeMaxSameParameterDeviation = 0.0;
    std::vector<EdgeId> retrimBoundaryEdgePcurveRebuildFailedEdgeIds;
    std::vector<EdgeId> retrimBoundaryEdgeSameParameterFailedEdgeIds;

    int multiSurfaceBoundarySampleCount = 0;
    int multiSurfaceProjectedSampleCount = 0;
    int multiSurfaceFailedProjectionCount = 0;
    double multiSurfaceMaxProjectionDistance = 0.0;
    double multiSurfaceAverageProjectionDistance = 0.0;
    int multiSurfaceAssignedBoundarySegmentCount = 0;
    int multiSurfaceSplitBoundaryEdgeCount = 0;
    int multiSurfaceBuiltFaceCount = 0;
    int multiSurfaceClosedWireCount = 0;
    int multiSurfaceOpenWireCount = 0;
    int multiSurfaceMultipleClosedWireFaceCount = 0;
    int multiSurfaceSkippedUnownedOpenWireFaceCount = 0;
    int multiSurfaceFailedPatchFaceIndex = -1;
    int multiSurfaceFailedFaceEdgeCount = 0;
    int multiSurfaceFailedFaceOriginalBoundarySegmentCount = 0;
    int multiSurfaceFailedFaceInternalEdgeCount = 0;
    int multiSurfaceFailedOpenWireEdgeCount = 0;
    double multiSurfaceFailedOpenWireLength = 0.0;
    double multiSurfaceFailedOpenWireEndpointGap = 0.0;
    bool multiSurfaceFailedOpenWireStartPointValid = false;
    double multiSurfaceFailedOpenWireStartX = 0.0;
    double multiSurfaceFailedOpenWireStartY = 0.0;
    double multiSurfaceFailedOpenWireStartZ = 0.0;
    bool multiSurfaceFailedOpenWireEndPointValid = false;
    double multiSurfaceFailedOpenWireEndX = 0.0;
    double multiSurfaceFailedOpenWireEndY = 0.0;
    double multiSurfaceFailedOpenWireEndZ = 0.0;
    double multiSurfaceSelectedWireConnectTolerance = 0.0;
    bool multiSurfaceFallbackWireConnectAttempted = false;
    bool multiSurfaceFallbackWireConnectSucceeded = false;
    int multiSurfaceBoundaryEdgePcurveRebuildAttemptCount = 0;
    int multiSurfaceBoundaryEdgePcurveRebuildSuccessCount = 0;
    int multiSurfaceBoundaryEdgePcurveRebuildFailureCount = 0;
    int multiSurfaceBoundaryEdgeSameParameterCheckCount = 0;
    int multiSurfaceBoundaryEdgeSameParameterFailureCount = 0;
    double multiSurfaceBoundaryEdgeMaxSameParameterDeviation = 0.0;
    std::vector<EdgeId> multiSurfaceFailedEdgeIds;
    std::vector<EdgeId> multiSurfaceFailedFaceOriginalBoundaryEdgeIds;
    std::vector<EdgeId> multiSurfaceBoundaryEdgePcurveRebuildFailedEdgeIds;
    std::vector<EdgeId> multiSurfaceBoundaryEdgeSameParameterFailedEdgeIds;

    int faceCountBeforeRepair = 0;
    int edgeCountBeforeRepair = 0;
    int shellCountBeforeRepair = 0;
    int solidCountBeforeRepair = 0;
    int faceCountAfterRepair = 0;
    int edgeCountAfterRepair = 0;
    int shellCountAfterRepair = 0;
    int solidCountAfterRepair = 0;

    int freeEdgesBeforeRepair = 0;
    int freeEdgesAfterRepair = 0;
    int multipleEdgesBeforeRepair = 0;
    int multipleEdgesAfterRepair = 0;
    int degeneratedFreeEdgesBeforeRepair = 0;
    int degeneratedFreeEdgesAfterRepair = 0;

    bool preRepairClosureCaptured = false;
    int preRepairFaceCount = 0;
    int preRepairEdgeCount = 0;
    int preRepairShellCount = 0;
    int preRepairSolidCount = 0;
    bool preRepairBRepCheckValid = false;
    int preRepairFreeEdgeCount = 0;
    int preRepairMultipleEdgeCount = 0;
    int preRepairDegeneratedFreeEdgeCount = 0;

    bool postRepairClosureCaptured = false;
    int postRepairFaceCount = 0;
    int postRepairEdgeCount = 0;
    int postRepairShellCount = 0;
    int postRepairSolidCount = 0;
    bool postRepairBRepCheckValid = false;
    int postRepairFreeEdgeCount = 0;
    int postRepairMultipleEdgeCount = 0;
    int postRepairDegeneratedFreeEdgeCount = 0;
    int appearedAfterRepairDegeneratedFreeEdgeCount = 0;
    std::vector<PatchReplacementFreeEdgeDiagnostic> freeEdgeDiagnostics;
    PatchTrimDiagnosticsReport trimDiagnostics;
    PatchExternalCadDiagnosticsReport externalCadDiagnostics;

    bool gateEvaluated = false;
    bool gatePassed = false;
    bool gateBeforeBRepCheckValid = false;
    bool gateAfterBRepCheckValid = false;
    bool gateRoundtripBRepCheckValid = false;
    bool gateStepExportOk = false;
    bool gateStepRoundtripOk = false;
    bool gateWatertightSolidRequired = false;
    bool gateRoundtripWatertightRequired = false;

    int gateBeforeFaceCount = 0;
    int gateBeforeEdgeCount = 0;
    int gateBeforeShellCount = 0;
    int gateBeforeSolidCount = 0;
    int gateAfterFaceCount = 0;
    int gateAfterEdgeCount = 0;
    int gateAfterShellCount = 0;
    int gateAfterSolidCount = 0;
    int gateRoundtripFaceCount = 0;
    int gateRoundtripEdgeCount = 0;
    int gateRoundtripShellCount = 0;
    int gateRoundtripSolidCount = 0;
    int gateBeforeFreeEdges = 0;
    int gateAfterFreeEdges = 0;
    int gateRoundtripFreeEdges = 0;
    int gateBeforeMultipleEdges = 0;
    int gateAfterMultipleEdges = 0;
    int gateRoundtripMultipleEdges = 0;

    double selectedSewingTolerance = 0.0;
    int sewingAttemptCount = 0;
    int bestSewingFreeEdges = 0;
    int bestSewingMultipleEdges = 0;
    int bestSewingDegeneratedFreeEdgeCount = 0;
    int bestSewingFaceCount = 0;
    int bestSewingEdgeCount = 0;
    int bestSewingShellCount = 0;
    int bestSewingSolidCount = 0;
    bool bestSewingBRepCheckValid = false;
    bool bestSewingCollapsed = false;

    PatchReplacementFailureReason failureReason = PatchReplacementFailureReason::None;

    std::string gateFailureReason;
    std::string gateMessage;
    std::string gateWarningMessage;
    std::string message;
    std::string warningMessage;
    std::string repairWarningMessage;
};

PatchReplacementReport validatePatchReplacementInput(const PatchReplacementInput& input);

}
