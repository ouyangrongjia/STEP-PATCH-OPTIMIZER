#include "external/geomagic/GeomagicAutoSurfaceBackend.h"

#include "external/geomagic/GeomagicOutputPathResolver.h"

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

#include <algorithm>
#include <limits>
#include <system_error>

namespace spo {

namespace {

QString path_to_qstring(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.c_str()), static_cast<qsizetype>(utf8.size()));
}

QString path_to_runtime_qstring(const std::filesystem::path& path) {
    return path_to_qstring(std::filesystem::absolute(path).lexically_normal());
}

QString string_to_qstring(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string qstring_to_string(const QString& value) {
    const auto utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

std::string bytearray_to_string(const QByteArray& value) {
    return {value.constData(), static_cast<std::size_t>(value.size())};
}

void append_message(std::string& target, const std::string& message) {
    if (message.empty()) {
        return;
    }
    if (!target.empty()) {
        target += "\n";
    }
    target += message;
}

GeomagicAutoSurfaceResult failure_result(
    const GeomagicAutoSurfaceConfig& config,
    std::string message,
    long long durationMs = 0) {
    GeomagicAutoSurfaceResult result;
    result.message = message;
    result.errorMessage = std::move(message);
    result.inputStlPath = config.inputStlPath;
    result.outputIgesPath = config.outputIgesPath;
    result.outputStepPath = config.outputStepPath;
    result.configJsonPath = config.configJsonPath;
    result.resultJsonPath = config.resultJsonPath;
    result.stdoutLogPath = config.stdoutLogPath;
    result.stderrLogPath = config.stderrLogPath;
    result.fitRegionLogPath = config.fitRegionLogPath;
    result.durationMs = durationMs;
    return result;
}

std::filesystem::path default_sidecar_path(
    const std::filesystem::path& outputStepPath,
    const char* suffix) {
    auto filename = outputStepPath.stem();
    filename += suffix;
    return outputStepPath.parent_path() / filename;
}

bool create_parent_directory(const std::filesystem::path& path, std::string* message) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        if (message != nullptr) {
            *message = "Could not create directory: " + error.message();
        }
        return false;
    }
    return true;
}

bool create_runtime_directories(const GeomagicAutoSurfaceConfig& config, std::string* message) {
    std::error_code error;
    std::filesystem::create_directories(config.workDir, error);
    if (error) {
        if (message != nullptr) {
            *message = "Could not create Geomagic workDir: " + error.message();
        }
        return false;
    }

    const std::filesystem::path paths[] = {
        config.outputStepPath,
        config.outputIgesPath,
        config.configJsonPath,
        config.resultJsonPath,
        config.stdoutLogPath,
        config.stderrLogPath,
        config.fitRegionLogPath,
    };
    for (const auto& path : paths) {
        if (!create_parent_directory(path, message)) {
            return false;
        }
    }
    return true;
}

GeomagicAutoSurfaceConfig complete_config(
    const GeomagicAutoSurfaceConfig& config,
    std::string* errorMessage) {
    auto effective = config;

    if (effective.outputStepPath.empty() || effective.outputIgesPath.empty()) {
        const auto paths = resolveGeomagicOutputPathsFromCropStl(effective.inputStlPath);
        if (!paths.success) {
            if (errorMessage != nullptr) {
                *errorMessage = paths.message;
            }
            return effective;
        }
        effective.outputStepPath = paths.outputStepPath;
        effective.outputIgesPath = paths.outputIgesPath;
    }

    if (effective.workDir.empty()) {
        effective.workDir = effective.outputStepPath.parent_path();
    }
    if (effective.configJsonPath.empty()) {
        effective.configJsonPath = default_sidecar_path(effective.outputStepPath, "_autosurface_config.json");
    }
    if (effective.resultJsonPath.empty()) {
        effective.resultJsonPath = default_sidecar_path(effective.outputStepPath, "_autosurface_result.json");
    }
    if (effective.stdoutLogPath.empty()) {
        effective.stdoutLogPath = default_sidecar_path(effective.outputStepPath, "_autosurface_stdout.log");
    }
    if (effective.stderrLogPath.empty()) {
        effective.stderrLogPath = default_sidecar_path(effective.outputStepPath, "_autosurface_stderr.log");
    }
    if (effective.fitRegionLogPath.empty()) {
        effective.fitRegionLogPath = default_sidecar_path(effective.outputStepPath, "_fit_region.log");
    }

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return effective;
}

QString bool_env(bool value) {
    return value ? QStringLiteral("1") : QStringLiteral("0");
}

