#include "external/geomagic/GeomagicAutoSurfaceBackend.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

struct ScopedCurrentPath {
    explicit ScopedCurrentPath(const std::filesystem::path& next) :
        previous(std::filesystem::current_path()) {
        std::filesystem::current_path(next);
    }

    ~ScopedCurrentPath() {
        std::filesystem::current_path(previous);
    }

    std::filesystem::path previous;
};

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

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream);
    stream << text;
}

void touch_file(const std::filesystem::path& path) {
    write_text_file(path, "");
}

std::filesystem::path write_mock_cmd(
    const std::filesystem::path& root,
    const char* name,
    const std::string& body) {
    const auto path = root / name;
    write_text_file(path, body);
    return path;
}

std::filesystem::path make_dummy_script(const std::filesystem::path& root) {
    const auto path = root / "dummy_autosurface_pipeline.py";
    write_text_file(path, "# dummy script for T4.2 backend mock tests\n");
    return path;
}

spo::GeomagicAutoSurfaceConfig make_config(
    const std::filesystem::path& root,
    const std::filesystem::path& mockProgram) {
    spo::GeomagicAutoSurfaceConfig config;
    config.wrapCorePath = mockProgram;
    config.scriptPath = make_dummy_script(root);
    config.inputStlPath = root / "data" / "crop_stl" / "model" / "candidate_0001.stl";
    config.outputStepPath = root / "data" / "crop_stp" / "model" / "candidate_0001.stp";
    config.outputIgesPath = root / "data" / "crop_igs" / "model" / "candidate_0001.igs";
    config.workDir = root / "workspace" / "region_0001";
    config.configJsonPath = config.workDir / "candidate_0001_autosurface_config.json";
    config.resultJsonPath = config.workDir / "candidate_0001_autosurface_result.json";
    config.stdoutLogPath = config.workDir / "candidate_0001_autosurface_stdout.log";
    config.stderrLogPath = config.workDir / "candidate_0001_autosurface_stderr.log";
    config.fitRegionLogPath = config.workDir / "candidate_0001_fit_region.log";
    touch_file(config.inputStlPath);
    return config;
}

std::string success_cmd_body(const std::filesystem::path& root) {
    const auto scriptArg = (root / "script_arg.txt").string();
    const auto legacyEnvMarker = (root / "legacy_env_present.txt").string();
    return
        "@echo off\n"
        "echo mock stdout\n"
        "echo FIT_REGION_STRICT_PATCH_TARGET=%FIT_REGION_STRICT_PATCH_TARGET%\n"
        "echo FIT_REGION_SKIP_REMESH=%FIT_REGION_SKIP_REMESH%\n"
        "echo FIT_REGION_AUTOSURFACE_TARGET=%FIT_REGION_AUTOSURFACE_TARGET%\n"
        "echo FIT_REGION_AUTOSURFACE_TOLERANCE=%FIT_REGION_AUTOSURFACE_TOLERANCE%\n"
        "echo FIT_REGION_DETAIL_LEVEL=%FIT_REGION_DETAIL_LEVEL%\n"
        "echo FIT_REGION_GEOMETRY_MODE=%FIT_REGION_GEOMETRY_MODE%\n"
        "echo FIT_REGION_AUTO_MERGE=%FIT_REGION_AUTO_MERGE%\n"
        "echo FIT_REGION_ADAPTIVE_FIT=%FIT_REGION_ADAPTIVE_FIT%\n"
        "echo FIT_REGION_LOG_FILE=%FIT_REGION_LOG_FILE%\n"
        "echo FIT_REGION_INPUT=%FIT_REGION_INPUT%\n"
        "echo FIT_REGION_OUTPUT=%FIT_REGION_OUTPUT%\n"
        "echo %2 > \"" + scriptArg + "\"\n"
        "if defined FIT_REGION_CONFIG_JSON type nul > \"" + legacyEnvMarker + "\"\n"
        "if defined FIT_REGION_RESULT_JSON type nul > \"" + legacyEnvMarker + "\"\n"
        "if defined FIT_REGION_OUTPUT_IGES type nul > \"" + legacyEnvMarker + "\"\n"
        "if defined FIT_REGION_WORK_DIR type nul > \"" + legacyEnvMarker + "\"\n"
        "type nul > \"%FIT_REGION_OUTPUT%\"\n"
        "exit /b 0\n";
}

