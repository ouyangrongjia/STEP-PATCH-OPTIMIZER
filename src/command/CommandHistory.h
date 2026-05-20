#pragma once

#include "common/Result.h"

#include <memory>
#include <string>
#include <vector>

namespace spo {

class Command;
struct CommandContext;

class CommandHistory {
public:
    Result execute(std::unique_ptr<Command> command, CommandContext& context);
    bool canUndo() const;
    bool canRedo() const;
    Result undo(CommandContext& context);
    Result redo(CommandContext& context);
    const std::vector<std::string>& executedCommands() const;
    void clear();

private:
    std::vector<std::unique_ptr<Command>> undoStack_;
    std::vector<std::unique_ptr<Command>> redoStack_;
    std::vector<std::string> executedCommands_;
};

}
