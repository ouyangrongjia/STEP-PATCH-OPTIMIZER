#include "command/SphereRegionBatchMergeCommand.h"

#include "command/CommandContext.h"

#include <utility>

namespace spo {

SphereRegionBatchMergeCommand::SphereRegionBatchMergeCommand(
    std::vector<MergeCandidate> candidates,
    SphereRegionMergeOptions options,
    RegionMergeResult* resultOut)
    : candidates_(std::move(candidates)), options_(options), resultOut_(resultOut) {}

const char* SphereRegionBatchMergeCommand::name() const {
    return "SphereRegionBatchMergeCommand";
}

Result SphereRegionBatchMergeCommand::execute(CommandContext& context) {
    if (!context.document.hasShape()) {
        result_ = {};
        result_.failure_reason = RegionMergeFailureReason::NotSupported;
        result_.message = "No loaded shape for SphereRegionBatchMergeCommand.";
        if (resultOut_ != nullptr) {
            *resultOut_ = result_;
        }
        return Result::error(result_.message);
    }

    beforeDocument_ = context.document;
    SphereRegionMerger merger;
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

bool SphereRegionBatchMergeCommand::undoable() const {
    return true;
}

Result SphereRegionBatchMergeCommand::undo(CommandContext& context) {
    if (!beforeDocument_.hasShape()) {
        return Result::error("No sphere region batch merge state to undo.");
    }

    context.document = beforeDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

Result SphereRegionBatchMergeCommand::redo(CommandContext& context) {
    if (!afterDocument_.hasShape()) {
        return Result::error("No sphere region batch merge state to redo.");
    }

    context.document = afterDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

const RegionMergeResult& SphereRegionBatchMergeCommand::result() const {
    return result_;
}

}
