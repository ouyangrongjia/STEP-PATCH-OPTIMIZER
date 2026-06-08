#include "external/geomagic/GeomagicAutoSurfaceResult.h"
#include "patch/PatchArtifactLocator.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    assert(stream);
    stream << "fixture";
}

void remove_temp_root(const std::filesystem::path& path) {
    std::filesystem::remove_all(path);
}

std::string path_to_string(const std::filesystem::path& path) {
    const auto utf8Path = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8Path.c_str()), utf8Path.size()};
}

void test_locates_plain_crop_step() {
    const auto root = temp_root("spo_patch_artifact_plain");
    const auto stl = root / "data" / "crop_stl" / "a" / "candidate_0001.stl";
    const auto step = root / "data" / "crop_stp" / "a" / "candidate_0001.stp";
    touch(stl);
    touch(step);

    const auto result = spo::PatchArtifactLocator().locateFromLocalStl(stl);

    assert(result.success);
    assert(result.foundStep);
    assert(result.patchStepPath == step);
    assert(result.localStlPath == stl);

    remove_temp_root(root);
}

void test_locates_mechanical_suffix() {
    const auto root = temp_root("spo_patch_artifact_mechanical");
    const auto stl = root / "data" / "crop_stl" / "a" / "candidate_0002.stl";
    const auto step = root / "data" / "crop_stp" / "a" / "candidate_0002_mechanical.stp";
    touch(stl);
    touch(step);

    const auto result = spo::PatchArtifactLocator().locateFromLocalStl(stl);

    assert(result.success);
    assert(result.patchStepPath == step);

    remove_temp_root(root);
}

void test_prefers_mechanical_over_organic() {
    const auto root = temp_root("spo_patch_artifact_strategy_priority");
    const auto stl = root / "data" / "crop_stl" / "a" / "candidate_0003.stl";
    const auto organic = root / "data" / "crop_stp" / "a" / "candidate_0003_organic.stp";
    const auto mechanical = root / "data" / "crop_stp" / "a" / "candidate_0003_mechanical.stp";
    touch(stl);
    touch(organic);
    touch(mechanical);

    const auto result = spo::PatchArtifactLocator().locateFromLocalStl(stl);

    assert(result.success);
    assert(result.patchStepPath == mechanical);

    remove_temp_root(root);
}

void test_uses_wildcard_fallback() {
    const auto root = temp_root("spo_patch_artifact_wildcard");
    const auto stl = root / "data" / "crop_stl" / "a" / "candidate_0004.stl";
    const auto custom = root / "data" / "crop_stp" / "a" / "candidate_0004_custom.stp";
    touch(stl);
    touch(custom);

    const auto result = spo::PatchArtifactLocator().locateFromLocalStl(stl);

    assert(result.success);
    assert(result.patchStepPath == custom);

    remove_temp_root(root);
}

void test_detects_sidecar_and_fit_log() {
    const auto root = temp_root("spo_patch_artifact_sidecar");
    const auto stl = root / "data" / "crop_stl" / "a" / "candidate_0005.stl";
    const auto step = root / "data" / "crop_stp" / "a" / "candidate_0005_mechanical.stp";
    const auto sidecar = root / "data" / "crop_stp" / "a" / "candidate_0005_mechanical_autosurface.igs";
    const auto log = root / "data" / "crop_stp" / "a" / "candidate_0005_mechanical_fit_region.log";
    touch(stl);
    touch(step);
    touch(sidecar);
    touch(log);

    const auto result = spo::PatchArtifactLocator().locateFromLocalStl(stl);

    assert(result.success);
    assert(result.foundIgesSidecar);
    assert(result.foundFitLog);
    assert(result.patchIgesSidecarPath == sidecar);
    assert(result.fitRegionLogPath == log);

    remove_temp_root(root);
}

void test_locates_non_ascii_crop_step_and_sidecars() {
    const auto root = temp_root("spo_patch_artifact_non_ascii");
    const auto stl = root / "data" / "crop_stl" / L"零件" / L"零件_candidate_0008.stl";
    const auto step = root / "data" / "crop_stp" / L"零件" / L"零件_candidate_0008.stp";
    const auto sidecar = root / "data" / "crop_stp" / L"零件" / L"零件_candidate_0008_autosurface.igs";
    const auto log = root / "data" / "crop_stp" / L"零件" / L"零件_candidate_0008_fit_region.log";
    touch(stl);
    touch(step);
    touch(sidecar);
    touch(log);

    const auto result = spo::PatchArtifactLocator().locateFromLocalStl(stl);

    assert(result.success);
    assert(result.foundStep);
    assert(result.foundIgesSidecar);
    assert(result.foundFitLog);
    assert(result.patchStepPath == step);
    assert(result.patchIgesSidecarPath == sidecar);
    assert(result.fitRegionLogPath == log);

    remove_temp_root(root);
}

void test_missing_patch_fails_with_message() {
    const auto root = temp_root("spo_patch_artifact_missing");
    const auto stl = root / "data" / "crop_stl" / "a" / "candidate_0006.stl";
    touch(stl);

    const auto result = spo::PatchArtifactLocator().locateFromLocalStl(stl);

    assert(!result.success);
    assert(!result.message.empty());

    remove_temp_root(root);
}

void test_locates_from_geomagic_result() {
    const auto root = temp_root("spo_patch_artifact_result");
    const auto stl = root / "data" / "crop_stl" / "a" / "candidate_0007.stl";
    const auto step = root / "data" / "crop_stp" / "a" / "candidate_0007.stp";
    const auto sidecar = root / "data" / "crop_stp" / "a" / "candidate_0007_autosurface.igs";
    const auto log = root / "data" / "crop_stp" / "a" / "candidate_0007_fit_region.log";
    touch(stl);
    touch(step);
    touch(sidecar);
    touch(log);

    spo::GeomagicAutoSurfaceResult geomagicResult;
    geomagicResult.inputStlPath = stl;
    geomagicResult.outputStepPath = step;

    const auto result = spo::PatchArtifactLocator().locateFromResult(geomagicResult);

    assert(result.success);
    assert(result.localStlPath == stl);
    assert(result.patchStepPath == step);
    assert(result.patchIgesSidecarPath == sidecar);
    assert(result.fitRegionLogPath == log);

    remove_temp_root(root);
}

void test_optional_real_fixture_is_discovery_based() {
    const auto root = repo_root();
    const auto stl = root / "data" / "crop_stl" / "local_candidate_0179.stl";
    if (!std::filesystem::exists(stl)) {
        std::cerr << "PatchArtifactLocator real fixture skipped: missing " << path_to_string(stl) << "\n";
        return;
    }

    const auto result = spo::PatchArtifactLocator().locateFromLocalStl(stl);
    if (!result.success) {
        std::cerr << "PatchArtifactLocator real fixture skipped: " << result.message << "\n";
        return;
    }

    assert(result.foundStep);
    assert(result.patchStepPath.filename().string().find("local_candidate_0179") != std::string::npos);
}

}

void run_patch_artifact_locator_tests() {
    test_locates_plain_crop_step();
    test_locates_mechanical_suffix();
    test_prefers_mechanical_over_organic();
    test_uses_wildcard_fallback();
    test_detects_sidecar_and_fit_log();
    test_locates_non_ascii_crop_step_and_sidecars();
    test_missing_patch_fails_with_message();
    test_locates_from_geomagic_result();
    test_optional_real_fixture_is_discovery_based();
}
