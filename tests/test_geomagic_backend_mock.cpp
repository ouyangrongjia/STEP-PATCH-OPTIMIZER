#include "external/geomagic/GeomagicAutoSurfaceBackend.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

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

QString path_to_qstring(const std::filesystem::path& path) {
    const auto utf8Path = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8Path.c_str()), static_cast<qsizetype>(utf8Path.size()));
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

QJsonObject read_json_object(const std::filesystem::path& path) {
    QFile file(path_to_qstring(path));
    assert(file.open(QIODevice::ReadOnly));
    const auto document = QJsonDocument::fromJson(file.readAll());
    assert(document.isObject());
    return document.object();
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

std::string success_cmd_body() {
    return
        "@echo off\n"
        "echo mock stdout\n"
        "echo FIT_REGION_ADAPTIVE_FIT=%FIT_REGION_ADAPTIVE_FIT%\n"
        "type nul > \"%FIT_REGION_OUTPUT%\"\n"
        "type nul > \"%FIT_REGION_OUTPUT_IGES%\"\n"
        "> \"%FIT_REGION_RESULT_JSON%\" echo {\"success\":true,\"timed_out\":false,\"exit_code\":0,\"bodies\":1,\"open_loops\":0,\"message\":\"mock success\"}\n"
        "exit /b 0\n";
}

std::string failure_cmd_body() {
    return
        "@echo off\n"
        "echo mock failure 1>&2\n"
        "> \"%FIT_REGION_RESULT_JSON%\" echo {\"success\":false,\"timed_out\":false,\"exit_code\":2,\"error_message\":\"mock failure\"}\n"
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
        "> \"%FIT_REGION_RESULT_JSON%\" echo {\"success\":true,\"timed_out\":false,\"exit_code\":0,\"message\":\"mock success without output\"}\n"
        "exit /b 0\n";
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
    const auto mock = write_mock_cmd(root, "mock_success.cmd", success_cmd_body());
    auto config = make_config(root, mock);

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert(result.success);
    assert(result.exitCode == 0);
    assert(std::filesystem::exists(config.outputStepPath));
    assert(std::filesystem::exists(config.outputIgesPath));
    assert(std::filesystem::exists(config.resultJsonPath));
    assert(std::filesystem::exists(config.stdoutLogPath));
    assert(std::filesystem::exists(config.stderrLogPath));
    assert(result.bodies == 1);
    assert(result.openLoops == 0);
    assert(result.message.find("mock success") != std::string::npos);

    remove_temp_root(root);
}

void test_mock_failure() {
    const auto root = temp_root("spo_geomagic_backend_failure");
    const auto mock = write_mock_cmd(root, "mock_failure.cmd", failure_cmd_body());
    auto config = make_config(root, mock);

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert(!result.success);
    assert(result.exitCode != 0);
    assert(!result.errorMessage.empty() || !result.message.empty());
    assert(std::filesystem::exists(config.stderrLogPath));
    assert(read_text_file(config.stderrLogPath).find("mock failure") != std::string::npos);

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

void test_auto_resolves_crop_paths_and_writes_config_json() {
    const auto root = temp_root("spo_geomagic_backend_autoresolve");
    {
        const ScopedCurrentPath cwd(root);
        const auto mock = write_mock_cmd(root, "mock_success.cmd", success_cmd_body());
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
        const auto expectedConfigJson = std::filesystem::path("data") / "crop_stp" / "model" / "candidate_0001_autosurface_config.json";

        assert(result.success);
        assert(result.outputStepPath.lexically_normal() == expectedStep);
        assert(result.outputIgesPath.lexically_normal() == expectedIges);
        assert(result.configJsonPath.lexically_normal() == expectedConfigJson);
        assert(std::filesystem::exists(expectedStep));
        assert(std::filesystem::exists(expectedIges));
        assert(std::filesystem::exists(expectedConfigJson));

        const auto object = read_json_object(expectedConfigJson);
        assert(object.value("output_step_path").toString() == path_to_qstring(expectedStep));
        assert(object.value("output_iges_path").toString() == path_to_qstring(expectedIges));
    }

    remove_temp_root(root);
}

void test_auto_resolves_chinese_crop_path() {
    const auto root = temp_root("spo_geomagic_backend_chinese");
    {
        const ScopedCurrentPath cwd(root);
        const auto mock = write_mock_cmd(root, "mock_success.cmd", success_cmd_body());
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

void test_auto_merge_forces_adaptive_fit_environment_to_zero() {
    const auto root = temp_root("spo_geomagic_backend_adaptive_fit");
    const auto mock = write_mock_cmd(root, "mock_success.cmd", success_cmd_body());
    auto config = make_config(root, mock);
    config.autoMerge = true;
    config.adaptiveFit = true;

    const auto result = spo::GeomagicAutoSurfaceBackend().run(config);

    assert(result.success);
    const auto stdoutText = read_text_file(config.stdoutLogPath);
    assert(stdoutText.find("FIT_REGION_ADAPTIVE_FIT=0") != std::string::npos);
    assert(result.message.find("autoMerge=True forces adaptiveFit=False") != std::string::npos);

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
    test_auto_resolves_crop_paths_and_writes_config_json();
    test_auto_resolves_chinese_crop_path();
    test_non_crop_input_without_explicit_outputs_fails_before_process();
    test_auto_merge_forces_adaptive_fit_environment_to_zero();
#endif
}
