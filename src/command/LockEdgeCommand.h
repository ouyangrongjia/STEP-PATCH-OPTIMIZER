#pragma once

#include "command/Command.h"

namespace spo {

class LockEdgeCommand final : public Command {
public:
    void execute() override {}
    void undo() override {}
};

}
