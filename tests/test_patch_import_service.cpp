#include "brep/ShapeDocument.h"
#include "external/geomagic/GeomagicAutoSurfaceBackend.h"
#include "external/geomagic/GeomagicOutputPathResolver.h"
#include "io/StepWriter.h"
#include "patch/PatchImportService.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <IGESControl_Writer.hxx>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

std::filesystem::path repo_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::filesystem::path temp_root(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void remove_temp_root(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

std::string path_to_occt_string(const std::filesystem::path& path) {
    const auto utf8Path = path.u8string();
    return {reinterpret_cast<const char*>(utf8Path.c_str()), utf8Path.size()};
}

std::string path_to_string(const std::filesystem::path& path) {
    const auto utf8Path = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8Path.c_str()), utf8Path.size()};
}

TopoDS_Shape make_test_box() {
    return BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream);
    stream << text;
}

void write_step_file(const std::filesystem::path& path) {
    const spo::ShapeDocument document(make_test_box(), {});
    const auto result = spo::StepWriter().write(document, path);
    assert(result.success());
    assert(std::filesystem::exists(path));
}

void write_iges_file(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    IGESControl_Writer writer("MM", 0);
    assert(writer.AddShape(make_test_box()));
    writer.ComputeModel();
    const auto occtPath = path_to_occt_string(path);
    assert(writer.Write(occtPath.c_str()));
    assert(std::filesystem::exists(path));
}

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
}

bool has_extension(const std::filesystem::path& path, const std::vector<std::string>& extensions) {
    const auto extension = path.extension().string();
    for (const auto& expected : extensions) {
        if (extension == expected) {
            return true;
        }
    }
    return false;
}

std::vector<std::filesystem::path> find_files(
    const std::filesystem::path& root,
    const std::vector<std::string>& extensions) {
    std::vector<std::filesystem::path> paths;
    if (!std::filesystem::exists(root)) {
        return paths;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && has_extension(entry.path(), extensions)) {
            paths.push_back(entry.path());
        }
    }

    std::sort(paths.begin(), paths.end());
    return paths;
}

std::filesystem::path first_crop_stl() {
    const auto root = repo_root();
    const auto preferred = root / "data" / "crop_stl" / L"03_配件_Clay" / "candidate_0007.stl";
    if (std::filesystem::exists(preferred)) {
        return preferred;
    }

    const auto paths = find_files(root / "data" / "crop_stl", {".stl", ".STL"});
    if (paths.empty()) {
        return {};
    }
    return paths.front();
}

bool real_geomagic_enabled() {
    const auto* enabled = std::getenv("SPO_ENABLE_REAL_GEOMAGIC_TESTS");
    return enabled != nullptr && std::string(enabled) == "1";
}

void test_missing_patch_file_fails() {
    const auto root = temp_root("spo_patch_import_missing");
    const auto result = spo::PatchImportService().importPatch(root / "missing.stp");

    assert(!result.success);
    assert(!result.errorMessage.empty() || !result.message.empty());

    remove_temp_root(root);
}

void test_unsupported_extension_fails() {
    const auto root = temp_root("spo_patch_import_unsupported");
    const auto path = root / "patch.txt";
    write_text_file(path, "not a CAD patch");

    const auto result = spo::PatchImportService().importPatch(path);

    assert(!result.success);
    assert(!result.errorMessage.empty() || !result.message.empty());

    remove_temp_root(root);
}

void test_import_simple_step_patch() {
    const auto root = temp_root("spo_patch_import_step");
    const auto path = root / "patch.stp";
    write_step_file(path);

    const auto result = spo::PatchImportService().importPatch(path);

    assert(result.success);
    assert(result.sourcePath == path);
    assert(!result.shape.IsNull());
    assert(result.faceCount > 0);
    assert(result.edgeCount > 0);
    assert(result.bboxValid);
    assert(result.bboxMinX <= result.bboxMaxX);
    assert(result.bboxMinY <= result.bboxMaxY);
    assert(result.bboxMinZ <= result.bboxMaxZ);
    assert(result.brepCheckValid);

    remove_temp_root(root);
}

