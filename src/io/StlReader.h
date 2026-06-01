#pragma once

#include "stl/StlMesh.h"

#include <filesystem>
#include <string>

namespace spo {

struct StlReadResult {
    bool success = false;
    StlMesh mesh;
    std::string message;
};

class StlReader {
public:
    StlReadResult read(const std::filesystem::path& path) const;
};

}
