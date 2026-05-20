#include "command/CommandHistory.h"

#include "command/Command.h"

#include <memory>
#include <string>

namespace spo {

Result CommandHistory::execute(std::unique_ptr<Command> command, CommandContext& context) {
    if (command == nullptr) {
        return Result::error("无效命令。");
    }

    const std::string commandName = command->name();
    const auto result = command->execute(context);
    if (!result.success()) {
        return result;
    }

    executedCommands_.push_back(commandName);
    if (command->undoable()) {
        undoStack_.push_back(std::move(command));
        redoStack_.clear();
    }
    return Result::ok();
}

bool CommandHistory::canUndo() const {
    return !undoStack_.empty();
}

bool CommandHistory::canRedo() const {
    return !redoStack_.empty();
}

Result CommandHistory::undo(CommandContext& context) {
    if (undoStack_.empty()) {
        return Result::error("没有可撤销的命令。");
    }

    auto command = std::move(undoStack_.back());
    undoStack_.pop_back();
    const auto result = command->undo(context);
    if (!result.success()) {
        undoStack_.push_back(std::move(command));
        return result;
    }

    redoStack_.push_back(std::move(command));
    return Result::ok();
}

Result CommandHistory::redo(CommandContext& context) {
    if (redoStack_.empty()) {
        return Result::error("没有可重做的命令。");
    }

    auto command = std::move(redoStack_.back());
    redoStack_.pop_back();
    const auto result = command->redo(context);
    if (!result.success()) {
        redoStack_.push_back(std::move(command));
        return result;
    }

    undoStack_.push_back(std::move(command));
    return Result::ok();
}

const std::vector<std::string>& CommandHistory::executedCommands() const {
    return executedCommands_;
}

void CommandHistory::clear() {
    undoStack_.clear();
    redoStack_.clear();
    executedCommands_.clear();
}

}
