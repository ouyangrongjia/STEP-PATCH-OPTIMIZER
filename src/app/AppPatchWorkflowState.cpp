#include "app/AppPatchWorkflowState.h"

namespace spo {

void AppPatchWorkflowState::clear() {
    artifactPaths = {};
    importedPatch = {};
    previewReport = {};
    previewReady = false;
    status = RegionPatchStatus::NotGenerated;
    statusMessage = "Patch preview cleared.";
    processStatus = makeProcessStatus(ProcessStage::Idle, statusMessage);
}

PatchApplyDecision AppPatchWorkflowState::applyDecision() const {
    return evaluatePatchApplyReadiness(previewReady, previewReport);
}

void AppPatchWorkflowState::updateApplyState() {
    const auto decision = applyDecision();
    status = decision.status;
    statusMessage = decision.canRequestApply ? decision.message : decision.reason;
}

}
