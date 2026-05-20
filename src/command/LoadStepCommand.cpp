#include "command/LoadStepCommand.h"

#include "command/CommandContext.h"
#include "io/StepReader.h"

#include <utility>

namespace spo {

LoadStepCommand::LoadStepCommand(std::filesystem::path path) : path_(std::move(path)) {}

const char* LoadStepCommand::name() const {
    return "LoadStepCommand";
}

Result LoadStepCommand::execute(CommandContext& context) {
    StepReader reader;
    auto result = reader.read(path_);
    if (!result.status.success()) {
        return result.status;
    }

    context.document = std::move(result.document);
    context.sourcePath = path_;
    context.featureEdges = {};
    context.validationReport = {};
    context.lockedEdges.clear();
    context.dirty = false;
    return Result::ok();
}

}
