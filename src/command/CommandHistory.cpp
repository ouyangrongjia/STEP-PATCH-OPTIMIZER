#include "command/CommandHistory.h"

#include "command/Command.h"

#include <Standard_Failure.hxx>

#include <exception>
#include <memory>
#include <string>

namespace spo {

namespace {

std::string occtMessage(const Standard_Failure& error) {
    const auto* message = error.GetMessageString();
    if (message == nullptr || std::string(message).empty()) {
        return "OCCT 操作失败。";
    }
    return message;
}

}

Result CommandHistory::execute(std::unique_ptr<Command> command, CommandContext& context) {
    if (command == nullptr) {
        return Result::error("无效命令。");
    }

    const std::string commandName = command->name();
    Result result = Result::ok();
    try {
        result = command->execute(context);
    } catch (const Standard_Failure& error) {
        return Result::error(commandName + " 失败：" + occtMessage(error));
    } catch (const std::exception& error) {
        return Result::error(commandName + " 失败：" + error.what());
    } catch (...) {
        return Result::error(commandName + " 失败：未知异常。");
    }
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
    Result result = Result::ok();
    try {
        result = command->undo(context);
    } catch (const Standard_Failure& error) {
        result = Result::error(std::string(command->name()) + " 撤销失败：" + occtMessage(error));
    } catch (const std::exception& error) {
        result = Result::error(std::string(command->name()) + " 撤销失败：" + error.what());
    } catch (...) {
        result = Result::error(std::string(command->name()) + " 撤销失败：未知异常。");
    }
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
    Result result = Result::ok();
    try {
        result = command->redo(context);
    } catch (const Standard_Failure& error) {
        result = Result::error(std::string(command->name()) + " 重做失败：" + occtMessage(error));
    } catch (const std::exception& error) {
        result = Result::error(std::string(command->name()) + " 重做失败：" + error.what());
    } catch (...) {
        result = Result::error(std::string(command->name()) + " 重做失败：未知异常。");
    }
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
