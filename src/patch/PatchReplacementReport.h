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

struct PatchReplacementReport {
    bool success = false;
    bool rollbackApplied = false;
    bool usedMultiFacePatch = false;

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

    PatchReplacementFailureReason failureReason = PatchReplacementFailureReason::None;

    std::string message;
    std::string warningMessage;
};

PatchReplacementReport validatePatchReplacementInput(const PatchReplacementInput& input);

}
