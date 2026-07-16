#include "app/AppPatchWorkflowState.h"
#include "app/PatchWorkflowText.h"

#include <cassert>

namespace {

void test_patch_workflow_state_clear_preserves_public_defaults() {
    spo::AppPatchWorkflowState state;
    state.previewReady = true;
    state.status = spo::RegionPatchStatus::PreviewReady;
    state.statusMessage = "ready";
    state.processStatus = spo::makeProcessStatus(
        spo::ProcessStage::PreviewReady,
        "ready");

    state.clear();

    assert(!state.previewReady);
    assert(state.status == spo::RegionPatchStatus::NotGenerated);
    assert(state.statusMessage == "Patch preview cleared.");
    assert(state.processStatus.stage == spo::ProcessStage::Idle);
    assert(state.processStatus.latestMessage == "Patch preview cleared.");
}

void test_patch_progress_text_keeps_existing_field_order() {
    auto status = spo::makeProcessStatus(
        spo::ProcessStage::RunningGeomagic,
        "Geomagic AutoSurface started.");
    status.candidateId = 7;
    status.sourceFaceCount = 2;
    status.boundaryEdgeCount = 4;
    status.latestWarning = "diagnostic warning";

    const auto line = spo::patch_workflow_text::progressLine(status, 65000);
    assert(line.startsWith("[1:05]  |  RunningGeomagic"));
    assert(line.contains("candidate=7  |  faces=2  |  boundary_edges=4"));
    assert(line.endsWith("warning=diagnostic warning"));

    const QStringList events{line};
    const auto report = spo::patch_workflow_text::progressReport(
        "Patch preview",
        status,
        events,
        65000);
    assert(report.startsWith(
        "Patch preview\n心跳：running  |  elapsed 1:05  |  last stage RunningGeomagic"));
    assert(report.contains("最近消息：Geomagic AutoSurface started."));
    assert(report.contains("\n阶段事件\n[1:05]"));

    const auto statusText = spo::patch_workflow_text::statusText(
        "Patch Apply",
        status,
        65000);
    assert(statusText.startsWith("Patch Apply | RunningGeomagic | elapsed 1:05"));
    assert(statusText.endsWith("Geomagic AutoSurface started."));
}

}

void run_patch_workflow_structure_tests() {
    test_patch_workflow_state_clear_preserves_public_defaults();
    test_patch_progress_text_keeps_existing_field_order();
}
