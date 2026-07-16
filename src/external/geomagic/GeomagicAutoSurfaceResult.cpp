#include "external/geomagic/GeomagicAutoSurfaceResult.h"
#include "external/geomagic/GeomagicJsonFile.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace spo {

namespace {

std::filesystem::path path_from_json_string(const QJsonObject& object, const char* key) {
    return std::filesystem::path(object.value(key).toString().toStdWString());
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

    object.insert("input_stl_path", geomagic_json::pathToQString(result.inputStlPath));
    object.insert("output_iges_path", geomagic_json::pathToQString(result.outputIgesPath));
    object.insert("output_step_path", geomagic_json::pathToQString(result.outputStepPath));
    object.insert("preserved_iges_path", geomagic_json::pathToQString(result.preservedIgesPath));
    object.insert("config_json_path", geomagic_json::pathToQString(result.configJsonPath));
    object.insert("result_json_path", geomagic_json::pathToQString(result.resultJsonPath));
    object.insert("stdout_log_path", geomagic_json::pathToQString(result.stdoutLogPath));
    object.insert("stderr_log_path", geomagic_json::pathToQString(result.stderrLogPath));
    object.insert("fit_region_log_path", geomagic_json::pathToQString(result.fitRegionLogPath));
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
    return geomagic_json::writeDocument(
        QJsonDocument(result_to_json(result)),
        path,
        errorMessage);
}

GeomagicAutoSurfaceResult readGeomagicAutoSurfaceResultJson(
    const std::filesystem::path& path,
    std::string* errorMessage) {
    QFile file(geomagic_json::pathToQString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        geomagic_json::setError(errorMessage, "Could not open result JSON file for reading.");
        return {};
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        geomagic_json::setError(errorMessage, "Could not parse result JSON file.");
        return {};
    }

    geomagic_json::setError(errorMessage, {});
    return result_from_json(document.object());
}

}