void test_import_simple_iges_patch() {
    const auto root = temp_root("spo_patch_import_iges");
    const auto path = root / "patch.igs";
    write_iges_file(path);

    const auto result = spo::PatchImportService().importPatch(path);

    assert(result.success);
    assert(result.sourcePath == path);
    assert(!result.shape.IsNull());
    assert(result.faceCount > 0);
    assert(result.edgeCount > 0);
    assert(result.bboxValid);
    assert(result.brepCheckValid);

    remove_temp_root(root);
}

void test_import_from_result_prefers_step() {
    const auto root = temp_root("spo_patch_import_prefers_step");
    const auto stepPath = root / "patch.stp";
    const auto igesPath = root / "patch.igs";
    write_step_file(stepPath);
    write_iges_file(igesPath);

    spo::GeomagicAutoSurfaceResult geomagicResult;
    geomagicResult.outputStepPath = stepPath;
    geomagicResult.outputIgesPath = igesPath;

    const auto result = spo::PatchImportService().importPatchFromResult(geomagicResult);

    assert(result.success);
    assert(result.sourcePath == stepPath);
    assert(result.attemptedStepPath == stepPath);
    assert(result.attemptedIgesPath.empty());

    remove_temp_root(root);
}

void test_import_from_result_falls_back_to_output_iges() {
    const auto root = temp_root("spo_patch_import_fallback_iges");
    const auto stepPath = root / "missing.stp";
    const auto igesPath = root / "patch.igs";
    write_iges_file(igesPath);

    spo::GeomagicAutoSurfaceResult geomagicResult;
    geomagicResult.outputStepPath = stepPath;
    geomagicResult.outputIgesPath = igesPath;

    const auto result = spo::PatchImportService().importPatchFromResult(geomagicResult);

    assert(result.success);
    assert(result.sourcePath == igesPath);
    assert(result.attemptedStepPath == stepPath);
    assert(result.attemptedIgesPath == igesPath);

    remove_temp_root(root);
}

void test_import_from_result_falls_back_to_preserved_iges() {
    const auto root = temp_root("spo_patch_import_fallback_preserved_iges");
    const auto stepPath = root / "missing.stp";
    const auto outputIgesPath = root / "missing.igs";
    const auto preservedIgesPath = root / "preserved.igs";
    write_iges_file(preservedIgesPath);

    spo::GeomagicAutoSurfaceResult geomagicResult;
    geomagicResult.outputStepPath = stepPath;
    geomagicResult.outputIgesPath = outputIgesPath;
    geomagicResult.preservedIgesPath = preservedIgesPath;

    const auto result = spo::PatchImportService().importPatchFromResult(geomagicResult);

    assert(result.success);
    assert(result.sourcePath == preservedIgesPath);
    assert(result.attemptedStepPath == stepPath);
    assert(result.attemptedIgesPath == preservedIgesPath);

    remove_temp_root(root);
}

void test_service_does_not_accept_or_mutate_shape_document() {
    static_assert(!std::is_invocable_v<
        decltype(&spo::PatchImportService::importPatch),
        const spo::PatchImportService*,
        spo::ShapeDocument&>);

    const auto root = temp_root("spo_patch_import_no_document_mutation");
    const auto path = root / "patch.stp";
    write_step_file(path);

    const spo::ShapeDocument document(make_test_box(), {});
    const auto before = document.stats();

    const auto result = spo::PatchImportService().importPatch(path);

    assert(result.success);
    assert(same_stats(document.stats(), before));

    remove_temp_root(root);
}

void test_optional_real_step_file_import() {
    const auto paths = find_files(repo_root() / "data" / "crop_stp", {".stp", ".step", ".STP", ".STEP"});
    if (paths.empty()) {
        return;
    }

    const auto result = spo::PatchImportService().importPatch(paths.front());

    std::cerr << "PatchImportService real STEP import: " << path_to_string(paths.front()) << "\n";
    assert(result.success);
    assert(result.faceCount > 0);
    assert(result.edgeCount > 0);
    assert(result.bboxValid);
}

