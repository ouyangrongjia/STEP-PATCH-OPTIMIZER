#pragma once

#include <string_view>

namespace spo {

class Logger {
public:
    void info(std::string_view) {}
    void warning(std::string_view) {}
    void error(std::string_view) {}
};

}
