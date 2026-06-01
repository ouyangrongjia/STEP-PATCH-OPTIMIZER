#include "external/geomagic/GeomagicAutoSurfaceResult.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <system_error>

namespace spo {

namespace {

QString path_to_json_string(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.c_str()), static_cast<qsizetype>(utf8.size()));
}

std::filesystem::path path_from_json_string(const QJsonObject& object, const char* key) {
    return std::filesystem::path(object.value(key).toString().toStdWString());
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

QJsonObject result_to_json(const GeomagicAutoSurfaceResult& result) {
    QJsonObject object;
    object.insert("success", result.success);
    object.insert("timed_out", result.timedOut);
    object.insert("exit_code", result.exitCode);
    object.insert("bodies", result.bodies);
    object.insert("open_loops", result.openLoops);
    object.insert("message", QString::fromStdString(result.message));
    object.insert("error_message", QString::fromStdString(result.errorMessage));
    object.insert("failed_stage", QString::fromStdString(result.failedStage));

    object.insert("input_stl_path", path_to_json_string(result.inputStlPath));
    object.insert("output_iges_path", path_to_json_string(result.outputIgesPath));
    object.insert("output_step_path", path_to_json_string(result.outputStepPath));
    object.insert("preserved_iges_path", path_to_json_string(result.preservedIgesPath));
    object.insert("config_json_path", path_to_json_string(result.configJsonPath));
    object.insert("result_json_path", path_to_json_string(result.resultJsonPath));
    object.insert("stdout_log_path", path_to_json_string(result.stdoutLogPath));
    object.insert("stderr_log_path", path_to_json_string(result.stderrLogPath));
    object.insert("fit_region_log_path", path_to_json_string(result.fitRegionLogPath));
    object.insert("duration_ms", static_cast<qint64>(result.durationMs));
    return object;
}

GeomagicAutoSurfaceResult result_from_json(const QJsonObject& object) {
    GeomagicAutoSurfaceResult result;
    result.success = object.value("success").toBool();
    result.timedOut = object.value("timed_out").toBool();
    result.exitCode = object.value("exit_code").toInt(-1);
    result.bodies = object.value("bodies").toInt();
    result.openLoops = object.value("open_loops").toInt();
    result.message = object.value("message").toString().toStdString();
    result.errorMessage = object.value("error_message").toString().toStdString();
    result.failedStage = object.value("failed_stage").toString().toStdString();

    result.inputStlPath = path_from_json_string(object, "input_stl_path");
    result.outputIgesPath = path_from_json_string(object, "output_iges_path");
    result.outputStepPath = path_from_json_string(object, "output_step_path");
    result.preservedIgesPath = path_from_json_string(object, "preserved_iges_path");
    result.configJsonPath = path_from_json_string(object, "config_json_path");
    result.resultJsonPath = path_from_json_string(object, "result_json_path");
    result.stdoutLogPath = path_from_json_string(object, "stdout_log_path");
    result.stderrLogPath = path_from_json_string(object, "stderr_log_path");
    result.fitRegionLogPath = path_from_json_string(object, "fit_region_log_path");
    result.durationMs = static_cast<long long>(object.value("duration_ms").toDouble());
    return result;
}

}

bool writeGeomagicAutoSurfaceResultJson(
    const GeomagicAutoSurfaceResult& result,
    const std::filesystem::path& path,
    std::string* errorMessage) {
    return write_json_document(QJsonDocument(result_to_json(result)), path, errorMessage);
}

GeomagicAutoSurfaceResult readGeomagicAutoSurfaceResultJson(
    const std::filesystem::path& path,
    std::string* errorMessage) {
    QFile file(path_to_json_string(path));
    if (!file.open(QIODevice::ReadOnly)) {
        set_error(errorMessage, "Could not open result JSON file for reading.");
        return {};
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        set_error(errorMessage, "Could not parse result JSON file.");
        return {};
    }

    set_error(errorMessage, {});
    return result_from_json(document.object());
}

}
