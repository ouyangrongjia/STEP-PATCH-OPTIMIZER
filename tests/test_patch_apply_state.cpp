#include "app/AppController.h"
#include "brep/ShapeDocument.h"
#include "command/Command.h"
#include "command/CommandContext.h"
#include "io/StepWriter.h"
#include "patch/PatchApplyState.h"
#include "patch/PatchPreviewReport.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <cassert>
#include <filesystem>
#include <memory>
#include <string>

namespace {

std::filesystem::path temp_root(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void remove_temp_root(const std::filesystem::path& path) {
    std::filesystem::remove_all(path);
}

TopoDS_Shape make_test_box() {
    return BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
}

class NoopUndoableCommand final : public spo::Command {
public:
    const char* name() const override {
        return "NoopUndoableCommand";
    }

    spo::Result execute(spo::CommandContext&) override {
        return spo::Result::ok();
    }

    bool undoable() const override {
        return true;
    }

    spo::Result undo(spo::CommandContext&) override {
        return spo::Result::ok();
    }

    spo::Result redo(spo::CommandContext&) override {
        return spo::Result::ok();
    }
};

void write_step_file(const std::filesystem::path& path) {
    const spo::ShapeDocument document(make_test_box(), {});
    const auto result = spo::StepWriter().write(document, path);
    assert(result.success());
    assert(std::filesystem::exists(path));
}

spo::PatchPreviewReport valid_report(int faceCount = 1) {
    spo::PatchPreviewReport report;
    report.success = true;
    report.highRisk = false;
    report.patchBRepCheckValid = true;
    report.patchBboxValid = true;
    report.patchFaceCount = faceCount;
    return report;
}

void test_no_preview_is_not_generated() {
    const auto decision = spo::evaluatePatchApplyReadiness(false, valid_report());

    assert(!decision.canRequestApply);
    assert(decision.status == spo::RegionPatchStatus::NotGenerated);
    assert(decision.reason == "No patch preview is ready.");
}

void test_failed_report_blocks_apply() {
    auto report = valid_report();
    report.success = false;

    const auto decision = spo::evaluatePatchApplyReadiness(true, report);

    assert(!decision.canRequestApply);
    assert(decision.status == spo::RegionPatchStatus::ApplyBlocked);
    assert(decision.reason == "Patch preview report is not successful.");
}

void test_high_risk_report_blocks_apply() {
    auto report = valid_report();
    report.highRisk = true;
    report.warningMessage = "bbox deviation";

    const auto decision = spo::evaluatePatchApplyReadiness(true, report);

    assert(!decision.canRequestApply);
    assert(decision.status == spo::RegionPatchStatus::PreviewHighRisk);
    assert(decision.reason.find("bbox deviation") != std::string::npos);
}

void test_invalid_bbox_blocks_apply() {
    auto report = valid_report();
    report.patchBboxValid = false;

    const auto decision = spo::evaluatePatchApplyReadiness(true, report);

    assert(!decision.canRequestApply);
    assert(decision.status == spo::RegionPatchStatus::ApplyBlocked);
    assert(decision.reason == "Patch bounding box is invalid.");
}

void test_invalid_brep_check_blocks_apply() {
    auto report = valid_report();
    report.patchBRepCheckValid = false;

    const auto decision = spo::evaluatePatchApplyReadiness(true, report);

    assert(!decision.canRequestApply);
    assert(decision.status == spo::RegionPatchStatus::ApplyBlocked);
    assert(decision.reason == "Imported patch failed BRepCheck.");
}

void test_zero_face_count_blocks_apply() {
    const auto decision = spo::evaluatePatchApplyReadiness(true, valid_report(0));

    assert(!decision.canRequestApply);
    assert(decision.status == spo::RegionPatchStatus::ApplyBlocked);
    assert(decision.reason == "Patch has no faces.");
}

void test_multi_face_valid_patch_is_preview_ready() {
    const auto decision = spo::evaluatePatchApplyReadiness(true, valid_report(12));

    assert(decision.canRequestApply);
    assert(decision.status == spo::RegionPatchStatus::PreviewReady);
    assert(decision.message == "Patch preview is ready for T6 replacement.");
}

void test_status_to_string() {
    assert(std::string(spo::toString(spo::RegionPatchStatus::NotGenerated)) == "NotGenerated");
    assert(std::string(spo::toString(spo::RegionPatchStatus::ApplyPending)) == "ApplyPending");
}

void test_app_controller_clear_resets_status() {
    const auto root = temp_root("spo_patch_apply_controller_clear");
    const auto path = root / "patch.stp";
    write_step_file(path);

    spo::AppController controller;
    const auto import = controller.importPatchFromFileForCurrentCandidate(path);

    assert(import.success());
    assert(controller.patchPreviewReady());
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::PreviewReady);

    controller.clearCurrentPatchOverlay();

    assert(!controller.patchPreviewReady());
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::NotGenerated);