std::string failure_cmd_body() {
    return
        "@echo off\n"
        "echo mock failure 1>&2\n"
        "exit /b 2\n";
}

std::string timeout_cmd_body() {
    return
        "@echo off\n"
        "ping 127.0.0.1 -n 5 > nul\n"
        "exit /b 0\n";
}

std::string missing_output_cmd_body() {
    return
        "@echo off\n"
        "echo mock success without output\n"
        "exit /b 0\n";
}

std::string step_without_result_json_cmd_body() {
    return
        "@echo off\n"
        "echo standard fit_region success\n"
        "type nul > \"%FIT_REGION_OUTPUT%\"\n"
        "exit /b 1\n";
}

std::string launch_marker_cmd_body(const std::filesystem::path& markerPath) {
    const auto marker = markerPath.generic_string();
    return
        "@echo off\n"
        "type nul > \"" + marker + "\"\n"
        "exit /b 0\n";
}

void assert_failure_message(const spo::GeomagicAutoSurfaceResult& result) {
    assert(!result.success);
    assert(!result.message.empty() || !result.errorMessage.empty());
}

void test_mock_success() {
    const auto root = temp_root("spo_geomagic_backend_success");
    const auto mock = write_mock_cmd(root, "mock_success.cmd", success_cmd_body(root));
    auto config = make_config(root, mock);

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert(result.success);
    assert(result.exitCode == 0);
    assert(std::filesystem::exists(config.outputStepPath));
    assert(!std::filesystem::exists(config.outputIgesPath));
    assert(!std::filesystem::exists(config.configJsonPath));
    assert(!std::filesystem::exists(config.resultJsonPath));
    assert(!std::filesystem::exists(config.stdoutLogPath));
    assert(!std::filesystem::exists(config.stderrLogPath));
    assert(!std::filesystem::exists(root / "legacy_env_present.txt"));
    assert(std::filesystem::path(read_text_file(root / "script_arg.txt")).is_absolute());
    assert(result.message.find("mock stdout") != std::string::npos);
    assert(result.message.find("FIT_REGION_SKIP_REMESH=1") != std::string::npos);
    assert(result.message.find("FIT_REGION_AUTOSURFACE_TARGET=1") != std::string::npos);
    assert(result.message.find("FIT_REGION_GEOMETRY_MODE=Mechanical") != std::string::npos);

    remove_temp_root(root);
}

void test_mock_failure() {
    const auto root = temp_root("spo_geomagic_backend_failure");
    const auto mock = write_mock_cmd(root, "mock_failure.cmd", failure_cmd_body());
    auto config = make_config(root, mock);

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert(!result.success);
    assert(result.exitCode != 0);
    assert(result.errorMessage.find("mock failure") != std::string::npos);
    assert(!std::filesystem::exists(config.configJsonPath));
    assert(!std::filesystem::exists(config.resultJsonPath));
    assert(!std::filesystem::exists(config.stdoutLogPath));
    assert(!std::filesystem::exists(config.stderrLogPath));

    remove_temp_root(root);
}

void test_timeout() {
    const auto root = temp_root("spo_geomagic_backend_timeout");
    const auto mock = write_mock_cmd(root, "mock_timeout.cmd", timeout_cmd_body());
    auto config = make_config(root, mock);
    config.timeoutSeconds = 1;

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert(!result.success);
    assert(result.timedOut);
    assert(result.durationMs > 0);
    assert(!result.errorMessage.empty());

    remove_temp_root(root);
}

