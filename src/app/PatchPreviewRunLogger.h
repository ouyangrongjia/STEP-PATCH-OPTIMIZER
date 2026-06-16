#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace spo {

class PatchPreviewRunLogger {
public:
    PatchPreviewRunLogger() = default;

    static PatchPreviewRunLogger create(const std::filesystem::path& workspaceRoot, int candidateId);

    bool ready() const noexcept;
    const std::filesystem::path& path() const noexcept;
    const std::string& errorMessage() const noexcept;
    std::chrono::steady_clock::time_point startedAt() const noexcept;

    void log(std::string_view stage, std::string_view message) const;
    void logDuration(
        std::string_view stage,
        std::string_view message,
        std::chrono::steady_clock::time_point operationStart) const;

private:
    PatchPreviewRunLogger(
        std::filesystem::path logPath,
        int candidateId,
        std::chrono::steady_clock::time_point startedAt);

    void write(std::string_view stage, std::string_view message, long long durationMs) const;

    std::filesystem::path path_;
    int candidateId_ = -1;
    std::chrono::steady_clock::time_point startedAt_{};
    bool ready_ = false;
    std::string errorMessage_;
};

}
