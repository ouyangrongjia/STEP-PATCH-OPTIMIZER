#include "app/PatchPreviewRunLogger.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace spo {

namespace {

std::tm local_time(std::time_t value) {
    std::tm result{};
#if defined(_WIN32)
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}

std::string timestamp_for_file(std::chrono::system_clock::time_point now) {
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto local = local_time(time);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream stream;
    stream << std::put_time(&local, "%Y%m%d_%H%M%S")
           << "_" << std::setw(3) << std::setfill('0') << milliseconds.count();
    return stream.str();
}

std::string timestamp_for_line(std::chrono::system_clock::time_point now) {
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto local = local_time(time);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
           << "." << std::setw(3) << std::setfill('0') << milliseconds.count();
    return stream.str();
}

std::string candidate_token(int candidateId) {
    if (candidateId < 0) {
        return "candidate_unknown";
    }

    std::ostringstream stream;
    stream << "candidate_" << std::setw(4) << std::setfill('0') << candidateId;
    return stream.str();
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.c_str()), text.size()};
}

std::string sanitize(std::string_view text) {
    std::string result(text);
    for (auto& value : result) {
        if (value == '\r' || value == '\n' || value == '\t') {
            value = ' ';
        }
    }
    return result;
}

long long elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

}

PatchPreviewRunLogger::PatchPreviewRunLogger(
    std::filesystem::path logPath,
    int candidateId,
    std::chrono::steady_clock::time_point startedAt) :
    path_(std::move(logPath)),
    candidateId_(candidateId),
    startedAt_(startedAt) {
}

PatchPreviewRunLogger PatchPreviewRunLogger::create(const std::filesystem::path& workspaceRoot, int candidateId) {
    const auto steadyStart = std::chrono::steady_clock::now();
    const auto systemStart = std::chrono::system_clock::now();
    const auto root = workspaceRoot.empty()
        ? std::filesystem::absolute(std::filesystem::current_path()).lexically_normal()
        : std::filesystem::absolute(workspaceRoot).lexically_normal();
    const auto logDir = root / "log";
    const auto filename = "patch_preview_" + timestamp_for_file(systemStart) + "_" + candidate_token(candidateId) + ".log";

    PatchPreviewRunLogger logger(logDir / filename, candidateId, steadyStart);

    std::error_code error;
    std::filesystem::create_directories(logDir, error);
    if (error) {
        logger.errorMessage_ = "Could not create patch preview log directory: " + error.message();
        return logger;
    }

    std::ofstream stream(logger.path_, std::ios::out | std::ios::trunc);
    if (!stream) {
        logger.errorMessage_ = "Could not create patch preview run log.";
        return logger;
    }

    logger.ready_ = true;
    stream << "patch_preview_run_log\n";
    stream << "created_at=" << timestamp_for_line(systemStart) << "\n";
    stream << "workspace_root=" << path_to_utf8(root) << "\n";
    stream << "candidate_id=" << candidateId << "\n";
    stream << "log_path=" << path_to_utf8(logger.path_) << "\n";
    stream << "\n";
    return logger;
}

bool PatchPreviewRunLogger::ready() const noexcept {
    return ready_;
}

const std::filesystem::path& PatchPreviewRunLogger::path() const noexcept {
    return path_;
}

const std::string& PatchPreviewRunLogger::errorMessage() const noexcept {
    return errorMessage_;
}

std::chrono::steady_clock::time_point PatchPreviewRunLogger::startedAt() const noexcept {
    return startedAt_;
}

void PatchPreviewRunLogger::log(std::string_view stage, std::string_view message) const {
    write(stage, message, -1);
}

void PatchPreviewRunLogger::logDuration(
    std::string_view stage,
    std::string_view message,
    std::chrono::steady_clock::time_point operationStart) const {
    write(stage, message, elapsed_ms(operationStart, std::chrono::steady_clock::now()));
}

void PatchPreviewRunLogger::write(std::string_view stage, std::string_view message, long long durationMs) const {
    if (!ready_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::ofstream stream(path_, std::ios::out | std::ios::app);
    if (!stream) {
        return;
    }

    stream << "timestamp=" << timestamp_for_line(std::chrono::system_clock::now())
           << " elapsed_ms=" << elapsed_ms(startedAt_, now);
    if (durationMs >= 0) {
        stream << " duration_ms=" << durationMs;
    }
    stream << " stage=" << sanitize(stage)
           << " message=" << sanitize(message)
           << "\n";
}

}
