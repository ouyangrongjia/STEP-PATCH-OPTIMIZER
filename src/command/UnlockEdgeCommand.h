#pragma once

#include "command/Command.h"

namespace spo {

class UnlockEdgeCommand final : public Command {
public:
    const char* name() const override { return "UnlockEdgeCommand"; }
    Result execute(CommandContext&) override { return Result::ok(); }
};

}
