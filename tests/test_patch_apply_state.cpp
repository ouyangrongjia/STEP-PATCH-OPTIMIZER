#include "app/AppController.h"
#include "brep/ShapeDocument.h"
#include "command/Command.h"
#include "command/CommandContext.h"
#include "io/StepWriter.h"
#include "merge/MergeCandidate.h"
#include "patch/PatchApplyState.h"
#include "patch/PatchPreviewReport.h"
#include "patch/PatchReplacementReport.h"
#include "validate/ShapeValidator.h"

#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Compound.hxx>

#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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

void write_shape_step_file(const TopoDS_Shape& shape, const std::filesystem::path& path) {
    const spo::ShapeDocument document(shape, {});
    const auto result = spo::StepWriter().write(document, path);
    assert(result.success());
    assert(std::filesystem::exists(path));
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
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

std::vector<spo::FaceId> first_adjacent_face_pair(const spo::ShapeDocument& document) {
    const auto& topology = document.topology();
    for (spo::EdgeId edgeId = 0; edgeId < static_cast<spo::EdgeId>(topology.edgeCount()); ++edgeId) {
        const auto* adjacency = topology.adjacencyForEdge(edgeId);
        if (adjacency != nullptr && adjacency->faces.size() == 2) {
            return {adjacency->faces[0], adjacency->faces[1]};
        }
    }
    return {0, 1};
}

spo::MergeCandidate make_feature_candidate(
    const spo::ShapeDocument& document,
    std::vector<spo::FaceId> faceIds,
    int candidateId = 23) {
    spo::MergeCandidate candidate;
    candidate.candidate_id = candidateId;
    candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    candidate.status = spo::MergeCandidateStatus::Accepted;
    candidate.faces = std::move(faceIds);
    candidate.face_count = static_cast<int>(candidate.faces.size());

    const std::set<spo::FaceId> candidateFaces(candidate.faces.begin(), candidate.faces.end());
    std::set<spo::EdgeId> boundaryEdges;
    const auto& topology = document.topology();
    for (const auto faceId : candidate.faces) {
        for (const auto edgeId : topology.edgesForFace(faceId)) {
            const auto* adjacency = topology.adjacencyForEdge(edgeId);
            const bool touchesOutside = adjacency == nullptr ||
                adjacency->faces.size() < 2 ||
                std::any_of(adjacency->faces.begin(), adjacency->faces.end(), [&](spo::FaceId adjacentFace) {
                    return candidateFaces.find(adjacentFace) == candidateFaces.end();
                });
            if (touchesOutside) {
                boundaryEdges.insert(edgeId);
            }
        }
    }
    candidate.boundary_edges.assign(boundaryEdges.begin(), boundaryEdges.end());
    candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());
    return candidate;
}

TopoDS_Shape make_candidate_face_compound(
    const spo::ShapeDocument& document,
    const std::vector<spo::FaceId>& faces) {
    if (faces.size() == 1) {
        return document.topology().face(faces.front());
    }

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto faceId : faces) {
        builder.Add(compound, document.topology().face(faceId));
    }
    return compound;
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
    const auto root = temp_root("spo_patch_apply_controller_no_preview");
    const auto modelPath = root / "model.stp";
    write_step_file(modelPath);

    spo::AppController controller;
    assert(controller.openStepFile(modelPath).success());
    const auto candidate = make_feature_candidate(controller.document(), {0});
    const auto beforeStats = controller.document().stats();
    spo::PatchReplacementReport report;
    const auto result = controller.applyCurrentPatchToCurrentCandidate(candidate, &report);

    assert(!result.success());
    assert(report.failureReason == spo::PatchReplacementFailureReason::MissingPreviewReport);
    assert(same_stats(controller.document().stats(), beforeStats));
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::NotGenerated);
    assert(!controller.currentPatchStatusMessage().empty());

    remove_temp_root(root);
}