void test_optional_real_iges_file_import() {
    const auto paths = find_files(repo_root() / "data" / "crop_igs", {".igs", ".iges", ".IGS", ".IGES"});
    if (paths.empty()) {
        return;
    }

    const auto result = spo::PatchImportService().importPatch(paths.front());

    std::cerr << "PatchImportService real IGES import: " << path_to_string(paths.front()) << "\n";
    assert(result.success);
    assert(result.faceCount > 0);
    assert(result.bboxValid);
}

void test_optional_real_geomagic_chain_import() {
    if (!real_geomagic_enabled()) {
        return;
    }

    const auto root = repo_root();
    const auto wrapCorePath = std::filesystem::path("E:/Geomagic Wrap/wrapCore.exe");
    const auto scriptPath = root / "scripts" / "geomagic_wrap" / "autosurface_pipeline.py";
    const auto inputStlPath = first_crop_stl();

    if (!std::filesystem::exists(wrapCorePath) ||
        !std::filesystem::exists(scriptPath) ||
        inputStlPath.empty()) {
        return;
    }

    const auto paths = spo::resolveGeomagicOutputPathsFromCropStl(
        inputStlPath,
        root / "data" / "crop_stl",
        root / "data" / "crop_stp",
        root / "data" / "crop_igs");
    assert(paths.success);

    const auto workDir = root / "workspace" / "test_patch_import_service_real";

    spo::GeomagicAutoSurfaceConfig config;
    config.inputStlPath = inputStlPath;
    config.outputStepPath = paths.outputStepPath;
    config.outputIgesPath = paths.outputIgesPath;
    config.wrapCorePath = wrapCorePath;
    config.scriptPath = scriptPath;
    config.workDir = workDir;
    config.configJsonPath = workDir / "autosurface_config.json";
    config.resultJsonPath = workDir / "autosurface_result.json";
    config.stdoutLogPath = workDir / "autosurface_stdout.log";
    config.stderrLogPath = workDir / "autosurface_stderr.log";
    config.fitRegionLogPath = workDir / "fit_region.log";
    config.timeoutSeconds = 1800;
    config.strictPatchTarget = true;
    config.skipRemesh = true;

    const auto geomagicResult = spo::GeomagicAutoSurfaceBackend().run(config);
    if (!geomagicResult.success) {
        std::cerr << "Geomagic real patch generation failed.\n"
            << "message: " << geomagicResult.message << "\n"
            << "error: " << geomagicResult.errorMessage << "\n"
            << "result_json: " << path_to_string(geomagicResult.resultJsonPath) << "\n"
            << "fit_region_log: " << path_to_string(geomagicResult.fitRegionLogPath) << "\n";
        assert(false);
    }

    assert(std::filesystem::exists(paths.outputStepPath));
    assert(std::filesystem::exists(paths.outputIgesPath));

    const auto importResult = spo::PatchImportService().importPatchFromResult(geomagicResult);

    std::cerr << "PatchImportService real Geomagic chain STL: " << path_to_string(inputStlPath) << "\n"
        << "PatchImportService real Geomagic chain STEP: " << path_to_string(paths.outputStepPath) << "\n"
        << "PatchImportService real Geomagic chain IGES: " << path_to_string(paths.outputIgesPath) << "\n"
        << "PatchImportService real Geomagic chain result JSON: " << path_to_string(geomagicResult.resultJsonPath) << "\n"
        << "PatchImportService real Geomagic chain fit log: " << path_to_string(geomagicResult.fitRegionLogPath) << "\n";

    assert(importResult.success);
    assert(importResult.faceCount > 0);
    assert(importResult.bboxValid);
}

}

void run_patch_import_service_tests() {
    test_missing_patch_file_fails();
    test_unsupported_extension_fails();
    test_import_simple_step_patch();
    test_import_simple_iges_patch();
    test_import_from_result_prefers_step();
    test_import_from_result_falls_back_to_output_iges();
    test_import_from_result_falls_back_to_preserved_iges();
    test_service_does_not_accept_or_mutate_shape_document();
    test_optional_real_step_file_import();
    test_optional_real_iges_file_import();
    test_optional_real_geomagic_chain_import();
}
