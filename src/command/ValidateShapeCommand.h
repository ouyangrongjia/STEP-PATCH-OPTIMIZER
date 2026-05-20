#pragma once

#include "command/Command.h"

namespace spo {

class ValidateShapeCommand final : public Command {
public:
    const char* name() const override;
    Result execute(CommandContext& context) override;
};

}
