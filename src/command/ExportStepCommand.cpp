#include "command/ExportStepCommand.h"

#include "command/CommandContext.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"

#include <utility>

namespace spo {

ExportStepCommand::ExportStepCommand(std::filesystem::path path) : path_(std::move(path)) {}

const char* ExportStepCommand::name() const {
    return "ExportStepCommand";
}

Result ExportStepCommand::execute(CommandContext& context) {
    StepWriter writer;
    const auto writeResult = writer.write(context.document, path_);
    if (!writeResult.success()) {
        return writeResult;
    }

    StepReader reader;
    const auto readResult = reader.read(path_);
    return readResult.status;
}

}
