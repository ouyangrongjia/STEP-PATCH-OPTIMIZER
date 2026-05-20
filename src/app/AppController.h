#pragma once

#include "brep/ShapeDocument.h"
#include "common/Config.h"
#include "common/Result.h"
#include "feature/FeatureEdgeDetector.h"
#include "merge/SameDomainUnifier.h"
#include "validate/ShapeValidator.h"

#include <filesystem>

namespace spo {

class AppController {
public:
    const char* applicationName() const;
    Result openStepFile(const std::filesystem::path& path);
    Result exportStepFile(const std::filesystem::path& path) const;
    Result verifyStepFileReadable(const std::filesystem::path& path) const;
    FeatureEdgeDetectionResult detectFeatureEdges(double angularThresholdDegrees, double minEdgeLength = 0.0) const;
    SameDomainUnifyResult unifySameDomain(
        double angularThresholdDegrees,
        double minEdgeLength,
        double linearTolerance);
    bool hasDocument() const;
    const ShapeDocument& document() const;
    ShapeValidationReport validateShape() const;

private:
    ShapeDocument document_;
};

}
