#pragma once

#include "brep/ShapeDocument.h"
#include "command/LockedEdgeRef.h"
#include "common/GeometryTypes.h"
#include "feature/FeatureEdgeDetector.h"
#include "validate/ShapeValidator.h"

#include <filesystem>
#include <vector>

namespace spo {

struct CommandContext {
    ShapeDocument document;
    FeatureEdgeDetectionResult featureEdges;
    ShapeValidationReport validationReport;
    std::filesystem::path sourcePath;
    std::vector<LockedEdgeRef> lockedEdges;
    bool dirty = false;
};

}
