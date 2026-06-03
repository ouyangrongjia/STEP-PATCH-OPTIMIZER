#include "patch/PatchApplyState.h"

namespace spo {

const char* toString(RegionPatchStatus status) {
    switch (status) {
    case RegionPatchStatus::NotGenerated:
        return "NotGenerated";
    case RegionPatchStatus::Generated:
        return "Generated";
    case RegionPatchStatus::PreviewReady:
        return "PreviewReady";
    case RegionPatchStatus::PreviewHighRisk:
        return "PreviewHighRisk";
    case RegionPatchStatus::ApplyBlocked:
        return "ApplyBlocked";
    case RegionPatchStatus::ApplyPending:
        return "ApplyPending";
    case RegionPatchStatus::Applied:
        return "Applied";
    case RegionPatchStatus::ApplyFailed:
        return "ApplyFailed";
    }
    return "Unknown";
}

PatchApplyDecision evaluatePatchApplyReadiness(
    bool patchPreviewReady,
    const PatchPreviewReport& report) {
    PatchApplyDecision decision;

    if (!patchPreviewReady) {
        decision.status = RegionPatchStatus::NotGenerated;
        decision.reason = "No patch preview is ready.";
        return decision;
    }

    if (!report.success) {
        decision.status = RegionPatchStatus::ApplyBlocked;
        decision.reason = "Patch preview report is not successful.";
        return decision;
    }

    if (report.highRisk) {
        decision.status = RegionPatchStatus::PreviewHighRisk;
        decision.reason = report.warningMessage.empty()
            ? std::string("Patch preview is high risk.")
            : report.warningMessage;
        return decision;
    }

    if (!report.patchBboxValid) {
        decision.status = RegionPatchStatus::ApplyBlocked;
        decision.reason = "Patch bounding box is invalid.";
        return decision;
    }

    if (!report.patchBRepCheckValid) {
        decision.status = RegionPatchStatus::ApplyBlocked;
        decision.reason = "Imported patch failed BRepCheck.";
        return decision;
    }

    if (report.patchFaceCount <= 0) {
        decision.status = RegionPatchStatus::ApplyBlocked;
        decision.reason = "Patch has no faces.";
        return decision;
    }

    decision.canRequestApply = true;
    decision.status = RegionPatchStatus::PreviewReady;
    decision.message = "Patch preview is ready for T6 replacement.";
    return decision;
}

}
