#include "command/PlaneRegionMergeCommand.h"

#include "command/CommandContext.h"

#include <utility>

namespace spo {

PlaneRegionMergeCommand::PlaneRegionMergeCommand(
    MergeCandidate candidate,
    PlaneRegionMergeOptions options,
    RegionMergeResult* resultOut)
    : candidate_(std::move(candidate)), options_(options), resultOut_(resultOut) {}

const char* PlaneRegionMergeCommand::name() const {
    return "PlaneRegionMergeCommand";
}

Result PlaneRegionMergeCommand::execute(CommandContext& context) {
    if (!context.document.hasShape()) {
        result_ = {};
        result_.failure_reason = RegionMergeFailureReason::NotSupported;
        result_.message = "No loaded shape for PlaneRegionMergeCommand.";
        if (resultOut_ != nullptr) {
            *resultOut_ = result_;
        }
        return Result::error(result_.message);
    }

    beforeDocument_ = context.document;
    PlaneRegionMerger merger;
    result_ = merger.merge(context.document, candidate_, options_);
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

bool PlaneRegionMergeCommand::undoable() const {
    return true;
}

Result PlaneRegionMergeCommand::undo(CommandContext& context) {
    if (!beforeDocument_.hasShape()) {
        return Result::error("No plane region merge state to undo.");
    }

    context.document = beforeDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

Result PlaneRegionMergeCommand::redo(CommandContext& context) {
    if (!afterDocument_.hasShape()) {
        return Result::error("No plane region merge state to redo.");
    }

    context.document = afterDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

const RegionMergeResult& PlaneRegionMergeCommand::result() const {
    return result_;
}

}
