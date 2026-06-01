#pragma once

#include <filesystem>
#include <string>

namespace spo {

struct GeomagicOutputPaths {
    bool success = false;
    std::filesystem::path outputStepPath;
    std::filesystem::path outputIgesPath;
    std::string message;
};

GeomagicOutputPaths resolveGeomagicOutputPathsFromCropStl(
    const std::filesystem::path& inputStlPath,
    const std::filesystem::path& cropStlRoot = std::filesystem::path("data/crop_stl"),
    const std::filesystem::path& cropStpRoot = std::filesystem::path("data/crop_stp"),
    const std::filesystem::path& cropIgsRoot = std::filesystem::path("data/crop_igs"));

}
