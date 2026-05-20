#pragma once

#include "common/Result.h"

namespace spo {

struct CommandContext;

class Command {
public:
    virtual ~Command() = default;
    virtual const char* name() const = 0;
    virtual Result execute(CommandContext& context) = 0;
    virtual bool undoable() const { return false; }
    virtual Result undo(CommandContext&) { return Result::error("该命令不支持撤销。"); }
    virtual Result redo(CommandContext&) { return Result::error("该命令不支持重做。"); }
};

}
