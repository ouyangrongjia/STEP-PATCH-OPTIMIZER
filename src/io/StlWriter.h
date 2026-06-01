#pragma once

#include "stl/StlMesh.h"

#include <filesystem>
#include <string>

namespace spo {

struct StlWriteResult {
    bool success = false;
    std::string message;
};

class StlWriter {
public:
    StlWriteResult write(const StlMesh& mesh, const std::filesystem::path& path) const;
};

}
