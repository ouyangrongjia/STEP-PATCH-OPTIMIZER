#pragma once

#include "brep/ShapeDocument.h"
#include "common/Result.h"

#include <filesystem>

namespace spo {

class StepWriter {
public:
    Result write(const ShapeDocument& document, const std::filesystem::path& path) const;
};

}