void test_missing_input_stl_does_not_start_process() {
    const auto root = temp_root("spo_geomagic_backend_missing_input");
    const auto marker = root / "launched.txt";
    const auto mock = write_mock_cmd(root, "mock_marker.cmd", launch_marker_cmd_body(marker));
    auto config = make_config(root, mock);
    std::filesystem::remove(config.inputStlPath);

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert_failure_message(result);
    assert(!std::filesystem::exists(marker));

    remove_temp_root(root);
}

void test_missing_script_does_not_start_process() {
    const auto root = temp_root("spo_geomagic_backend_missing_script");
    const auto marker = root / "launched.txt";
    const auto mock = write_mock_cmd(root, "mock_marker.cmd", launch_marker_cmd_body(marker));
    auto config = make_config(root, mock);
    std::filesystem::remove(config.scriptPath);

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert_failure_message(result);
    assert(!std::filesystem::exists(marker));

    remove_temp_root(root);
}

void test_missing_output_step_forces_failure() {
    const auto root = temp_root("spo_geomagic_backend_missing_output");
    const auto mock = write_mock_cmd(root, "mock_missing_output.cmd", missing_output_cmd_body());
    auto config = make_config(root, mock);

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert(!result.success);
    assert(!std::filesystem::exists(config.outputStepPath));
    assert(!result.message.empty() || !result.errorMessage.empty());

    remove_temp_root(root);
}

void test_stale_output_step_is_removed_before_process() {
    const auto root = temp_root("spo_geomagic_backend_stale_output");
    const auto mock = write_mock_cmd(root, "mock_missing_output.cmd", missing_output_cmd_body());
    auto config = make_config(root, mock);
    touch_file(config.outputStepPath);

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert(!result.success);
    assert(!std::filesystem::exists(config.outputStepPath));
    assert(!result.message.empty() || !result.errorMessage.empty());

    remove_temp_root(root);
}

void test_output_step_without_result_json_is_success() {
    const auto root = temp_root("spo_geomagic_backend_step_without_json");
    const auto mock = write_mock_cmd(root, "mock_step_without_json.cmd", step_without_result_json_cmd_body());
    auto config = make_config(root, mock);

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert(result.success);
    assert(result.exitCode == 1);
    assert(std::filesystem::exists(config.outputStepPath));
    assert(!std::filesystem::exists(config.resultJsonPath));
    assert(!std::filesystem::exists(config.outputIgesPath));
    assert(result.errorMessage.empty());

    remove_temp_root(root);
}

void test_auto_resolves_crop_paths_without_writing_sidecar_json() {
    const auto root = temp_root("spo_geomagic_backend_autoresolve");
    {
        const ScopedCurrentPath cwd(root);
        const auto mock = write_mock_cmd(root, "mock_success.cmd", success_cmd_body(root));
        auto config = make_config(root, mock);
        config.inputStlPath = std::filesystem::path("data") / "crop_stl" / "model" / "candidate_0001.stl";
        config.outputStepPath.clear();
        config.outputIgesPath.clear();
        config.workDir.clear();
        config.configJsonPath.clear();
        config.resultJsonPath.clear();
        config.stdoutLogPath.clear();
        config.stderrLogPath.clear();
        config.fitRegionLogPath.clear();
        touch_file(config.inputStlPath);

        const auto result = spo::GeomagicAutoSurfaceBackend().run(config);
        const auto expectedStep = std::filesystem::path("data") / "crop_stp" / "model" / "candidate_0001.stp";
        const auto expectedIges = std::filesystem::path("data") / "crop_igs" / "model" / "candidate_0001.igs";

        assert(result.success);
        assert(result.outputStepPath.lexically_normal() == expectedStep);
        assert(result.outputIgesPath.lexically_normal() == expectedIges);
        assert(std::filesystem::exists(expectedStep));
        assert(!std::filesystem::exists(expectedIges));
        assert(result.configJsonPath.empty());
        assert(result.resultJsonPath.empty());
        assert(result.stdoutLogPath.empty());
        assert(result.stderrLogPath.empty());
    }

    remove_temp_root(root);
}

