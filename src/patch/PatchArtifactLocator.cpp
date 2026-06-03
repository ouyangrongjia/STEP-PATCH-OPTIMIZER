#include "patch/PatchArtifactLocator.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace spo {

namespace {

PatchArtifactPaths failed(std::filesystem::path localStlPath, std::string message) {
    PatchArtifactPaths result;
    result.localStlPath = std::move(localStlPath);
    result.message = std::move(message);
    return result;
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string path_text(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.c_str()), utf8.size()};
}

std::string component_key(const std::filesystem::path& component) {
    auto key = path_text(component);
#if defined(_WIN32)
    key = ascii_lower(std::move(key));
#endif
    return key;
}

std::filesystem::path normalized_absolute(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

bool regular_file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::optional<std::filesystem::file_time_type> modified_time(const std::filesystem::path& path) {
    std::error_code error;
    const auto value = std::filesystem::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
    return value;
}

bool starts_with_path(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto pathIt = path.begin();
    auto rootIt = root.begin();
    for (; rootIt != root.end(); ++rootIt, ++pathIt) {
        if (pathIt == path.end() || component_key(*pathIt) != component_key(*rootIt)) {
            return false;
        }
    }
    return true;
}

bool find_crop_stl_root(
    const std::filesystem::path& inputPath,
    std::filesystem::path& cropStlRoot,
    std::filesystem::path& dataRoot) {
    std::filesystem::path current;
    std::filesystem::path previous;
    for (const auto& component : inputPath) {
        current /= component;
        if (component_key(previous.filename()) == "data" && component_key(component) == "crop_stl") {
            cropStlRoot = current;
            dataRoot = previous;
            return true;
        }
        previous = current;
    }
    return false;
}

std::filesystem::path with_stem_suffix(
    const std::filesystem::path& directory,
    const std::string& stem,
    const std::string& suffix,
    const char* extension) {
    return directory / (stem + suffix + extension);
}

std::string stem_string(const std::filesystem::path& path) {
    const auto utf8 = path.stem().generic_u8string();
    return {reinterpret_cast<const char*>(utf8.c_str()), utf8.size()};
}

std::string extension_key(const std::filesystem::path& path) {
    return ascii_lower(path.extension().string());
}

bool stem_starts_with(const std::filesystem::path& path, const std::string& prefix) {
    return stem_string(path).rfind(prefix, 0) == 0;
}

std::vector<std::filesystem::path> matching_wildcard_steps(
    const std::filesystem::path& directory,
    const std::string& stem,
    const char* extension) {
    std::vector<std::filesystem::path> matches;
    if (!path_exists(directory)) {
        return matches;
    }

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        const auto path = entry.path();
        if (!entry.is_regular_file() || extension_key(path) != extension) {
            continue;
        }
        if (stem_starts_with(path, stem + "_")) {
            matches.push_back(path);
        }
    }
    return matches;
}

int strategy_score(const std::filesystem::path& path) {
    const auto stem = ascii_lower(stem_string(path));
    if (stem.ends_with("_mechanical")) {
        return 0;
    }
    if (stem.ends_with("_organic")) {
        return 1;
    }
    return 2;
}

std::optional<std::filesystem::path> choose_best_match(std::vector<std::filesystem::path> matches) {
    if (matches.empty()) {
        return std::nullopt;
    }

    std::sort(matches.begin(), matches.end(), [](const auto& lhs, const auto& rhs) {
        const auto lhsScore = strategy_score(lhs);
        const auto rhsScore = strategy_score(rhs);
        if (lhsScore != rhsScore) {
            return lhsScore < rhsScore;
        }

        const auto lhsTime = modified_time(lhs);
        const auto rhsTime = modified_time(rhs);
        if (lhsTime.has_value() && rhsTime.has_value() && *lhsTime != *rhsTime) {
            return *lhsTime > *rhsTime;
        }
        if (lhsTime.has_value() != rhsTime.has_value()) {
            return lhsTime.has_value();
        }

        return path_text(lhs) < path_text(rhs);
    });
    return matches.front();
}

std::optional<std::filesystem::path> find_step_for_local_stl(
    const std::filesystem::path& cropStpDir,
    const std::string& stem) {
    const auto exactStp = with_stem_suffix(cropStpDir, stem, "", ".stp");
    if (regular_file_exists(exactStp)) {
        return exactStp;
    }

    const auto mechanicalStp = with_stem_suffix(cropStpDir, stem, "_mechanical", ".stp");
    if (regular_file_exists(mechanicalStp)) {
        return mechanicalStp;
    }

    const auto organicStp = with_stem_suffix(cropStpDir, stem, "_organic", ".stp");
    if (regular_file_exists(organicStp)) {
        return organicStp;
    }

    if (const auto wildcardStp = choose_best_match(matching_wildcard_steps(cropStpDir, stem, ".stp"))) {
        return wildcardStp;
    }

    const auto exactStep = with_stem_suffix(cropStpDir, stem, "", ".step");
    if (regular_file_exists(exactStep)) {
        return exactStep;
    }

    return choose_best_match(matching_wildcard_steps(cropStpDir, stem, ".step"));
}

