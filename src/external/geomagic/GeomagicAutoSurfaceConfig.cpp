#include "external/geomagic/GeomagicAutoSurfaceConfig.h"
#include "external/geomagic/GeomagicJsonFile.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace spo {

namespace {

QJsonArray int_array_to_json(const std::vector<int>& values) {
    QJsonArray array;
    for (const auto value : values) {
        array.append(value);
    }
    return array;
}

QJsonObject config_to_json(const GeomagicAutoSurfaceConfig& config) {
    QJsonObject object;
    object.insert("wrap_core_path", geomagic_json::pathToQString(config.wrapCorePath));
    object.insert("script_path", geomagic_json::pathToQString(config.scriptPath));
    object.insert("input_stl_path", geomagic_json::pathToQString(config.inputStlPath));
    object.insert("output_iges_path", geomagic_json::pathToQString(config.outputIgesPath));
    object.insert("output_step_path", geomagic_json::pathToQString(config.outputStepPath));
    object.insert("work_dir", geomagic_json::pathToQString(config.workDir));
    object.insert("config_json_path", geomagic_json::pathToQString(config.configJsonPath));
    object.insert("result_json_path", geomagic_json::pathToQString(config.resultJsonPath));
    object.insert("stdout_log_path", geomagic_json::pathToQString(config.stdoutLogPath));
    object.insert("stderr_log_path", geomagic_json::pathToQString(config.stderrLogPath));
    object.insert("fit_region_log_path", geomagic_json::pathToQString(config.fitRegionLogPath));

    object.insert("keep_temp", config.keepTemp);
    object.insert("skip_remesh", config.skipRemesh);
    object.insert("quick_smooth", config.quickSmooth);
    object.insert("relax", config.relax);
    object.insert("relax_iterations", config.relaxIterations);
    object.insert("relax_strength", config.relaxStrength);

    object.insert("adaptive_fit", config.adaptiveFit);
    object.insert("auto_merge", config.autoMerge);
    object.insert("strict_patch_target", config.strictPatchTarget);
    object.insert("sharpen_contours", config.sharpenContours);
    object.insert("num_patches", config.numPatches);
    object.insert("fallback_num_patches", int_array_to_json(config.fallbackNumPatches));
    object.insert("detail", config.detail);
    object.insert("tolerance", config.tolerance);
    object.insert("geometry", QString::fromStdString(config.geometry));
    object.insert("convert_iges_to_step", config.convertIgesToStep);
    object.insert("timeout_seconds", config.timeoutSeconds);
    return object;
}

GeomagicAutoSurfaceConfigValidation invalid_config(std::string message) {
    GeomagicAutoSurfaceConfigValidation validation;
    validation.message = std::move(message);
    return validation;
}

}

GeomagicAutoSurfaceConfigValidation validateGeomagicAutoSurfaceConfig(
    const GeomagicAutoSurfaceConfig& config) {
    if (config.wrapCorePath.empty()) {
        return invalid_config("wrapCorePath must not be empty.");
    }
    if (config.scriptPath.empty()) {
        return invalid_config("scriptPath must not be empty.");
    }
    if (config.inputStlPath.empty()) {
        return invalid_config("inputStlPath must not be empty.");
    }
    if (config.outputStepPath.empty()) {
        return invalid_config("outputStepPath must not be empty.");
    }
    if (config.workDir.empty()) {
        return invalid_config("workDir must not be empty.");
    }
    if (config.numPatches <= 0) {
        return invalid_config("numPatches must be greater than zero.");
    }
    if (config.timeoutSeconds <= 0) {
        return invalid_config("timeoutSeconds must be greater than zero.");
    }
    if (config.tolerance <= 0.0) {
        return invalid_config("tolerance must be greater than zero.");
    }
    if (config.detail < 0.0 || config.detail > 1.0) {
        return invalid_config("detail must be in [0, 1].");
    }
    if (config.geometry != "Organic" && config.geometry != "Mechanical") {
        return invalid_config("geometry must be Organic or Mechanical.");
    }
    for (const auto fallback : config.fallbackNumPatches) {
        if (fallback <= 0) {
            return invalid_config("fallbackNumPatches entries must be greater than zero.");
        }
    }

    GeomagicAutoSurfaceConfigValidation validation;
    validation.valid = true;
    if (config.autoMerge && config.adaptiveFit) {
        validation.message = "autoMerge=True forces adaptiveFit=False in the backend/runtime.";
    }
    return validation;
}

bool writeGeomagicAutoSurfaceConfigJson(
    const GeomagicAutoSurfaceConfig& config,
    const std::filesystem::path& path,
    std::string* errorMessage) {
    return geomagic_json::writeDocument(
        QJsonDocument(config_to_json(config)),
        path,
        errorMessage);
}

}
