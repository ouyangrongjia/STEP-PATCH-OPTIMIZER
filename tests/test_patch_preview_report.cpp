#include "app/AppController.h"
#include "brep/ShapeDocument.h"
#include "io/StepWriter.h"
#include "patch/PatchImportService.h"
#include "patch/PatchPreviewReport.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <cassert>
#include <filesystem>
#include <string>
#include <type_traits>

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

TopoDS_Shape make_test_box(double dx = 10.0, double dy = 20.0, double dz = 30.0) {
    return BRepPrimAPI_MakeBox(dx, dy, dz).Shape();
}

void write_step_file(const std::filesystem::path& path) {
    const spo::ShapeDocument document(make_test_box(), {});
    const auto result = spo::StepWriter().write(document, path);
    assert(result.success());
}

spo::PatchArtifactPaths artifact_paths(const std::filesystem::path& patchPath) {
    spo::PatchArtifactPaths paths;
    paths.success = true;
    paths.patchStepPath = patchPath;
    paths.foundStep = true;
    return paths;
}

void test_report_from_valid_imported_patch() {
    const auto root = temp_root("spo_patch_preview_valid");
    const auto path = root / "patch.stp";
    write_step_file(path);
    const auto imported = spo::PatchImportService().importPatch(path);
    assert(imported.success);

    spo::PatchPreviewReportInput input;
    input.importedPatch = &imported;
    input.artifactPaths = artifact_paths(path);

    const auto report = spo::buildPatchPreviewReport(input);

    assert(report.success);
    assert(report.patchFaceCount == imported.faceCount);
    assert(report.patchEdgeCount == imported.edgeCount);
    assert(report.patchBboxValid);
    assert(report.patchBRepCheckValid);
    assert(report.patchStepPath == path);

    remove_temp_root(root);
}

void test_multi_face_patch_warns_without_blocking_preview() {
    const auto root = temp_root("spo_patch_preview_multiface");
    const auto path = root / "patch.stp";
    write_step_file(path);
    const auto imported = spo::PatchImportService().importPatch(path);
    assert(imported.success);
    assert(imported.faceCount > 1);

    spo::PatchPreviewReportInput input;
    input.importedPatch = &imported;
    input.artifactPaths = artifact_paths(path);

    const auto report = spo::buildPatchPreviewReport(input);

    assert(report.success);
    assert(report.patchFaceCount > 1);
    assert(!report.highRisk);
    assert(!report.warningMessage.empty());

    remove_temp_root(root);
}

void test_bbox_deviation_marks_high_risk() {
    spo::ImportedPatchInfo imported;
    imported.success = true;
    imported.brepCheckValid = true;
    imported.faceCount = 1;
    imported.edgeCount = 4;
    imported.bboxValid = true;
    imported.bboxMinX = 100.0;
    imported.bboxMinY = 100.0;
    imported.bboxMinZ = 100.0;
    imported.bboxMaxX = 110.0;
    imported.bboxMaxY = 110.0;
    imported.bboxMaxZ = 110.0;

    spo::PatchPreviewReportInput input;
    input.importedPatch = &imported;
    input.candidateId = 42;
    input.sourceFaceCount = 2;
    input.sourceBoundaryEdgeCount = 4;
    input.candidateBboxValid = true;
    input.candidateBBoxMinX = 0.0;
    input.candidateBBoxMinY = 0.0;
    input.candidateBBoxMinZ = 0.0;
    input.candidateBBoxMaxX = 10.0;
    input.candidateBBoxMaxY = 10.0;
    input.candidateBBoxMaxZ = 10.0;

    const auto report = spo::buildPatchPreviewReport(input);

    assert(report.success);
    assert(report.highRisk);
    assert(report.bboxCenterDistance > 0.0);
    assert(!report.warningMessage.empty());
}

void test_patch_only_report_recommends_selecting_candidate() {
    spo::ImportedPatchInfo imported;
    imported.success = true;
    imported.brepCheckValid = true;
    imported.faceCount = 1;
    imported.edgeCount = 4;
    imported.bboxValid = true;
    imported.bboxMinX = 0.0;
    imported.bboxMinY = 0.0;
    imported.bboxMinZ = 0.0;
    imported.bboxMaxX = 1.0;
    imported.bboxMaxY = 1.0;
    imported.bboxMaxZ = 1.0;

    spo::PatchPreviewReportInput input;
    input.importedPatch = &imported;

    const auto report = spo::buildPatchPreviewReport(input);

    assert(report.success);
    assert(!report.candidateBboxValid);
    assert(report.patchBboxValid);
    assert(!report.recommendedAction.empty());
}

void test_iges_artifact_is_not_reported_as_step_path() {
    const auto igesPath = std::filesystem::path("patch.igs");
    spo::ImportedPatchInfo imported;
    imported.success = true;
    imported.sourcePath = igesPath;
    imported.brepCheckValid = true;
    imported.faceCount = 1;
    imported.edgeCount = 4;
    imported.bboxValid = true;

    spo::PatchArtifactPaths artifacts;
    artifacts.success = true;
    artifacts.patchIgesSidecarPath = igesPath;
    artifacts.foundIgesSidecar = true;

    spo::PatchPreviewReportInput input;
    input.importedPatch = &imported;
    input.artifactPaths = artifacts;

    const auto report = spo::buildPatchPreviewReport(input);

    assert(report.success);
    assert(report.patchStepPath.empty());
    assert(report.patchIgesSidecarPath == igesPath);
}

void test_failed_import_report_is_high_risk() {
    spo::ImportedPatchInfo imported;
    imported.success = false;
    imported.errorMessage = "import failed";

    spo::PatchPreviewReportInput input;
    input.importedPatch = &imported;

    const auto report = spo::buildPatchPreviewReport(input);

    assert(!report.success);
    assert(report.highRisk);
    assert(!report.message.empty());
    assert(!report.recommendedAction.empty());
}

void test_report_builder_does_not_accept_mutable_document() {
    static_assert(!std::is_invocable_v<
        decltype(static_cast<spo::PatchPreviewReport (*)(const spo::ShapeDocument*, const spo::MergeCandidate*, const spo::ImportedPatchInfo&, const spo::PatchArtifactPaths&)>(
            &spo::buildPatchPreviewReport)),
        spo::ShapeDocument&,
        const spo::MergeCandidate*,
        const spo::ImportedPatchInfo&,
        const spo::PatchArtifactPaths&>);
}

void test_app_controller_imports_patch_file_and_clears_preview_state() {
    const auto root = temp_root("spo_patch_preview_controller");
    const auto path = root / "patch.stp";
    write_step_file(path);

    spo::AppController controller;
    const auto import = controller.importPatchFromFileForCurrentCandidate(path);

    assert(import.success());
    assert(controller.patchPreviewReady());
    assert(controller.currentImportedPatchInfo().success);
    assert(controller.currentPatchPreviewReport().success);
    assert(controller.currentPatchPreviewReport().patchStepPath == path);

    controller.clearCurrentPatchOverlay();

    assert(!controller.patchPreviewReady());

    remove_temp_root(root);
}

}

void run_patch_preview_report_tests() {
    test_report_from_valid_imported_patch();
    test_multi_face_patch_warns_without_blocking_preview();
    test_bbox_deviation_marks_high_risk();
    test_patch_only_report_recommends_selecting_candidate();
    test_iges_artifact_is_not_reported_as_step_path();
    test_failed_import_report_is_high_risk();
    test_report_builder_does_not_accept_mutable_document();
    test_app_controller_imports_patch_file_and_clears_preview_state();
}
