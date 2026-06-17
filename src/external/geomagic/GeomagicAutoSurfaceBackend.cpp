#include "external/geomagic/GeomagicAutoSurfaceBackend.h"

#include "external/geomagic/GeomagicOutputPathResolver.h"

#include <QElapsedTimer>
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

std::string qstring_to_string(const QString& value) {
    const auto utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

std::string bytearray_to_string(const QByteArray& value) {
    return {value.constData(), static_cast<std::size_t>(value.size())};
}

QString bool_env(bool value) {
    return value ? QStringLiteral("1") : QStringLiteral("0");
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
        config.fitRegionLogPath,
    };
    for (const auto& path : paths) {
        if (!path.empty() && !create_parent_directory(path, message)) {
            return false;
        }
    }
    return true;
}

bool remove_stale_output_file(const std::filesystem::path& path, std::string* message) {
    if (path.empty()) {
        return true;
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return true;
    }
    if (error) {
        if (message != nullptr) {
            *message = "Could not inspect stale Geomagic output: " + error.message();
        }
        return false;
    }
    std::filesystem::remove(path, error);
    if (error) {
        if (message != nullptr) {
            *message = "Could not remove stale Geomagic output: " + error.message();
        }
        return false;
    }
    return true;
}

bool remove_stale_output_artifacts(const GeomagicAutoSurfaceConfig& config, std::string* message) {
    return remove_stale_output_file(config.outputStepPath, message) &&
        remove_stale_output_file(config.outputIgesPath, message) &&
        remove_stale_output_file(default_sidecar_path(config.outputStepPath, "_autosurface.igs"), message);
}

GeomagicAutoSurfaceConfig complete_config(
    const GeomagicAutoSurfaceConfig& config,
    std::string* errorMessage) {
    auto effective = config;

    if (effective.outputStepPath.empty()) {
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
        effective.workDir = std::filesystem::current_path();
    }
    effective.workDir = std::filesystem::absolute(effective.workDir).lexically_normal();
    if (effective.fitRegionLogPath.empty()) {
        effective.fitRegionLogPath = default_sidecar_path(effective.outputStepPath, "_fit_region.log");
    }

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return effective;
}

