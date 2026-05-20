#pragma once

#include "command/Command.h"

#include <filesystem>

namespace spo {

class ExportStepCommand final : public Command {
public:
    explicit ExportStepCommand(std::filesystem::path path);
    const char* name() const override;
    Result execute(CommandContext& context) override;

private:
    std::filesystem::path path_;
};

}
