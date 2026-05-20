#pragma once

#include "command/Command.h"
#include "command/LockedEdgeRef.h"
#include "common/GeometryTypes.h"

#include <vector>

namespace spo {

class LockEdgeCommand final : public Command {
public:
    explicit LockEdgeCommand(std::vector<EdgeId> edgeIds);
    explicit LockEdgeCommand(EdgeId edgeId);
    const char* name() const override;
    Result execute(CommandContext& context) override;
    bool undoable() const override;
    Result undo(CommandContext& context) override;
    Result redo(CommandContext& context) override;

private:
    std::vector<EdgeId> edgeIds_;
    std::vector<LockedEdgeRef> beforeLockedEdges_;
    std::vector<LockedEdgeRef> afterLockedEdges_;
};

}
