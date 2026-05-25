#include "command/PlaneRegionBatchMergeCommand.h"

#include "command/CommandContext.h"

#include <utility>

namespace spo {

PlaneRegionBatchMergeCommand::PlaneRegionBatchMergeCommand(
    std::vector<MergeCandidate> candidates,
    PlaneRegionMergeOptions options,
    RegionMergeResult* resultOut)
    : candidates_(std::move(candidates)), options_(options), resultOut_(resultOut) {}

const char* PlaneRegionBatchMergeCommand::name() const {
    return "PlaneRegionBatchMergeCommand";
}

Result PlaneRegionBatchMergeCommand::execute(CommandContext& context) {
    if (!context.document.hasShape()) {
        result_ = {};
        result_.failure_reason = RegionMergeFailureReason::NotSupported;
        result_.message = "No loaded shape for PlaneRegionBatchMergeCommand.";
        if (resultOut_ != nullptr) {
            *resultOut_ = result_;
        }
        return Result::error(result_.message);
    }

    beforeDocument_ = context.document;
    PlaneRegionMerger merger;
    result_ = merger.mergeBatch(context.document, candidates_, options_);
    if (resultOut_ != nullptr) {
        *resultOut_ = result_;
    }
    if (!result_.success) {
        return Result::error(result_.message);
    }

    afterDocument_ = result_.document;
    context.document = afterDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

bool PlaneRegionBatchMergeCommand::undoable() const {
    return true;
}

Result PlaneRegionBatchMergeCommand::undo(CommandContext& context) {
    if (!beforeDocument_.hasShape()) {
        return Result::error("No plane region batch merge state to undo.");
    }

    context.document = beforeDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

Result PlaneRegionBatchMergeCommand::redo(CommandContext& context) {
    if (!afterDocument_.hasShape()) {
        return Result::error("No plane region batch merge state to redo.");
    }

    context.document = afterDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

const RegionMergeResult& PlaneRegionBatchMergeCommand::result() const {
    return result_;
}

}
