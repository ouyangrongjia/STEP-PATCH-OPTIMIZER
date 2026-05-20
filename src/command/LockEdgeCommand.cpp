#include "command/LockEdgeCommand.h"

#include "command/CommandContext.h"
#include "command/LockedEdgeRef.h"

#include <algorithm>
#include <utility>

namespace spo {

LockEdgeCommand::LockEdgeCommand(std::vector<EdgeId> edgeIds) : edgeIds_(std::move(edgeIds)) {}

LockEdgeCommand::LockEdgeCommand(EdgeId edgeId) : edgeIds_({edgeId}) {}

const char* LockEdgeCommand::name() const {
    return "LockEdgeCommand";
}

Result LockEdgeCommand::execute(CommandContext& context) {
    if (edgeIds_.empty()) {
        return Result::error("未选择需要锁定的边。");
    }
    if (!context.document.hasShape()) {
        return Result::error("当前没有已加载的模型。");
    }

    beforeLockedEdges_ = context.lockedEdges;
    std::sort(edgeIds_.begin(), edgeIds_.end());
    edgeIds_.erase(std::unique(edgeIds_.begin(), edgeIds_.end()), edgeIds_.end());

    auto lockedIds = lockedEdgeIds(context.lockedEdges);
    const auto& topology = context.document.topology();
    for (const auto edgeId : edgeIds_) {
        if (edgeId >= topology.edgeCount()) {
            return Result::error("锁定边 ID 超出当前模型拓扑范围。");
        }
        if (lockedIds.contains(edgeId)) {
            continue;
        }
        auto ref = makeLockedEdgeRef(topology, edgeId);
        if (!ref.has_value()) {
            return Result::error("无法生成冻结边几何签名。");
        }
        context.lockedEdges.push_back(*ref);
        lockedIds.insert(edgeId);
    }

    afterLockedEdges_ = context.lockedEdges;
    context.dirty = true;
    return Result::ok();
}

bool LockEdgeCommand::undoable() const {
    return true;
}

Result LockEdgeCommand::undo(CommandContext& context) {
    context.lockedEdges = beforeLockedEdges_;
    context.dirty = true;
    return Result::ok();
}

Result LockEdgeCommand::redo(CommandContext& context) {
    context.lockedEdges = afterLockedEdges_;
    context.dirty = true;
    return Result::ok();
}

}
