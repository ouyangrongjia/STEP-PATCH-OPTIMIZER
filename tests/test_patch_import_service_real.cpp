#include "external/geomagic/GeomagicAutoSurfaceBackend.h"
#include "external/geomagic/GeomagicOutputPathResolver.h"
#include "patch/PatchImportService.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::filesystem::path repo_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string path_to_string(const std::filesystem::path& path) {
    const auto utf8Path = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8Path.c_str()), utf8Path.size()};
}

std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension;
}

bool has_extension(const std::filesystem::path& path, const std::vector<std::string>& extensions) {
    const auto extension = lowercase_extension(path);
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

bool real_geomagic_enabled() {
    const auto* enabled = std::getenv("SPO_ENABLE_REAL_GEOMAGIC_TESTS");
    return enabled != nullptr && std::string(enabled) == "1";
}

std::filesystem::path find_crop_stl() {
    const auto root = repo_root();
    const auto preferred = root / "data" / "crop_stl" / L"03_配件_Clay" / "candidate_0007.stl";
    if (std::filesystem::exists(preferred)) {
        return preferred;
    }

    const auto paths = find_files(root / "data" / "crop_stl", {".stl"});
    if (paths.empty()) {
        return {};
    }
    return paths.front();
}

std::filesystem::path sidecar_path(const std::filesystem::path& outputStepPath, const char* suffix) {
    auto filename = outputStepPath.stem();
    filename += suffix;
    return outputStepPath.parent_path() / filename;
}

void print_geomagic_failure(
    const spo::GeomagicAutoSurfaceConfig& config,
    const spo::GeomagicAutoSurfaceResult& result) {
    std::cerr
        << "Geomagic real patch generation failed.\n"
        << "message: " << result.message << "\n"
        << "error: " << result.errorMessage << "\n"
        << "fit_region_log: " << path_to_string(result.fitRegionLogPath) << "\n"
        << "stdout_log: " << path_to_string(result.stdoutLogPath) << "\n"
        << "stderr_log: " << path_to_string(result.stderrLogPath) << "\n"
        << "input_stl: " << path_to_string(config.inputStlPath) << "\n"
        << "output_step: " << path_to_string(config.outputStepPath) << "\n";
}

void test_optional_real_patch_files_import() {
    const auto root = repo_root();
    const auto stepPaths = find_files(root / "data" / "crop_stp", {".stp", ".step"});
    const auto igesPaths = find_files(root / "data" / "crop_igs", {".igs", ".iges"});

    if (stepPaths.empty() && igesPaths.empty()) {
        std::cerr << "PatchImportService real patch file import skipped: no data/crop_stp or data/crop_igs files.\n";
        return;
    }

    if (!stepPaths.empty()) {
        const auto result = spo::PatchImportService().importPatch(stepPaths.front());

        std::cerr << "PatchImportService real STEP import: " << path_to_string(stepPaths.front()) << "\n";
        assert(result.success);
        assert(result.faceCount > 0);
        assert(result.edgeCount > 0);
        assert(result.bboxValid);
        assert(!result.shape.IsNull());
    }

    if (!igesPaths.empty()) {
        const auto result = spo::PatchImportService().importPatch(igesPaths.front());

        std::cerr << "PatchImportService real IGES import: " << path_to_string(igesPaths.front()) << "\n";
        assert(result.success);
        assert(result.faceCount > 0);
        assert(result.bboxValid);
        assert(!result.shape.IsNull());
    }
}

void test_real_geomagic_crop_stl_to_imported_patch() {
    if (!real_geomagic_enabled()) {
        std::cerr << "PatchImportService real Geomagic chain skipped: SPO_ENABLE_REAL_GEOMAGIC_TESTS is not 1.\n";
        return;
    }

    const auto root = repo_root();
    const auto wrapCorePath = std::filesystem::path("E:/Geomagic Wrap/wrapCore.exe");
    const auto scriptPath = root / "scripts" / "geomagic_wrap" / "autosurface_pipeline.py";
    const auto inputStlPath = find_crop_stl();

    if (!std::filesystem::exists(wrapCorePath)) {
        std::cerr << "PatchImportService real Geomagic chain skipped: missing " << path_to_string(wrapCorePath) << "\n";
        return;
    }
    if (!std::filesystem::exists(scriptPath)) {
        std::cerr << "PatchImportService real Geomagic chain skipped: missing " << path_to_string(scriptPath) << "\n";
        return;
    }
    if (inputStlPath.empty()) {
        std::cerr << "PatchImportService real Geomagic chain skipped: no STL under data/crop_stl.\n";
        return;
    }

    const auto paths = spo::resolveGeomagicOutputPathsFromCropStl(
        inputStlPath,
        root / "data" / "crop_stl",
        root / "data" / "crop_stp",
        root / "data" / "crop_igs");
    assert(paths.success);

    spo::GeomagicAutoSurfaceConfig config;
    config.wrapCorePath = wrapCorePath;
    config.scriptPath = scriptPath;
    config.inputStlPath = inputStlPath;
    config.outputStepPath = paths.outputStepPath;
    config.outputIgesPath = paths.outputIgesPath;
    config.workDir = paths.outputStepPath.parent_path();
    config.configJsonPath = sidecar_path(paths.outputStepPath, "_autosurface_config.json");
    config.resultJsonPath = sidecar_path(paths.outputStepPath, "_autosurface_result.json");
    config.stdoutLogPath = sidecar_path(paths.outputStepPath, "_autosurface_stdout.log");
    config.stderrLogPath = sidecar_path(paths.outputStepPath, "_autosurface_stderr.log");
    config.fitRegionLogPath = sidecar_path(paths.outputStepPath, "_fit_region.log");
    config.skipRemesh = true;
    config.quickSmooth = false;
    config.relax = false;
    config.strictPatchTarget = true;
    config.timeoutSeconds = 1800;

    const auto geomagicResult = spo::GeomagicAutoSurfaceBackend().run(config);
    if (!geomagicResult.success) {
        print_geomagic_failure(config, geomagicResult);
        assert(false);
    }

    assert(std::filesystem::exists(paths.outputStepPath));
    const auto sidecarIgesPath = sidecar_path(paths.outputStepPath, "_autosurface.igs");

    const auto imported = spo::PatchImportService().importPatchFromResult(geomagicResult);

    std::cerr
        << "PatchImportService real Geomagic chain input STL: " << path_to_string(inputStlPath) << "\n"
        << "PatchImportService real Geomagic chain output STEP: " << path_to_string(paths.outputStepPath) << "\n"
        << "PatchImportService real Geomagic chain sidecar IGES: " << path_to_string(sidecarIgesPath) << "\n"
        << "PatchImportService real Geomagic chain fit log: " << path_to_string(config.fitRegionLogPath) << "\n"
        << "PatchImportService real Geomagic chain stdout log: " << path_to_string(config.stdoutLogPath) << "\n"
        << "PatchImportService real Geomagic chain stderr log: " << path_to_string(config.stderrLogPath) << "\n"
        << "PatchImportService import success: " << imported.success << "\n"
        << "PatchImportService import faceCount: " << imported.faceCount << "\n"
        << "PatchImportService import edgeCount: " << imported.edgeCount << "\n"
        << "PatchImportService import bboxValid: " << imported.bboxValid << "\n"
        << "PatchImportService import brepCheckValid: " << imported.brepCheckValid << "\n"
        << "PatchImportService import bbox: ["
        << imported.bboxMinX << ", " << imported.bboxMinY << ", " << imported.bboxMinZ << "] - ["
        << imported.bboxMaxX << ", " << imported.bboxMaxY << ", " << imported.bboxMaxZ << "]\n";

    assert(imported.success);
    assert(imported.faceCount > 0);
    assert(imported.bboxValid);
    assert(!imported.shape.IsNull());
}

}

void run_patch_import_service_real_tests() {
    test_optional_real_patch_files_import();
    test_real_geomagic_crop_stl_to_imported_patch();
}