void test_auto_resolves_chinese_crop_path() {
    const auto root = temp_root("spo_geomagic_backend_chinese");
    {
        const ScopedCurrentPath cwd(root);
        const auto mock = write_mock_cmd(root, "mock_success.cmd", success_cmd_body(root));
        auto config = make_config(root, mock);
        config.inputStlPath = std::filesystem::path("data") / "crop_stl" / L"03_配件_Clay" / "candidate_0007.stl";
        config.outputStepPath.clear();
        config.outputIgesPath.clear();
        config.workDir.clear();
        config.configJsonPath.clear();
        config.resultJsonPath.clear();
        config.stdoutLogPath.clear();
        config.stderrLogPath.clear();
        config.fitRegionLogPath.clear();
        touch_file(config.inputStlPath);

        const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

        assert(result.success);
        assert(result.outputStepPath.generic_wstring().find(L"03_配件_Clay") != std::wstring::npos);
        assert(result.outputIgesPath.generic_wstring().find(L"03_配件_Clay") != std::wstring::npos);
        assert(result.outputStepPath.lexically_normal() ==
            (std::filesystem::path("data") / "crop_stp" / L"03_配件_Clay" / "candidate_0007.stp"));
        assert(result.outputIgesPath.lexically_normal() ==
            (std::filesystem::path("data") / "crop_igs" / L"03_配件_Clay" / "candidate_0007.igs"));
    }

    remove_temp_root(root);
}

void test_non_crop_input_without_explicit_outputs_fails_before_process() {
    const auto root = temp_root("spo_geomagic_backend_non_crop_input");
    {
        const ScopedCurrentPath cwd(root);
        const auto marker = root / "launched.txt";
        const auto mock = write_mock_cmd(root, "mock_marker.cmd", launch_marker_cmd_body(marker));
        auto config = make_config(root, mock);
        config.inputStlPath = std::filesystem::path("data") / "stl" / "model.stl";
        config.outputStepPath.clear();
        config.outputIgesPath.clear();
        touch_file(config.inputStlPath);

        const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

        assert_failure_message(result);
        assert(!std::filesystem::exists(marker));
    }

    remove_temp_root(root);
}

void test_config_fitting_flags_are_passed_to_backend_environment() {
    const auto root = temp_root("spo_geomagic_backend_adaptive_fit");
    const auto mock = write_mock_cmd(root, "mock_success.cmd", success_cmd_body(root));
    auto config = make_config(root, mock);
    config.autoMerge = true;
    config.adaptiveFit = true;
    config.skipRemesh = true;
    config.numPatches = 4;
    config.tolerance = 0.08;
    config.detail = 0.25;

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert(result.success);
    assert(result.message.find("FIT_REGION_SKIP_REMESH=1") != std::string::npos);
    assert(result.message.find("FIT_REGION_AUTOSURFACE_TARGET=4") != std::string::npos);
    assert(result.message.find("FIT_REGION_AUTOSURFACE_TOLERANCE=0.08") != std::string::npos);
    assert(result.message.find("FIT_REGION_DETAIL_LEVEL=0.25") != std::string::npos);
    assert(result.message.find("FIT_REGION_ADAPTIVE_FIT=1") != std::string::npos);
    assert(!std::filesystem::exists(root / "legacy_env_present.txt"));

    remove_temp_root(root);
}

}

void run_geomagic_backend_mock_tests() {
#if defined(_WIN32)
    test_mock_success();
    test_mock_failure();
    test_timeout();
    test_missing_input_stl_does_not_start_process();
    test_missing_script_does_not_start_process();
    test_missing_output_step_forces_failure();
    test_stale_output_step_is_removed_before_process();
    test_output_step_without_result_json_is_success();
    test_auto_resolves_crop_paths_without_writing_sidecar_json();
    test_auto_resolves_chinese_crop_path();
    test_non_crop_input_without_explicit_outputs_fails_before_process();
    test_config_fitting_flags_are_passed_to_backend_environment();
#endif
}