void test_app_controller_preview_candidate_mismatch_blocks_apply() {
    const auto root = temp_root("spo_patch_apply_controller_mismatch");
    const auto modelPath = root / "model.stp";
    const auto patchPath = root / "patch.stp";
    write_step_file(modelPath);

    spo::AppController controller;
    assert(controller.openStepFile(modelPath).success());
    auto previewCandidate = make_feature_candidate(controller.document(), {0}, 23);
    auto currentCandidate = previewCandidate;
    currentCandidate.candidate_id = 24;
    write_shape_step_file(controller.document().topology().face(0), patchPath);

    const auto import = controller.importPatchFromFileForCurrentCandidate(patchPath, &previewCandidate);
    assert(import.success());
    assert(controller.patchPreviewReady());

    const auto beforeStats = controller.document().stats();
    const auto commandCountBefore = controller.history().executedCommands().size();
    spo::PatchReplacementReport report;
    const auto result = controller.applyCurrentPatchToCurrentCandidate(currentCandidate, &report);

    assert(!result.success());
    assert(result.message() == "Patch preview does not match current candidate.");
    assert(report.failureReason == spo::PatchReplacementFailureReason::UnsupportedCandidate);
    assert(controller.patchPreviewReady());
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::ApplyFailed);
    assert(controller.history().executedCommands().size() == commandCountBefore);
    assert(same_stats(controller.document().stats(), beforeStats));

    remove_temp_root(root);
}

void test_app_controller_valid_preview_applies_through_command_history_and_undo_redo() {
    const auto root = temp_root("spo_patch_apply_controller_success");
    const auto modelPath = root / "model.stp";
    const auto patchPath = root / "patch.stp";
    write_step_file(modelPath);

    spo::AppController controller;
    assert(controller.openStepFile(modelPath).success());
    const auto candidate = make_feature_candidate(controller.document(), {0});
    write_shape_step_file(make_candidate_face_compound(controller.document(), candidate.faces), patchPath);
    assert(controller.importPatchFromFileForCurrentCandidate(patchPath, &candidate).success());
    assert(controller.patchPreviewReady());

    const auto beforeStats = controller.document().stats();
    const auto commandCountBefore = controller.history().executedCommands().size();
    spo::PatchReplacementReport report;
    const auto apply = controller.applyCurrentPatchToCurrentCandidate(candidate, &report);

    assert(apply.success());
    assert(report.success);
    assert(report.sourceFacesReplaced);
    assert(report.repairApplied);
    assert(report.sameParameterApplied);
    assert(report.sewingApplied);
    assert(report.repairRunCount == 1);
    assert(controller.history().executedCommands().size() == commandCountBefore + 1);
    assert(!controller.patchPreviewReady());
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::Applied);

    const auto afterStats = controller.document().stats();
    const auto validation = spo::ShapeValidator().validate(controller.document());
    assert(validation.has_shape);
    assert(validation.brep_check_valid);
    assert(validation.free_edges == 0);
    assert(validation.multiple_edges == 0);
    assert(validation.stats.solids == beforeStats.solids);

    assert(controller.undo().success());
    assert(same_stats(controller.document().stats(), beforeStats));
    assert(controller.redo().success());
    assert(same_stats(controller.document().stats(), afterStats));
    assert(report.repairRunCount == 1);

    remove_temp_root(root);
}

void test_app_controller_failed_apply_keeps_document_and_preview_state() {
    const auto root = temp_root("spo_patch_apply_controller_failed_apply");
    const auto modelPath = root / "model.stp";
    const auto patchPath = root / "patch.stp";
    write_step_file(modelPath);

    spo::AppController controller;
    assert(controller.openStepFile(modelPath).success());
    auto candidate = make_feature_candidate(controller.document(), {0});
    candidate.boundary_edges.clear();
    candidate.boundary_edge_count = 0;
    const auto previewCandidate = make_feature_candidate(controller.document(), {0});
    write_shape_step_file(make_candidate_face_compound(controller.document(), previewCandidate.faces), patchPath);
    assert(controller.importPatchFromFileForCurrentCandidate(patchPath, &previewCandidate).success());
    assert(controller.patchPreviewReady());

    const auto beforeStats = controller.document().stats();
    const auto commandCountBefore = controller.history().executedCommands().size();
    spo::PatchReplacementReport report;
    const auto result = controller.applyCurrentPatchToCurrentCandidate(candidate, &report);

    assert(!result.success());
    assert(report.failureReason == spo::PatchReplacementFailureReason::InvalidBoundary);
    assert(controller.patchPreviewReady());
    assert(controller.currentPatchStatus() == spo::RegionPatchStatus::ApplyFailed);
    assert(controller.history().executedCommands().size() == commandCountBefore);
    assert(same_stats(controller.document().stats(), beforeStats));

    remove_temp_root(root);
}

