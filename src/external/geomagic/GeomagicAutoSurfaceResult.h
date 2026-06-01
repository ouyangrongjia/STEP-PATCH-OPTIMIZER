#pragma once

#include <filesystem>
#include <string>

namespace spo {

struct GeomagicAutoSurfaceResult {
    bool success = false;
    bool timedOut = false;

    int exitCode = -1;
    int bodies = 0;
    int openLoops = 0;

    std::string message;
    std::string errorMessage;
    std::string failedStage;

    std::filesystem::path inputStlPath;
    std::filesystem::path outputIgesPath;
    std::filesystem::path outputStepPath;
    std::filesystem::path preservedIgesPath;

    std::filesystem::path configJsonPath;
    std::filesystem::path resultJsonPath;
    std::filesystem::path stdoutLogPath;
    std::filesystem::path stderrLogPath;
    std::filesystem::path fitRegionLogPath;

    long long durationMs = 0;
};

bool writeGeomagicAutoSurfaceResultJson(
    const GeomagicAutoSurfaceResult& result,
    const std::filesystem::path& path,
    std::string* errorMessage = nullptr);

GeomagicAutoSurfaceResult readGeomagicAutoSurfaceResultJson(
    const std::filesystem::path& path,
    std::string* errorMessage = nullptr);

}
