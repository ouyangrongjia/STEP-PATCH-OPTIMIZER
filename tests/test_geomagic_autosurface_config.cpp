#include "external/geomagic/GeomagicAutoSurfaceConfig.h"
#include "external/geomagic/GeomagicAutoSurfaceResult.h"
#include "external/geomagic/GeomagicOutputPathResolver.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr double kTolerance = 1.0e-12;

std::filesystem::path temp_root(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

QString path_to_qstring(const std::filesystem::path& path) {
    const auto utf8Path = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8Path.c_str()), static_cast<qsizetype>(utf8Path.size()));
}

QJsonObject read_json_object(const std::filesystem::path& path) {
    QFile file(path_to_qstring(path));
    assert(file.open(QIODevice::ReadOnly));
    const auto document = QJsonDocument::fromJson(file.readAll());
    assert(document.isObject());
    return document.object();
}

spo::GeomagicAutoSurfaceConfig make_valid_config(const std::filesystem::path& root) {
    spo::GeomagicAutoSurfaceConfig config;
    config.inputStlPath = root / "data" / "crop_stl" / "model" / "candidate_0001.stl";
    config.outputStepPath = root / "data" / "crop_stp" / "model" / "candidate_0001.stp";
    config.outputIgesPath = root / "data" / "crop_igs" / "model" / "candidate_0001.igs";
    config.workDir = root / "workspace" / "region_0001";
    config.configJsonPath = config.workDir / "autosurface_config.json";
    config.resultJsonPath = config.workDir / "autosurface_result.json";
    config.stdoutLogPath = config.workDir / "autosurface_stdout.log";
    config.stderrLogPath = config.workDir / "autosurface_stderr.log";
    config.fitRegionLogPath = config.workDir / "fit_region.log";
    return config;
}

void assert_invalid(const spo::GeomagicAutoSurfaceConfig& config) {
    const auto validation = spo::validateGeomagicAutoSurfaceConfig(config);
    assert(!validation.valid);
    assert(!validation.message.empty());
}

void test_default_config_values() {
    const spo::GeomagicAutoSurfaceConfig config;

    assert(config.wrapCorePath == std::filesystem::path("E:/Geomagic Wrap/wrapCore.exe"));
    assert(config.scriptPath == std::filesystem::path("scripts/geomagic_wrap/autosurface_pipeline.py"));
    assert(config.autoMerge);
    assert(!config.adaptiveFit);
    assert(config.strictPatchTarget);
    assert(config.numPatches == 1);
    assert((config.fallbackNumPatches == std::vector<int>{2, 4, 8}));
    assert(std::fabs(config.detail - 0.10) <= kTolerance);
    assert(std::fabs(config.tolerance - 0.03) <= kTolerance);
    assert(config.geometry == "Organic");
    assert(config.timeoutSeconds == 1800);
    assert(config.keepTemp);
    assert(config.skipRemesh);
}

void test_valid_config_does_not_require_existing_files() {
    const auto root = temp_root("spo_geomagic_config_valid");
    const auto config = make_valid_config(root);

    const auto validation = spo::validateGeomagicAutoSurfaceConfig(config);

    assert(validation.valid);

    std::filesystem::remove_all(root);
}

void test_empty_required_paths_are_invalid() {
    const auto root = temp_root("spo_geomagic_config_empty_paths");

    auto config = make_valid_config(root);
    config.scriptPath.clear();
    assert_invalid(config);

    config = make_valid_config(root);
    config.inputStlPath.clear();
    assert_invalid(config);

    config = make_valid_config(root);
    config.outputStepPath.clear();
    assert_invalid(config);

    config = make_valid_config(root);
    config.outputIgesPath.clear();
    assert_invalid(config);

    config = make_valid_config(root);
    config.workDir.clear();
    assert_invalid(config);

    config = make_valid_config(root);
    config.resultJsonPath.clear();
    assert_invalid(config);

    std::filesystem::remove_all(root);
}

void test_invalid_numeric_params_are_invalid() {
    const auto root = temp_root("spo_geomagic_config_invalid_numeric");

    auto config = make_valid_config(root);
    config.numPatches = 0;
    assert_invalid(config);

    config = make_valid_config(root);
    config.timeoutSeconds = 0;
    assert_invalid(config);

    config = make_valid_config(root);
    config.tolerance = 0.0;
    assert_invalid(config);

    config = make_valid_config(root);
    config.detail = -0.01;
    assert_invalid(config);

    config = make_valid_config(root);
    config.detail = 1.01;
    assert_invalid(config);

    config = make_valid_config(root);
    config.fallbackNumPatches = {2, 0, 8};
    assert_invalid(config);

    std::filesystem::remove_all(root);
}

void test_invalid_geometry_is_invalid() {
    const auto root = temp_root("spo_geomagic_config_invalid_geometry");
    auto config = make_valid_config(root);
    config.geometry = "BadMode";

    assert_invalid(config);

    std::filesystem::remove_all(root);
}

