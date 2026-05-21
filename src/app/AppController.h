#pragma once

#include "command/Command.h"
#include "command/CommandContext.h"
#include "command/CommandHistory.h"
#include "common/Config.h"
#include "common/GeometryTypes.h"
#include "common/Result.h"
#include "feature/FeatureEdgeDetector.h"
#include "merge/MergePlanner.h"
#include "merge/SameDomainUnifier.h"
#include "validate/ShapeValidator.h"

#include <filesystem>
#include <memory>
#include <set>
#include <vector>

namespace spo {

class AppController {
public:
    const char* applicationName() const;
    Result execute(std::unique_ptr<Command> command);
    Result undo();
    Result redo();
    bool canUndo() const;
    bool canRedo() const;
    Result openStepFile(const std::filesystem::path& path);
    Result exportStepFile(const std::filesystem::path& path);
    Result verifyStepFileReadable(const std::filesystem::path& path);
    FeatureEdgeDetectionResult detectFeatureEdges(double angularThresholdDegrees, double minEdgeLength = 0.0);
    MergePlannerResult previewMergeCandidates(
        double angularThresholdDegrees,
        double minEdgeLength,
        const MergePlannerOptions& options);
    SameDomainUnifyResult unifySameDomain(
        double angularThresholdDegrees,
        double minEdgeLength,
        double linearTolerance,
        bool concatBsplines);
    bool hasDocument() const;
    const ShapeDocument& document() const;
    const FeatureEdgeDetectionResult& featureEdges() const;
    ShapeValidationReport validateShape();
    Result lockEdges(const std::vector<EdgeId>& edgeIds);
    Result unlockEdges(const std::vector<EdgeId>& edgeIds);
    std::set<EdgeId> lockedEdges() const;
    const CommandHistory& history() const;

private:
    CommandContext context_;
    CommandHistory history_;
};

}
