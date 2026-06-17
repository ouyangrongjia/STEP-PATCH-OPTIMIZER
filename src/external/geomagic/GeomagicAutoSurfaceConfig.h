#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace spo {

struct GeomagicAutoSurfaceConfig {
    std::filesystem::path wrapCorePath = std::filesystem::path("E:/Geomagic Wrap/wrapCore.exe");
    std::filesystem::path scriptPath = std::filesystem::path("scripts/geomagic_wrap/autosurface_pipeline.py");

    std::filesystem::path inputStlPath;
    std::filesystem::path outputIgesPath;
    std::filesystem::path outputStepPath;
    std::filesystem::path workDir;

    std::filesystem::path configJsonPath;
    std::filesystem::path resultJsonPath;
    std::filesystem::path stdoutLogPath;
    std::filesystem::path stderrLogPath;
    std::filesystem::path fitRegionLogPath;

    bool keepTemp = true;
    bool skipRemesh = true;
    bool quickSmooth = false;
    bool relax = false;
    int relaxIterations = 2;
    double relaxStrength = 0.25;

    bool adaptiveFit = false;
    bool autoMerge = true;
    bool strictPatchTarget = true;
    bool sharpenContours = false;

    int numPatches = 1;
    std::vector<int> fallbackNumPatches = {2, 4, 8};

    double detail = 0.10;
    double tolerance = 0.03;
    std::string geometry = "Mechanical";

    bool convertIgesToStep = true;
    int timeoutSeconds = 1800;
};

struct GeomagicAutoSurfaceConfigValidation {
    bool valid = false;
    std::string message;
};

GeomagicAutoSurfaceConfigValidation validateGeomagicAutoSurfaceConfig(
    const GeomagicAutoSurfaceConfig& config);

bool writeGeomagicAutoSurfaceConfigJson(
    const GeomagicAutoSurfaceConfig& config,
    const std::filesystem::path& path,
    std::string* errorMessage = nullptr);

}
