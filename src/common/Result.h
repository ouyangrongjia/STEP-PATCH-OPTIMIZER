#pragma once

#include <string>

namespace spo {

class Result {
public:
    static Result ok() { return Result(true, {}); }
    static Result error(std::string message) { return Result(false, std::move(message)); }

    bool success() const { return success_; }
    const std::string& message() const { return message_; }

private:
    Result(bool success, std::string message) : success_(success), message_(std::move(message)) {}

    bool success_ = false;
    std::string message_;
};

}