QProcessEnvironment build_environment(const GeomagicAutoSurfaceConfig& config) {
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto adaptiveFit = config.autoMerge && config.adaptiveFit ? false : config.adaptiveFit;

    environment.insert(QStringLiteral("FIT_REGION_INPUT"), path_to_runtime_qstring(config.inputStlPath));
    environment.insert(QStringLiteral("FIT_REGION_OUTPUT"), path_to_runtime_qstring(config.outputStepPath));
    environment.insert(QStringLiteral("FIT_REGION_OUTPUT_IGES"), path_to_runtime_qstring(config.outputIgesPath));
    environment.insert(QStringLiteral("FIT_REGION_WORK_DIR"), path_to_runtime_qstring(config.workDir));
    environment.insert(QStringLiteral("FIT_REGION_CONFIG_JSON"), path_to_runtime_qstring(config.configJsonPath));
    environment.insert(QStringLiteral("FIT_REGION_RESULT_JSON"), path_to_runtime_qstring(config.resultJsonPath));
    environment.insert(QStringLiteral("FIT_REGION_LOG_FILE"), path_to_runtime_qstring(config.fitRegionLogPath));

    environment.insert(QStringLiteral("FIT_REGION_KEEP_TEMP"), bool_env(config.keepTemp));
    environment.insert(QStringLiteral("FIT_REGION_SKIP_REMESH"), bool_env(config.skipRemesh));
    environment.insert(QStringLiteral("FIT_REGION_QUICK_SMOOTH"), bool_env(config.quickSmooth));
    environment.insert(QStringLiteral("FIT_REGION_RELAX"), bool_env(config.relax));
    environment.insert(QStringLiteral("FIT_REGION_RELAX_ITERATION"), QString::number(config.relaxIterations));
    environment.insert(QStringLiteral("FIT_REGION_RELAX_STRENGTH"), QString::number(config.relaxStrength));

    environment.insert(QStringLiteral("FIT_REGION_AUTOSURFACE_TARGET"), QString::number(config.numPatches));
    environment.insert(QStringLiteral("FIT_REGION_AUTOSURFACE_TOLERANCE"), QString::number(config.tolerance));
    environment.insert(QStringLiteral("FIT_REGION_DETAIL_LEVEL"), QString::number(config.detail));
    environment.insert(QStringLiteral("FIT_REGION_GEOMETRY_MODE"), string_to_qstring(config.geometry));
    environment.insert(QStringLiteral("FIT_REGION_AUTO_MERGE"), bool_env(config.autoMerge));
    environment.insert(QStringLiteral("FIT_REGION_ADAPTIVE_FIT"), bool_env(adaptiveFit));
    environment.insert(QStringLiteral("FIT_REGION_STRICT_PATCH_TARGET"), bool_env(config.strictPatchTarget));
    return environment;
}

bool write_log_file(
    const std::filesystem::path& path,
    const QByteArray& content,
    std::string* errorMessage) {
    if (!create_parent_directory(path, errorMessage)) {
        return false;
    }

    QFile file(path_to_qstring(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not open log file for writing.";
        }
        return false;
    }
    if (file.write(content) < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not write log file.";
        }
        return false;
    }
    return true;
}

int timeout_milliseconds(int timeoutSeconds) {
    constexpr int kMaxSeconds = std::numeric_limits<int>::max() / 1000;
    return std::min(timeoutSeconds, kMaxSeconds) * 1000;
}

void supplement_result(
    GeomagicAutoSurfaceResult& result,
    const GeomagicAutoSurfaceConfig& config,
    int exitCode,
    bool timedOut,
    long long durationMs,
    const std::string& forceAdaptiveMessage) {
    result.timedOut = result.timedOut || timedOut;
    if (result.exitCode == -1 && exitCode != -1) {
        result.exitCode = exitCode;
    }
    result.inputStlPath = config.inputStlPath;
    result.outputIgesPath = config.outputIgesPath;
    result.outputStepPath = config.outputStepPath;
    result.configJsonPath = config.configJsonPath;
    result.resultJsonPath = config.resultJsonPath;
    result.stdoutLogPath = config.stdoutLogPath;
    result.stderrLogPath = config.stderrLogPath;
    result.fitRegionLogPath = config.fitRegionLogPath;
    result.durationMs = durationMs;
    append_message(result.message, forceAdaptiveMessage);
}

