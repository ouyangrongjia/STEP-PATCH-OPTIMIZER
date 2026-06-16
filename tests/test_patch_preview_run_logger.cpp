#include "app/PatchPreviewRunLogger.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

std::filesystem::path temp_root(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void test_creates_timestamped_root_log_for_candidate() {
    const auto root = temp_root("spo_patch_preview_run_logger_basic");

    const auto logger = spo::PatchPreviewRunLogger::create(root, 179);

    assert(logger.ready());
    assert(logger.errorMessage().empty());
    assert(std::filesystem::is_directory(root / "log"));
    assert(std::filesystem::exists(logger.path()));
    assert(logger.path().parent_path() == root / "log");
    assert(logger.path().extension() == ".log");
    assert(logger.path().filename().string().find("patch_preview_") != std::string::npos);
    assert(logger.path().filename().string().find("candidate_0179") != std::string::npos);

    logger.log("AnalyzingBoundary", "Patch preview pipeline started.");
    const auto operationStart = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    logger.logDuration("RunningGeomagic", "Geomagic AutoSurface finished.", operationStart);

    const auto text = read_text(logger.path());
    assert(text.find("candidate_id=179") != std::string::npos);
    assert(text.find("stage=AnalyzingBoundary") != std::string::npos);
    assert(text.find("stage=RunningGeomagic") != std::string::npos);
    assert(text.find("message=Patch preview pipeline started.") != std::string::npos);
    assert(text.find("message=Geomagic AutoSurface finished.") != std::string::npos);
    assert(text.find("elapsed_ms=") != std::string::npos);
    assert(text.find("duration_ms=") != std::string::npos);

    std::filesystem::remove_all(root);
}

}

void run_patch_preview_run_logger_tests() {
    test_creates_timestamped_root_log_for_candidate();
}
