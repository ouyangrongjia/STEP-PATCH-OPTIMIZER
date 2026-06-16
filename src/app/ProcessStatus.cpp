#include "app/ProcessStatus.h"

#include <utility>

namespace spo {

const char* toString(ProcessStage stage) {
    switch (stage) {
    case ProcessStage::Idle:
        return "Idle";
    case ProcessStage::LoadingStep:
        return "LoadingStep";
    case ProcessStage::ExportingStep:
        return "ExportingStep";
    case ProcessStage::PreviewingMergeCandidates:
        return "PreviewingMergeCandidates";
    case ProcessStage::AnalyzingBoundary:
        return "AnalyzingBoundary";
    case ProcessStage::CroppingStl:
        return "CroppingStl";
    case ProcessStage::RunningGeomagic:
        return "RunningGeomagic";
    case ProcessStage::ImportingPatch:
        return "ImportingPatch";
    case ProcessStage::PreviewReady:
        return "PreviewReady";
    case ProcessStage::ApplyingPatch:
        return "ApplyingPatch";
    case ProcessStage::BuildingReplacement:
        return "BuildingReplacement";
    case ProcessStage::Repairing:
        return "Repairing";
    case ProcessStage::AdaptiveSewing:
        return "AdaptiveSewing";
    case ProcessStage::ValidatingGate:
        return "ValidatingGate";
    case ProcessStage::Applied:
        return "Applied";
    case ProcessStage::ApplyFailed:
        return "ApplyFailed";
    case ProcessStage::CachedUndo:
        return "CachedUndo";
    case ProcessStage::CachedRedo:
        return "CachedRedo";
    }
    return "Unknown";
}

ProcessStatusSnapshot makeProcessStatus(ProcessStage stage, std::string message) {
    ProcessStatusSnapshot snapshot;
    snapshot.stage = stage;
    snapshot.latestMessage = std::move(message);
    return snapshot;
}

}
