#pragma once

#include "patch/PatchReplacementInput.h"

#include <string>

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

    double selectedSewingTolerance = 0.0;
    int sewingAttemptCount = 0;
    int bestSewingFreeEdges = 0;
    int bestSewingMultipleEdges = 0;
    int bestSewingFaceCount = 0;
    int bestSewingEdgeCount = 0;
    int bestSewingShellCount = 0;
    int bestSewingSolidCount = 0;
    bool bestSewingBRepCheckValid = false;
    bool bestSewingCollapsed = false;

    std::string message;
    std::string warningMessage;
};

struct PatchReplacementReport {
    bool success = false;
    bool rollbackApplied = false;
    bool usedMultiFacePatch = false;
    bool sourceFacesReplaced = false;

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
