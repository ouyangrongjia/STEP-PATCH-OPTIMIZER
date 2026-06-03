#pragma once

#include "patch/ImportedPatchInfo.h"
#include "patch/PatchArtifactLocator.h"

#include <filesystem>
#include <string>

namespace spo {

class ShapeDocument;
struct MergeCandidate;

struct PatchPreviewReport {
    bool success = false;
    bool highRisk = false;

    int candidateId = -1;
    int sourceFaceCount = 0;
    int sourceBoundaryEdgeCount = 0;

    std::filesystem::path localStlPath;
    std::filesystem::path patchStepPath;
    std::filesystem::path patchIgesSidecarPath;
    std::filesystem::path fitRegionLogPath;

    int patchFaceCount = 0;
    int patchEdgeCount = 0;
    int patchSolidCount = 0;
    int patchShellCount = 0;

    bool patchBRepCheckValid = false;

    bool candidateBboxValid = false;
    bool patchBboxValid = false;

    double candidateBBoxMinX = 0.0;
    double candidateBBoxMinY = 0.0;
    double candidateBBoxMinZ = 0.0;
    double candidateBBoxMaxX = 0.0;
    double candidateBBoxMaxY = 0.0;
    double candidateBBoxMaxZ = 0.0;

    double patchBBoxMinX = 0.0;
    double patchBBoxMinY = 0.0;
    double patchBBoxMinZ = 0.0;
    double patchBBoxMaxX = 0.0;
    double patchBBoxMaxY = 0.0;
    double patchBBoxMaxZ = 0.0;

    double bboxCenterDistance = 0.0;
    double bboxDiagonalRatio = 0.0;

    std::string message;
    std::string warningMessage;
    std::string recommendedAction;
};

struct PatchPreviewReportInput {
    const ImportedPatchInfo* importedPatch = nullptr;
    PatchArtifactPaths artifactPaths;

    int candidateId = -1;
    int sourceFaceCount = 0;
    int sourceBoundaryEdgeCount = 0;

    bool candidateBboxValid = false;
    double candidateBBoxMinX = 0.0;
    double candidateBBoxMinY = 0.0;
    double candidateBBoxMinZ = 0.0;
    double candidateBBoxMaxX = 0.0;
    double candidateBBoxMaxY = 0.0;
    double candidateBBoxMaxZ = 0.0;
};

PatchPreviewReport buildPatchPreviewReport(const PatchPreviewReportInput& input);

PatchPreviewReport buildPatchPreviewReport(
    const ShapeDocument* document,
    const MergeCandidate* candidate,
    const ImportedPatchInfo& importedPatch,
    const PatchArtifactPaths& artifactPaths);

}
