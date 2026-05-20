#pragma once

#include "command/Command.h"

namespace spo {

class UnlockEdgeCommand final : public Command {
public:
    void execute() override {}
    void undo() override {}
};

}