void test_app_controller_multi_face_patch_enters_apply_path_without_unsupported() {
    const auto root = temp_root("spo_patch_apply_controller_multiface");
    const auto modelPath = root / "model.stp";
    const auto patchPath = root / "patch.stp";
    write_step_file(modelPath);

    spo::AppController controller;
    assert(controller.openStepFile(modelPath).success());
    const auto faces = first_adjacent_face_pair(controller.document());
    const auto candidate = make_feature_candidate(controller.document(), faces);
    write_shape_step_file(make_candidate_face_compound(controller.document(), candidate.faces), patchPath);
    assert(controller.importPatchFromFileForCurrentCandidate(patchPath, &candidate).success());
    assert(controller.patchPreviewReady());
    assert(controller.currentPatchPreviewReport().patchFaceCount > 1);

    const auto beforeStats = controller.document().stats();
    spo::PatchReplacementReport report;
    const auto result = controller.applyCurrentPatchToCurrentCandidate(candidate, &report);

    assert(report.failureReason != spo::PatchReplacementFailureReason::UnsupportedCandidate);
    assert(report.usedMultiFacePatch || report.replacementFaceCount > 1);
    if (result.success()) {
        assert(report.success);
        assert(!controller.patchPreviewReady());
    } else {
        assert(report.failureReason == spo::PatchReplacementFailureReason::GateFailed ||
            report.failureReason == spo::PatchReplacementFailureReason::BuildFailed ||
            report.failureReason == spo::PatchReplacementFailureReason::InvalidBoundary);
        assert(same_stats(controller.document().stats(), beforeStats));
    }

    remove_temp_root(root);
}

void test_no_hard_coded_real_sample_path_in_apply_sources() {
#if defined(SPO_SOURCE_DIR)
    const auto sourceRoot = std::filesystem::path(SPO_SOURCE_DIR);
    const auto appControllerHeader = read_text_file(sourceRoot / "src" / "app" / "AppController.h");
    const auto appControllerSource = read_text_file(sourceRoot / "src" / "app" / "AppController.cpp");
    const auto mainWindowHeader = read_text_file(sourceRoot / "src" / "app" / "MainWindow.h");
    const auto mainWindowSource = read_text_file(sourceRoot / "src" / "app" / "MainWindow.cpp");
    const auto commandHeader = read_text_file(sourceRoot / "src" / "command" / "PatchReplacementCommand.h");
    const auto commandSource = read_text_file(sourceRoot / "src" / "command" / "PatchReplacementCommand.cpp");
    const auto allText = appControllerHeader + appControllerSource + mainWindowHeader + mainWindowSource + commandHeader + commandSource;
    const auto bannedLocalCandidate = std::string("local_candidate_") + "0179";
    const auto bannedMechanical = std::string("local_candidate_") + "0179_" + "mechanical.stp";
    const auto bannedClay = std::string("03_") + "\xE9\x85\x8D\xE4\xBB\xB6" + "_Clay_candidate_" + "0179";

    assert(allText.find(bannedLocalCandidate) == std::string::npos);
    assert(allText.find(bannedMechanical) == std::string::npos);
    assert(allText.find(bannedClay) == std::string::npos);
#endif
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
    test_app_controller_preview_candidate_mismatch_blocks_apply();
    test_app_controller_valid_preview_applies_through_command_history_and_undo_redo();
    test_app_controller_failed_apply_keeps_document_and_preview_state();
    test_app_controller_multi_face_patch_enters_apply_path_without_unsupported();
    test_no_hard_coded_real_sample_path_in_apply_sources();
    test_app_controller_open_step_clears_patch_preview_state();
    test_app_controller_undo_redo_clear_patch_preview_state();
    test_app_controller_same_domain_merge_clears_patch_preview_state();
}
