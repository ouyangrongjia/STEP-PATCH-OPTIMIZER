#include "external/geomagic/GeomagicOutputPathResolver.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>
#include <vector>

namespace spo {

namespace {

GeomagicOutputPaths failure(std::string message) {
    GeomagicOutputPaths result;
    result.message = std::move(message);
    return result;
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::filesystem::path normalized_absolute(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}

std::string component_key(const std::filesystem::path& component) {
    const auto utf8 = component.generic_u8string();
    std::string key(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
#if defined(_WIN32)
    key = ascii_lower(std::move(key));
#endif
    return key;
}

bool starts_with_path(
    const std::filesystem::path& path,
    const std::filesystem::path& root) {
    auto pathIt = path.begin();
    auto rootIt = root.begin();
    for (; rootIt != root.end(); ++rootIt, ++pathIt) {
        if (pathIt == path.end() || component_key(*pathIt) != component_key(*rootIt)) {
            return false;
        }
    }
    return true;
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
            *message = "Could not create Geomagic output directory: " + error.message();
        }
        return false;
    }
    return true;
}

}

GeomagicOutputPaths resolveGeomagicOutputPathsFromCropStl(
    const std::filesystem::path& inputStlPath,
    const std::filesystem::path& cropStlRoot,
    const std::filesystem::path& cropStpRoot,
    const std::filesystem::path& cropIgsRoot) {
    const auto extension = ascii_lower(inputStlPath.extension().string());
    if (extension != ".stl") {
        return failure("Geomagic input path must use .stl extension.");
    }

    const auto normalizedInput = normalized_absolute(inputStlPath);
    const auto normalizedCropStlRoot = normalized_absolute(cropStlRoot);
    if (!starts_with_path(normalizedInput, normalizedCropStlRoot)) {
        return failure("Geomagic input STL is not under the crop_stl root.");
    }

    auto relativePath = normalizedInput.lexically_relative(normalizedCropStlRoot);
    if (relativePath.empty() || relativePath == ".") {
        return failure("Geomagic input STL must include a file path under the crop_stl root.");
    }

    auto outputStepRelative = relativePath;
    outputStepRelative.replace_extension(".stp");
    auto outputIgesRelative = relativePath;
    outputIgesRelative.replace_extension(".igs");

    GeomagicOutputPaths result;
    result.outputStepPath = (cropStpRoot / outputStepRelative).lexically_normal();
    result.outputIgesPath = (cropIgsRoot / outputIgesRelative).lexically_normal();

    std::string message;
    if (!create_parent_directory(result.outputStepPath, &message) ||
        !create_parent_directory(result.outputIgesPath, &message)) {
        return failure(message);
    }

    result.success = true;
    return result;
}

}
