#include "app/AppController.h"

#include "command/DetectFeatureCommand.h"
#include "command/ExportStepCommand.h"
#include "command/LoadStepCommand.h"
#include "command/MergePatchCommand.h"
#include "command/ValidateShapeCommand.h"

namespace spo {

const char* AppController::applicationName() const {
    return kApplicationName;
}

Result AppController::execute(std::unique_ptr<Command> command) {
    if (command == nullptr) {
        return Result::error("无效命令。");
    }

    const auto commandName = command->name();
    auto result = command->execute(context_);
    if (result.success()) {
        history_.record(commandName);
    }
    return result;
}

Result AppController::openStepFile(const std::filesystem::path& path) {
    return execute(std::make_unique<LoadStepCommand>(path));
}

Result AppController::exportStepFile(const std::filesystem::path& path) {
    return execute(std::make_unique<ExportStepCommand>(path));
}

Result AppController::verifyStepFileReadable(const std::filesystem::path& path) {
    CommandContext readContext;
    LoadStepCommand command(path);
    return command.execute(readContext);
}

FeatureEdgeDetectionResult AppController::detectFeatureEdges(double angularThresholdDegrees, double minEdgeLength) {
    const auto result = execute(std::make_unique<DetectFeatureCommand>(angularThresholdDegrees, minEdgeLength));
    if (!result.success()) {
        return {};
    }
    return context_.featureEdges;
}

SameDomainUnifyResult AppController::unifySameDomain(
    double angularThresholdDegrees,
    double minEdgeLength,
    double linearTolerance) {
    auto command = std::make_unique<MergePatchCommand>(angularThresholdDegrees, minEdgeLength, linearTolerance);
    auto* commandPtr = command.get();
    const auto commandName = commandPtr->name();
    const auto status = commandPtr->execute(context_);
    if (!status.success()) {
        return {};
    }
    history_.record(commandName);
    return commandPtr->result();
}

bool AppController::hasDocument() const {
    return context_.document.hasShape();
}

const ShapeDocument& AppController::document() const {
    return context_.document;
}

ShapeValidationReport AppController::validateShape() {
    const auto result = execute(std::make_unique<ValidateShapeCommand>());
    if (!result.success()) {
        return {};
    }
    return context_.validationReport;
}

const CommandHistory& AppController::history() const {
    return history_;
}

}
