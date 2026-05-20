#include "app/AppController.h"

#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "merge/SameDomainUnifier.h"
#include "validate/ShapeValidator.h"

#include <vector>

namespace spo {

const char* AppController::applicationName() const {
    return kApplicationName;
}

Result AppController::openStepFile(const std::filesystem::path& path) {
    StepReader reader;
    auto result = reader.read(path);
    if (!result.status.success()) {
        return result.status;
    }

    document_ = std::move(result.document);
    return Result::ok();
}

Result AppController::exportStepFile(const std::filesystem::path& path) const {
    StepWriter writer;
    return writer.write(document_, path);
}

Result AppController::verifyStepFileReadable(const std::filesystem::path& path) const {
    StepReader reader;
    const auto result = reader.read(path);
    return result.status;
}

FeatureEdgeDetectionResult AppController::detectFeatureEdges(double angularThresholdDegrees, double minEdgeLength) const {
    if (!document_.hasShape()) {
        return {};
    }

    FeatureEdgeDetector detector;
    return detector.detect(document_.topology(), angularThresholdDegrees, minEdgeLength);
}

SameDomainUnifyResult AppController::unifySameDomain(
    double angularThresholdDegrees,
    double minEdgeLength,
    double linearTolerance) {
    const auto features = detectFeatureEdges(angularThresholdDegrees, minEdgeLength);
    std::vector<EdgeId> protectedEdges;
    protectedEdges.reserve(features.edges.size());
    for (const auto& edge : features.edges) {
        protectedEdges.push_back(edge.edge);
    }

    SameDomainUnifier unifier;
    SameDomainUnifyOptions options;
    options.angular_tolerance_degrees = angularThresholdDegrees;
    options.linear_tolerance = linearTolerance;

    auto result = unifier.unify(document_, protectedEdges, options);
    if (result.document.hasShape()) {
        document_ = result.document;
    }
    return result;
}

bool AppController::hasDocument() const {
    return document_.hasShape();
}

const ShapeDocument& AppController::document() const {
    return document_;
}

ShapeValidationReport AppController::validateShape() const {
    ShapeValidator validator;
    return validator.validate(document_);
}

}
