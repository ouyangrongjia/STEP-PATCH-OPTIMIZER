#include "external/geomagic/GeomagicAutoSurfaceConfig.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <system_error>
#include <utility>

namespace spo {

namespace {

QString path_to_json_string(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.c_str()), static_cast<qsizetype>(utf8.size()));
}

QJsonArray int_array_to_json(const std::vector<int>& values) {
    QJsonArray array;
    for (const auto value : values) {
        array.append(value);
    }
    return array;
}

void set_error(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

bool ensure_parent_directory(const std::filesystem::path& path, std::string* errorMessage) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        set_error(errorMessage, "Could not create JSON output directory: " + error.message());
        return false;
    }
    return true;
}

bool write_json_document(
    const QJsonDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage) {
    if (!ensure_parent_directory(path, errorMessage)) {
        return false;
    }

    QFile file(path_to_json_string(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        set_error(errorMessage, "Could not open JSON file for writing.");
        return false;
    }

    if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
        set_error(errorMessage, "Could not write JSON file.");
        return false;
    }

    set_error(errorMessage, {});
    return true;
}

QJsonObject config_to_json(const GeomagicAutoSurfaceConfig& config) {
    QJsonObject object;
    object.insert("wrap_core_path", path_to_json_string(config.wrapCorePath));
    object.insert("script_path", path_to_json_string(config.scriptPath));
    object.insert("input_stl_path", path_to_json_string(config.inputStlPath));
    object.insert("output_iges_path", path_to_json_string(config.outputIgesPath));
    object.insert("output_step_path", path_to_json_string(config.outputStepPath));
    object.insert("work_dir", path_to_json_string(config.workDir));
    object.insert("config_json_path", path_to_json_string(config.configJsonPath));
    object.insert("result_json_path", path_to_json_string(config.resultJsonPath));
    object.insert("stdout_log_path", path_to_json_string(config.stdoutLogPath));
    object.insert("stderr_log_path", path_to_json_string(config.stderrLogPath));
    object.insert("fit_region_log_path", path_to_json_string(config.fitRegionLogPath));

    object.insert("keep_temp", config.keepTemp);
    object.insert("skip_remesh", config.skipRemesh);
    object.insert("quick_smooth", config.quickSmooth);
    object.insert("relax", config.relax);
    object.insert("relax_iterations", config.relaxIterations);
    object.insert("relax_strength", config.relaxStrength);

    object.insert("adaptive_fit", config.adaptiveFit);
    object.insert("auto_merge", config.autoMerge);
    object.insert("strict_patch_target", config.strictPatchTarget);
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
    return write_json_document(QJsonDocument(config_to_json(config)), path, errorMessage);
}

}