QProcessEnvironment build_environment(const GeomagicAutoSurfaceConfig& config) {
    auto environment = QProcessEnvironment::systemEnvironment();

    environment.remove(QStringLiteral("FIT_REGION_OUTPUT_IGES"));
    environment.remove(QStringLiteral("FIT_REGION_WORK_DIR"));
    environment.remove(QStringLiteral("FIT_REGION_CONFIG_JSON"));
    environment.remove(QStringLiteral("FIT_REGION_RESULT_JSON"));
    environment.remove(QStringLiteral("FIT_REGION_LOG_FILE"));
    environment.remove(QStringLiteral("FIT_REGION_REPAIR_MESH"));
    environment.remove(QStringLiteral("FIT_REGION_KEEP_TEMP"));
    environment.remove(QStringLiteral("FIT_REGION_SKIP_REMESH"));
    environment.remove(QStringLiteral("FIT_REGION_QUICK_SMOOTH"));
    environment.remove(QStringLiteral("FIT_REGION_RELAX"));
    environment.remove(QStringLiteral("FIT_REGION_RELAX_ITERATION"));
    environment.remove(QStringLiteral("FIT_REGION_RELAX_STRENGTH"));
    environment.remove(QStringLiteral("FIT_REGION_AUTOSURFACE_TARGET"));
    environment.remove(QStringLiteral("FIT_REGION_AUTOSURFACE_TOLERANCE"));
    environment.remove(QStringLiteral("FIT_REGION_DETAIL_LEVEL"));
    environment.remove(QStringLiteral("FIT_REGION_GEOMETRY_MODE"));
    environment.remove(QStringLiteral("FIT_REGION_AUTO_MERGE"));
    environment.remove(QStringLiteral("FIT_REGION_ADAPTIVE_FIT"));
    environment.remove(QStringLiteral("FIT_REGION_SHARPEN_CONTOURS"));
    environment.remove(QStringLiteral("FIT_REGION_FILL_HOLE_MAX_EDGES"));
    environment.remove(QStringLiteral("FIT_REGION_FILL_HOLE_LENGTH_RATIO"));

    environment.insert(QStringLiteral("FIT_REGION_INPUT"), path_to_runtime_qstring(config.inputStlPath));
    environment.insert(QStringLiteral("FIT_REGION_OUTPUT"), path_to_runtime_qstring(config.outputStepPath));
    environment.insert(QStringLiteral("FIT_REGION_LOG_FILE"), path_to_runtime_qstring(config.fitRegionLogPath));
    environment.insert(QStringLiteral("FIT_REGION_REPAIR_MESH"), QStringLiteral("1"));
    environment.insert(QStringLiteral("FIT_REGION_KEEP_TEMP"), bool_env(config.keepTemp));
    environment.insert(QStringLiteral("FIT_REGION_SKIP_REMESH"), bool_env(config.skipRemesh));
    environment.insert(QStringLiteral("FIT_REGION_QUICK_SMOOTH"), bool_env(config.quickSmooth));
    environment.insert(QStringLiteral("FIT_REGION_RELAX"), bool_env(config.relax));
    environment.insert(QStringLiteral("FIT_REGION_RELAX_ITERATION"), QString::number(config.relaxIterations));
    environment.insert(QStringLiteral("FIT_REGION_RELAX_STRENGTH"), QString::number(config.relaxStrength, 'g', 12));
    environment.insert(QStringLiteral("FIT_REGION_AUTOSURFACE_TARGET"), QString::number(config.numPatches));
    environment.insert(QStringLiteral("FIT_REGION_AUTOSURFACE_TOLERANCE"), QString::number(config.tolerance, 'g', 12));
    environment.insert(QStringLiteral("FIT_REGION_DETAIL_LEVEL"), QString::number(config.detail, 'g', 12));
    environment.insert(QStringLiteral("FIT_REGION_GEOMETRY_MODE"), QString::fromStdString(config.geometry));
    environment.insert(QStringLiteral("FIT_REGION_AUTO_MERGE"), bool_env(config.autoMerge));
    environment.insert(QStringLiteral("FIT_REGION_ADAPTIVE_FIT"), bool_env(config.adaptiveFit));
    environment.insert(QStringLiteral("FIT_REGION_SHARPEN_CONTOURS"), bool_env(config.sharpenContours));
    environment.insert(
        QStringLiteral("FIT_REGION_STRICT_PATCH_TARGET"),
        config.strictPatchTarget ? QStringLiteral("1") : QStringLiteral("0"));
    return environment;
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
    long long durationMs) {
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
}

GeomagicAutoSurfaceResult load_result_or_fallback(
    const GeomagicAutoSurfaceConfig& config,
    int exitCode,
    bool timedOut,
    long long durationMs,
    const QByteArray& stdoutData,
    const QByteArray& stderrData) {
    GeomagicAutoSurfaceResult result;
    result.success = std::filesystem::exists(config.outputStepPath);
    result.exitCode = exitCode;
    result.message = bytearray_to_string(stdoutData);
    result.errorMessage = bytearray_to_string(stderrData);
    if (result.success && result.message.empty()) {
        result.message = "Geomagic process completed and output STEP was created.";
    }

    supplement_result(result, config, exitCode, timedOut, durationMs);

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
    } else if (!config.outputIgesPath.empty() && std::filesystem::exists(config.outputIgesPath)) {
        result.preservedIgesPath = config.outputIgesPath;
    } else {
        append_message(result.message, "Warning: Geomagic output IGES file was not created.");
    }

    if (exitCode != 0 && !result.success && result.errorMessage.empty()) {
        result.errorMessage = bytearray_to_string(stderrData);
        if (result.errorMessage.empty()) {
            result.errorMessage = "Geomagic AutoSurface process failed.";
        }
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
    std::string staleOutputError;
    if (!remove_stale_output_artifacts(effective, &staleOutputError)) {
        return failure_result(effective, staleOutputError, timer.elapsed());
    }

    QProcess process;
    process.setProgram(path_to_qstring(effective.wrapCorePath));
    process.setArguments({QStringLiteral("--script"), path_to_runtime_qstring(effective.scriptPath)});
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

    const auto exitCode = timedOut ? -1 : process.exitCode();
    return load_result_or_fallback(
        effective,
        exitCode,
        timedOut,
        timer.elapsed(),
        stdoutData,
        stderrData);
}

}
