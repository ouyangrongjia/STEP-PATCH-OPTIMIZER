#pragma once

#include "patch/PatchPreviewReport.h"

#include <string>

namespace spo {

enum class RegionPatchStatus {
    NotGenerated,
    Generated,
    PreviewReady,
    PreviewHighRisk,
    ApplyBlocked,
    ApplyPending,
    Applied,
    ApplyFailed
};

const char* toString(RegionPatchStatus status);

struct PatchApplyDecision {
    bool canRequestApply = false;
    RegionPatchStatus status = RegionPatchStatus::NotGenerated;
    std::string message;
    std::string reason;
};

PatchApplyDecision evaluatePatchApplyReadiness(
    bool patchPreviewReady,
    const PatchPreviewReport& report);

}
