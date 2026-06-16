#pragma once

#include <filesystem>
#include <string>

namespace spo {

enum class ProcessStage {
    Idle,
    LoadingStep,
    ExportingStep,
    PreviewingMergeCandidates,
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
    std::filesystem::path patchPreviewRunLogPath;

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
    bool gateBeforeBRepCheckValid = false;
    bool gateAfterBRepCheckValid = false;
    bool gateRoundtripBRepCheckValid = false;
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