std::optional<std::filesystem::path> find_iges_fallback(
    const std::filesystem::path& cropIgsDir,
    const std::string& stem) {
    const auto exactIgs = with_stem_suffix(cropIgsDir, stem, "", ".igs");
    if (regular_file_exists(exactIgs)) {
        return exactIgs;
    }

    if (const auto wildcardIgs = choose_best_match(matching_wildcard_steps(cropIgsDir, stem, ".igs"))) {
        return wildcardIgs;
    }

    const auto exactIges = with_stem_suffix(cropIgsDir, stem, "", ".iges");
    if (regular_file_exists(exactIges)) {
        return exactIges;
    }

    return choose_best_match(matching_wildcard_steps(cropIgsDir, stem, ".iges"));
}

void attach_sidecars(PatchArtifactPaths& result) {
    if (!result.patchStepPath.empty()) {
        const auto sidecar = result.patchStepPath.parent_path() / (stem_string(result.patchStepPath) + "_autosurface.igs");
        if (regular_file_exists(sidecar)) {
            result.patchIgesSidecarPath = sidecar;
            result.foundIgesSidecar = true;
        }

        const auto log = result.patchStepPath.parent_path() / (stem_string(result.patchStepPath) + "_fit_region.log");
        if (regular_file_exists(log)) {
            result.fitRegionLogPath = log;
            result.foundFitLog = true;
        }
    }
}

void attach_result_fallback_sidecars(PatchArtifactPaths& paths, const GeomagicAutoSurfaceResult& result) {
    if (!paths.foundIgesSidecar) {
        if (regular_file_exists(result.preservedIgesPath)) {
            paths.patchIgesSidecarPath = result.preservedIgesPath;
            paths.foundIgesSidecar = true;
        } else if (regular_file_exists(result.outputIgesPath)) {
            paths.patchIgesSidecarPath = result.outputIgesPath;
            paths.foundIgesSidecar = true;
        }
    }
    if (!paths.foundFitLog && regular_file_exists(result.fitRegionLogPath)) {
        paths.fitRegionLogPath = result.fitRegionLogPath;
        paths.foundFitLog = true;
    }
}

}

PatchArtifactPaths PatchArtifactLocator::locateFromResult(const GeomagicAutoSurfaceResult& result) const {
    PatchArtifactPaths paths;
    paths.localStlPath = result.inputStlPath;

    if (regular_file_exists(result.outputStepPath)) {
        paths.patchStepPath = result.outputStepPath;
        paths.foundStep = true;
        paths.success = true;
        paths.message = "Located Geomagic STEP patch from result.";
        attach_sidecars(paths);
        attach_result_fallback_sidecars(paths, result);
        return paths;
    }

    attach_result_fallback_sidecars(paths, result);
    if (paths.foundIgesSidecar) {
        paths.success = true;
        paths.message = "Located Geomagic IGES fallback from result.";
        return paths;
    }

    paths.message = "Geomagic result does not reference an existing STEP or IGES patch.";
    return paths;
}

PatchArtifactPaths PatchArtifactLocator::locateFromLocalStl(const std::filesystem::path& localStlPath) const {
    const auto input = normalized_absolute(localStlPath);
    if (extension_key(input) != ".stl") {
        return failed(input, "Patch artifact lookup requires a .stl local crop path.");
    }
    if (!regular_file_exists(input)) {
        return failed(input, "Local crop STL does not exist.");
    }

    std::filesystem::path cropStlRoot;
    std::filesystem::path dataRoot;
    if (!find_crop_stl_root(input, cropStlRoot, dataRoot) || !starts_with_path(input, cropStlRoot)) {
        return failed(input, "Local crop STL is not under a data/crop_stl directory.");
    }

    const auto relativePath = input.lexically_relative(cropStlRoot);
    const auto relativeDir = relativePath.parent_path();
    const auto stem = stem_string(input);
    const auto cropStpDir = dataRoot / "crop_stp" / relativeDir;
    const auto cropIgsDir = dataRoot / "crop_igs" / relativeDir;

    PatchArtifactPaths result;
    result.localStlPath = input;

    if (const auto step = find_step_for_local_stl(cropStpDir, stem)) {
        result.patchStepPath = *step;
        result.foundStep = true;
        result.success = true;
        result.message = "Located Geomagic STEP patch from local STL.";
        attach_sidecars(result);
        return result;
    }

    if (const auto iges = find_iges_fallback(cropIgsDir, stem)) {
        result.patchIgesSidecarPath = *iges;
        result.foundIgesSidecar = true;
        result.success = true;
        result.message = "Located Geomagic IGES fallback from local STL.";
        return result;
    }

    result.message = "No Geomagic STEP or IGES patch was found for local STL: " + path_text(input);
    return result;
}

}
