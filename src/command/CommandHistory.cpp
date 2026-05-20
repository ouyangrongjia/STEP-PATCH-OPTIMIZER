#include "command/CommandHistory.h"

#include <utility>

namespace spo {

void CommandHistory::record(std::string commandName) {
    executedCommands_.push_back(std::move(commandName));
}

const std::vector<std::string>& CommandHistory::executedCommands() const {
    return executedCommands_;
}

void CommandHistory::clear() {
    executedCommands_.clear();
}

}
