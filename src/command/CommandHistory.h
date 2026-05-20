#pragma once

#include <string>
#include <vector>

namespace spo {

class CommandHistory {
public:
    void record(std::string commandName);
    const std::vector<std::string>& executedCommands() const;
    void clear();

private:
    std::vector<std::string> executedCommands_;
};

}
