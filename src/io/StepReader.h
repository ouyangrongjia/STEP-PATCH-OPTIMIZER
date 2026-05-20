#pragma once

#include "common/Result.h"
#include "brep/ShapeDocument.h"

#include <filesystem>

namespace spo {

struct StepReadResult {
    Result status = Result::error("尚未开始。");
    ShapeDocument document;
};

class StepReader {
public:
    Result canRead(const std::filesystem::path& path) const;
    StepReadResult read(const std::filesystem::path& path) const;
};

}
