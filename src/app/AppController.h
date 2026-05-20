#pragma once

#include "command/Command.h"
#include "command/CommandContext.h"
#include "command/CommandHistory.h"
#include "common/Config.h"
#include "common/Result.h"
#include "feature/FeatureEdgeDetector.h"
#include "merge/SameDomainUnifier.h"
#include "validate/ShapeValidator.h"

#include <filesystem>
#include <memory>

namespace spo {

class AppController {
public:
    const char* applicationName() const;
    Result execute(std::unique_ptr<Command> command);
    Result openStepFile(const std::filesystem::path& path);
    Result exportStepFile(const std::filesystem::path& path);
    Result verifyStepFileReadable(const std::filesystem::path& path);
    FeatureEdgeDetectionResult detectFeatureEdges(double angularThresholdDegrees, double minEdgeLength = 0.0);
    SameDomainUnifyResult unifySameDomain(
        double angularThresholdDegrees,
        double minEdgeLength,
        double linearTolerance);
    bool hasDocument() const;
    const ShapeDocument& document() const;
    ShapeValidationReport validateShape();
    const CommandHistory& history() const;

private:
    CommandContext context_;
    CommandHistory history_;
};

}
