#pragma once

#include "merge/SurfaceRefitter.h"

#include <filesystem>
#include <string>

namespace spo {

struct SurfaceRefitDiagnosticArtifacts {
    std::filesystem::path csv_path;
    std::filesystem::path obj_path;
    bool csv_written = false;
    bool obj_written = false;
    std::string message;

    bool success() const { return csv_written && obj_written; }
};

SurfaceRefitDiagnosticArtifacts writeSurfaceRefitDiagnosticArtifacts(
    const SurfaceRefitReport& report,
    const std::filesystem::path& output_directory);

}
