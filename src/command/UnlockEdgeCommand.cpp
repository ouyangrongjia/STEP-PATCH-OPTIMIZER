#include "command/UnlockEdgeCommand.h"

#include "command/CommandContext.h"

#include <algorithm>
#include <utility>

namespace spo {

UnlockEdgeCommand::UnlockEdgeCommand(std::vector<EdgeId> edgeIds) : edgeIds_(std::move(edgeIds)) {}

UnlockEdgeCommand::UnlockEdgeCommand(EdgeId edgeId) : edgeIds_({edgeId}) {}

const char* UnlockEdgeCommand::name() const {
    return "UnlockEdgeCommand";
}

Result UnlockEdgeCommand::execute(CommandContext& context) {
    if (edgeIds_.empty()) {
        return Result::error("未选择需要解锁的边。");
    }

    beforeLockedEdges_ = context.lockedEdges;
    std::sort(edgeIds_.begin(), edgeIds_.end());
    edgeIds_.erase(std::unique(edgeIds_.begin(), edgeIds_.end()), edgeIds_.end());

    context.lockedEdges.erase(
        std::remove_if(
            context.lockedEdges.begin(),
            context.lockedEdges.end(),
            [this](const LockedEdgeRef& ref) {
                return std::find(edgeIds_.begin(), edgeIds_.end(), ref.edgeId) != edgeIds_.end();
            }),
        context.lockedEdges.end());

    afterLockedEdges_ = context.lockedEdges;
    context.dirty = true;
    return Result::ok();
}

bool UnlockEdgeCommand::undoable() const {
    return true;
}

Result UnlockEdgeCommand::undo(CommandContext& context) {
    context.lockedEdges = beforeLockedEdges_;
    context.dirty = true;
    return Result::ok();
}

Result UnlockEdgeCommand::redo(CommandContext& context) {
    context.lockedEdges = afterLockedEdges_;
    context.dirty = true;
    return Result::ok();
}

}
