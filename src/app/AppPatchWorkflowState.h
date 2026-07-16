#pragma once

#include "app/ProcessStatus.h"
#include "patch/ImportedPatchInfo.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchApplyState.h"
#include "patch/PatchPreviewReport.h"

#include <string>

namespace spo {

struct AppPatchWorkflowState {
    PatchArtifactPaths artifactPaths;
    ImportedPatchInfo importedPatch;
    PatchPreviewReport previewReport;
    bool previewReady = false;
    RegionPatchStatus status = RegionPatchStatus::NotGenerated;
    std::string statusMessage;
    ProcessStatusSnapshot processStatus = makeProcessStatus(ProcessStage::Idle, "Idle.");

    void clear();
    PatchApplyDecision applyDecision() const;
    void updateApplyState();
};

}
