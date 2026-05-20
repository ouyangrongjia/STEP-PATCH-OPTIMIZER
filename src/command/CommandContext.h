#pragma once

#include "brep/ShapeDocument.h"
#include "feature/FeatureEdgeDetector.h"
#include "validate/ShapeValidator.h"

#include <filesystem>

namespace spo {

struct CommandContext {
    ShapeDocument document;
    FeatureEdgeDetectionResult featureEdges;
    ShapeValidationReport validationReport;
    std::filesystem::path sourcePath;
    bool dirty = false;
};

}
