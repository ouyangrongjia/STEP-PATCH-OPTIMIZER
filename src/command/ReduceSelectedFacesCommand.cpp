#include "command/ReduceSelectedFacesCommand.h"

#include "command/CommandContext.h"

#include <set>
#include <utility>

namespace spo {

namespace {

std::vector<LockedEdgeRef> remapUnchangedLockedEdges(
    const ShapeDocument& before,
    const ShapeDocument& after,
    const std::vector<LockedEdgeRef>& lockedEdges) {
    std::vector<LockedEdgeRef> remapped;
    std::set<EdgeId> used;
    for (const auto& locked : lockedEdges) {
        if (locked.edgeId >= before.topology().edgeCount()) {
            continue;
        }
        const auto& oldEdge = before.topology().edge(locked.edgeId);
        for (EdgeId edgeId = 0; edgeId < after.topology().edgeCount(); ++edgeId) {
            if (used.contains(edgeId) || !oldEdge.IsSame(after.topology().edge(edgeId))) {
                continue;
            }
            const auto reference = makeLockedEdgeRef(after.topology(), edgeId);
            if (reference.has_value()) {
                remapped.push_back(*reference);
                used.insert(edgeId);
            }
            break;
        }
    }
    return remapped;
}

} // namespace

ReduceSelectedFacesCommand::ReduceSelectedFacesCommand(SurfaceReductionResult preparedResult)
    : preparedResult_(std::move(preparedResult)) {}

const char* ReduceSelectedFacesCommand::name() const {
    return "ReduceSelectedFacesCommand";
}

Result ReduceSelectedFacesCommand::execute(CommandContext& context) {
    if (!context.document.hasShape()) {
        return Result::error("当前没有已加载的模型。");
    }
    if (!preparedResult_.canApply()) {
        return Result::error("曲面减面预览未通过全部质量门，不能执行 Apply。");
    }
    if (preparedResult_.sourceShape.IsNull() ||
        !context.document.shape().IsSame(preparedResult_.sourceShape)) {
        return Result::error("当前文档已变化，曲面减面预览已失效，请重新计算。");
    }
    beforeDocument_ = context.document;
    beforeLockedEdges_ = context.lockedEdges;
    afterDocument_ = ShapeDocument(preparedResult_.reducedShape, context.document.sourcePath());
    if (!afterDocument_.hasShape()) {
        return Result::error("曲面减面结果为空，事务未提交。");
    }
    afterLockedEdges_ = remapUnchangedLockedEdges(beforeDocument_, afterDocument_, beforeLockedEdges_);
    context.document = afterDocument_;
    context.lockedEdges = afterLockedEdges_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

bool ReduceSelectedFacesCommand::undoable() const {
    return true;
}

Result ReduceSelectedFacesCommand::undo(CommandContext& context) {
    if (!beforeDocument_.hasShape()) {
        return Result::error("没有可撤销的曲面减面事务。");
    }
    context.document = beforeDocument_;
    context.lockedEdges = beforeLockedEdges_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

Result ReduceSelectedFacesCommand::redo(CommandContext& context) {
    if (!afterDocument_.hasShape()) {
        return Result::error("没有可重做的曲面减面事务。");
    }
    context.document = afterDocument_;
    context.lockedEdges = afterLockedEdges_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

const SurfaceReductionReport& ReduceSelectedFacesCommand::report() const {
    return preparedResult_.report;
}

} // namespace spo
