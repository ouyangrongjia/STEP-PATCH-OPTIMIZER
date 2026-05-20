#pragma once

#include "command/Command.h"

namespace spo {

class LockEdgeCommand final : public Command {
public:
    const char* name() const override { return "LockEdgeCommand"; }
    Result execute(CommandContext&) override { return Result::ok(); }
};

}
