#pragma once

#include "external/geomagic/GeomagicAutoSurfaceResult.h"

#include <filesystem>
#include <string>

namespace spo {

struct PatchArtifactPaths {
    bool success = false;
    std::string message;

    std::filesystem::path localStlPath;
    std::filesystem::path patchStepPath;
    std::filesystem::path patchIgesSidecarPath;
    std::filesystem::path fitRegionLogPath;

    bool foundStep = false;
    bool foundIgesSidecar = false;
    bool foundFitLog = false;
};

class PatchArtifactLocator {
public:
    PatchArtifactPaths locateFromResult(const GeomagicAutoSurfaceResult& result) const;

    PatchArtifactPaths locateFromLocalStl(const std::filesystem::path& localStlPath) const;
};

}