void test_auto_merge_and_adaptive_fit_is_nonfatal() {
    const auto root = temp_root("spo_geomagic_config_automerge_adaptive");
    auto config = make_valid_config(root);
    config.autoMerge = true;
    config.adaptiveFit = true;

    const auto validation = spo::validateGeomagicAutoSurfaceConfig(config);

    assert(validation.valid);
    assert(validation.message.find("adaptiveFit") != std::string::npos);

    std::filesystem::remove_all(root);
}

void test_write_config_json() {
    const auto root = temp_root("spo_geomagic_config_write_json");
    auto config = make_valid_config(root);
    config.geometry = "Mechanical";
    config.numPatches = 4;
    std::string error;

    const auto ok = spo::writeGeomagicAutoSurfaceConfigJson(config, config.configJsonPath, &error);

    assert(ok);
    assert(error.empty());
    assert(std::filesystem::exists(config.configJsonPath));

    const auto object = read_json_object(config.configJsonPath);
    assert(object.value("input_stl_path").toString() == path_to_qstring(config.inputStlPath));
    assert(object.value("output_step_path").toString() == path_to_qstring(config.outputStepPath));
    assert(object.value("output_iges_path").toString() == path_to_qstring(config.outputIgesPath));
    assert(object.value("script_path").toString() == path_to_qstring(config.scriptPath));
    assert(object.value("num_patches").toInt() == 4);
    assert(object.value("geometry").toString() == "Mechanical");

    std::filesystem::remove_all(root);
}

void test_write_and_read_result_json() {
    const auto root = temp_root("spo_geomagic_result_json");
    const auto resultPath = root / "autosurface_result.json";
    spo::GeomagicAutoSurfaceResult result;
    result.success = true;
    result.timedOut = false;
    result.exitCode = 0;
    result.bodies = 1;
    result.openLoops = 2;
    result.message = "ok";
    result.errorMessage = "";
    result.failedStage = "";
    result.inputStlPath = root / "data" / "crop_stl" / L"03_配件_Clay" / "candidate_0007.stl";
    result.outputIgesPath = root / "data" / "crop_igs" / L"03_配件_Clay" / "candidate_0007.igs";
    result.outputStepPath = root / "data" / "crop_stp" / L"03_配件_Clay" / "candidate_0007.stp";
    result.preservedIgesPath = result.outputIgesPath;
    result.configJsonPath = root / "autosurface_config.json";
    result.resultJsonPath = resultPath;
    result.stdoutLogPath = root / "autosurface_stdout.log";
    result.stderrLogPath = root / "autosurface_stderr.log";
    result.fitRegionLogPath = root / "fit_region.log";
    result.durationMs = 1234;
    std::string error;

    assert(spo::writeGeomagicAutoSurfaceResultJson(result, resultPath, &error));
    assert(error.empty());

    const auto loaded = spo::readGeomagicAutoSurfaceResultJson(resultPath, &error);

    assert(error.empty());
    assert(loaded.success == result.success);
    assert(loaded.exitCode == result.exitCode);
    assert(loaded.bodies == result.bodies);
    assert(loaded.openLoops == result.openLoops);
    assert(loaded.inputStlPath == result.inputStlPath);
    assert(loaded.outputIgesPath == result.outputIgesPath);
    assert(loaded.outputStepPath == result.outputStepPath);
    assert(loaded.message == result.message);
    assert(loaded.durationMs == result.durationMs);

    std::filesystem::remove_all(root);
}

void test_resolver_config_integration_writes_resolved_paths() {
    const auto root = temp_root("spo_geomagic_resolver_config_integration");
    const auto cropStlRoot = root / "data" / "crop_stl";
    const auto cropStpRoot = root / "data" / "crop_stp";
    const auto cropIgsRoot = root / "data" / "crop_igs";
    const auto input = cropStlRoot / "model" / "candidate_0001.stl";

    const auto paths = spo::resolveGeomagicOutputPathsFromCropStl(input, cropStlRoot, cropStpRoot, cropIgsRoot);
    assert(paths.success);

    auto config = make_valid_config(root);
    config.inputStlPath = input;
    config.outputStepPath = paths.outputStepPath;
    config.outputIgesPath = paths.outputIgesPath;
    assert(spo::validateGeomagicAutoSurfaceConfig(config).valid);

    std::string error;
    assert(spo::writeGeomagicAutoSurfaceConfigJson(config, config.configJsonPath, &error));
    assert(error.empty());

    const auto object = read_json_object(config.configJsonPath);
    assert(object.value("output_step_path").toString() == path_to_qstring(cropStpRoot / "model" / "candidate_0001.stp"));
    assert(object.value("output_iges_path").toString() == path_to_qstring(cropIgsRoot / "model" / "candidate_0001.igs"));

    std::filesystem::remove_all(root);
}

}

void run_geomagic_autosurface_config_tests() {
    test_default_config_values();
    test_valid_config_does_not_require_existing_files();
    test_empty_required_paths_are_invalid();
    test_invalid_numeric_params_are_invalid();
    test_invalid_geometry_is_invalid();
    test_auto_merge_and_adaptive_fit_is_nonfatal();
    test_write_config_json();
    test_write_and_read_result_json();
    test_resolver_config_integration_writes_resolved_paths();
}