    remove_temp_root(root);
}

void test_app_controller_request_apply_without_preview() {
    spo::AppController controller;
    const auto result = controller.requestApplyCurrentPatchPreview();

    assert(!result.success());
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::NotGenerated);
    assert(!controller.currentPatchStatusMessage().empty());
}

void test_app_controller_request_apply_with_valid_preview_sets_pending_without_document_mutation() {
    const auto root = temp_root("spo_patch_apply_controller_pending");
    const auto path = root / "patch.stp";
    write_step_file(path);

    spo::AppController controller;
    const auto import = controller.importPatchFromFileForCurrentCandidate(path);
    assert(import.success());

    const auto commandCountBefore = controller.history().executedCommands();
    const auto result = controller.requestApplyCurrentPatchPreview();

    assert(!result.success());
    assert(result.message().find("T6 replacement is not implemented") != std::string::npos);
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::ApplyPending);
    assert(controller.history().executedCommands() == commandCountBefore);

    remove_temp_root(root);
}

void test_app_controller_open_step_clears_patch_preview_state() {
    const auto root = temp_root("spo_patch_apply_controller_open_step_clear");
    const auto path = root / "patch.stp";
    write_step_file(path);

    spo::AppController controller;
    assert(controller.importPatchFromFileForCurrentCandidate(path).success());
    assert(controller.patchPreviewReady());

    assert(controller.openStepFile(path).success());

    assert(!controller.patchPreviewReady());
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::NotGenerated);

    remove_temp_root(root);
}

void test_app_controller_undo_redo_clear_patch_preview_state() {
    const auto root = temp_root("spo_patch_apply_controller_undo_redo_clear");
    const auto path = root / "patch.stp";
    write_step_file(path);

    spo::AppController controller;
    assert(controller.execute(std::make_unique<NoopUndoableCommand>()).success());

    assert(controller.importPatchFromFileForCurrentCandidate(path).success());
    assert(controller.patchPreviewReady());
    assert(controller.undo().success());
    assert(!controller.patchPreviewReady());
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::NotGenerated);

    assert(controller.importPatchFromFileForCurrentCandidate(path).success());
    assert(controller.patchPreviewReady());
    assert(controller.redo().success());
    assert(!controller.patchPreviewReady());
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::NotGenerated);

    remove_temp_root(root);
}

void test_app_controller_same_domain_merge_clears_patch_preview_state() {
    const auto root = temp_root("spo_patch_apply_controller_merge_clear");
    const auto path = root / "model.stp";
    write_step_file(path);

    spo::AppController controller;
    assert(controller.openStepFile(path).success());
    assert(controller.importPatchFromFileForCurrentCandidate(path).success());
    assert(controller.patchPreviewReady());

    const auto result = controller.unifySameDomain(25.0, 0.0, 0.001, false);

    assert(result.document.hasShape());
    assert(!controller.patchPreviewReady());
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::NotGenerated);

    remove_temp_root(root);
}

}

void run_patch_apply_state_tests() {
    test_no_preview_is_not_generated();
    test_failed_report_blocks_apply();
    test_high_risk_report_blocks_apply();
    test_invalid_bbox_blocks_apply();
    test_invalid_brep_check_blocks_apply();
    test_zero_face_count_blocks_apply();
    test_multi_face_valid_patch_is_preview_ready();
    test_status_to_string();
    test_app_controller_clear_resets_status();
    test_app_controller_request_apply_without_preview();
    test_app_controller_request_apply_with_valid_preview_sets_pending_without_document_mutation();
    test_app_controller_open_step_clears_patch_preview_state();
    test_app_controller_undo_redo_clear_patch_preview_state();
    test_app_controller_same_domain_merge_clears_patch_preview_state();
}