GeomagicAutoSurfaceResult load_result_or_fallback(
    const GeomagicAutoSurfaceConfig& config,
    int exitCode,
    bool timedOut,
    long long durationMs,
    const QByteArray& stdoutData,
    const QByteArray& stderrData,
    const std::string& forceAdaptiveMessage) {
    GeomagicAutoSurfaceResult result;
    std::string readError;
    if (std::filesystem::exists(config.resultJsonPath)) {
        result = readGeomagicAutoSurfaceResultJson(config.resultJsonPath, &readError);
        if (!readError.empty()) {
            result = {};
            result.success = false;
            result.errorMessage = readError;
        }
    } else {
        result.success = std::filesystem::exists(config.outputStepPath);
        result.exitCode = exitCode;
        result.message = result.success ? "Geomagic process completed without result JSON." : "Geomagic process did not produce result JSON.";
        if (!result.success) {
            result.errorMessage = bytearray_to_string(stderrData);
            if (result.errorMessage.empty()) {
                result.errorMessage = result.message;
            }
        }
    }

    supplement_result(result, config, exitCode, timedOut, durationMs, forceAdaptiveMessage);

    if (timedOut) {
        result.success = false;
        result.timedOut = true;
        result.exitCode = -1;
        result.errorMessage = "Geomagic AutoSurface process timed out.";
        append_message(result.message, result.errorMessage);
        return result;
    }

    if (!std::filesystem::exists(config.outputStepPath)) {
        result.success = false;
        append_message(result.message, "Geomagic output STEP file was not created.");
        if (result.errorMessage.empty()) {
            result.errorMessage = "Geomagic output STEP file was not created.";
        }
    } else if (!std::filesystem::exists(config.outputIgesPath)) {
        append_message(result.message, "Warning: Geomagic output IGES file was not created.");
    }

    if (exitCode != 0 && !result.success && result.errorMessage.empty()) {
        result.errorMessage = bytearray_to_string(stderrData);
        if (result.errorMessage.empty()) {
            result.errorMessage = "Geomagic AutoSurface process failed.";
        }
    }
    if (result.message.empty() && !stdoutData.isEmpty()) {
        result.message = bytearray_to_string(stdoutData);
    }
    return result;
}

}

GeomagicAutoSurfaceResult GeomagicAutoSurfaceBackend::run(const GeomagicAutoSurfaceConfig& config) const {
    QElapsedTimer timer;
    timer.start();

    std::string completionError;
    auto effective = complete_config(config, &completionError);
    if (!completionError.empty()) {
        return failure_result(effective, completionError, timer.elapsed());
    }

    const auto validation = validateGeomagicAutoSurfaceConfig(effective);
    if (!validation.valid) {
        return failure_result(effective, validation.message, timer.elapsed());
    }

    if (!std::filesystem::exists(effective.inputStlPath)) {
        return failure_result(effective, "Input STL file does not exist.", timer.elapsed());
    }
    if (!std::filesystem::exists(effective.scriptPath)) {
        return failure_result(effective, "Geomagic AutoSurface script file does not exist.", timer.elapsed());
    }

    std::string directoryError;
    if (!create_runtime_directories(effective, &directoryError)) {
        return failure_result(effective, directoryError, timer.elapsed());
    }

    std::string configWriteError;
    if (!writeGeomagicAutoSurfaceConfigJson(effective, effective.configJsonPath, &configWriteError)) {
        return failure_result(effective, configWriteError, timer.elapsed());
    }

    const auto forceAdaptiveMessage = effective.autoMerge && effective.adaptiveFit
        ? std::string("autoMerge=True forces adaptiveFit=False.")
        : std::string();

    QProcess process;
    process.setProgram(path_to_qstring(effective.wrapCorePath));
    process.setArguments({QStringLiteral("--script"), path_to_qstring(effective.scriptPath)});
    process.setWorkingDirectory(path_to_runtime_qstring(effective.workDir));
    process.setProcessEnvironment(build_environment(effective));
    process.start();

    if (!process.waitForStarted()) {
        auto result = failure_result(
            effective,
            "Could not start Geomagic AutoSurface process: " + qstring_to_string(process.errorString()),
            timer.elapsed());
        return result;
    }

    bool timedOut = false;
    if (!process.waitForFinished(timeout_milliseconds(effective.timeoutSeconds))) {
        timedOut = true;
        process.kill();
        process.waitForFinished(5000);
    }

    const auto stdoutData = process.readAllStandardOutput();
    const auto stderrData = process.readAllStandardError();

    std::string logError;
    if (!write_log_file(effective.stdoutLogPath, stdoutData, &logError)) {
        return failure_result(effective, logError, timer.elapsed());
    }
    if (!write_log_file(effective.stderrLogPath, stderrData, &logError)) {
        return failure_result(effective, logError, timer.elapsed());
    }

    const auto exitCode = timedOut ? -1 : process.exitCode();
    return load_result_or_fallback(
        effective,
        exitCode,
        timedOut,
        timer.elapsed(),
        stdoutData,
        stderrData,
        forceAdaptiveMessage);
}

}
