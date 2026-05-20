#pragma once

#include "common/Result.h"

namespace spo {

struct CommandContext;

class Command {
public:
    virtual ~Command() = default;
    virtual const char* name() const = 0;
    virtual Result execute(CommandContext& context) = 0;
};

}
