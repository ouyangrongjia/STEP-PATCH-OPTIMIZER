#pragma once

#include <filesystem>
#include <string>

namespace spo {

enum class ProcessStage {
    Idle,
    AnalyzingBoundary,
    CroppingStl,
    RunningGeomagic,
    ImportingPatch,
    PreviewReady,
    ApplyingPatch,
    BuildingReplacement,
    Repairing,
    AdaptiveSewing,
    ValidatingGate,
    Applied,
    ApplyFailed,
    CachedUndo,
    CachedRedo
};

const char* toString(ProcessStage stage);

struct ProcessStatusSnapshot {
    ProcessStage stage = ProcessStage::Idle;
    int candidateId = -1;
    int sourceFaceCount = 0;
    int boundaryEdgeCount = 0;

    std::filesystem::path localStlPath;
    std::filesystem::path patchStepPath;
    std::filesystem::path patchIgesPath;
    std::filesystem::path fitRegionLogPath;

    double selectedSewingTolerance = 0.0;
    int sewingAttemptIndex = 0;
    int sewingAttemptCount = 0;
    int bestFreeEdges = 0;
    int bestMultipleEdges = 0;
    int bestFaceCount = 0;
    int bestEdgeCount = 0;
    int bestShellCount = 0;
    int bestSolidCount = 0;
    bool bestBRepCheckValid = false;

    int repairRunCount = 0;
    bool adaptiveSewingApplied = false;
    bool repairApplied = false;
    bool gateEvaluated = false;
    bool gatePassed = false;

    bool cropBoundaryBandEvaluated = false;
    bool cropSourceTriangleAuditEvaluated = false;
    int cropBoundaryBandSampleCount = 0;
    int cropBoundaryBandMissingPointCount = 0;
    double cropBoundaryBandMaxDistance = 0.0;
    int cropRejectedNearBoundaryTriangleCount = 0;
    int cropConservativeKeepCandidateCount = 0;

    std::string latestGateFailureReason;
    std::string latestMessage;
    std::string latestWarning;
};

ProcessStatusSnapshot makeProcessStatus(ProcessStage stage, std::string message = {});

}
