#include "command/SphereRegionMergeCommand.h"

#include "command/CommandContext.h"

#include <utility>

namespace spo {

SphereRegionMergeCommand::SphereRegionMergeCommand(
    MergeCandidate candidate,
    SphereRegionMergeOptions options,
    RegionMergeResult* resultOut)
    : candidate_(std::move(candidate)), options_(options), resultOut_(resultOut) {}

const char* SphereRegionMergeCommand::name() const {
    return "SphereRegionMergeCommand";
}

Result SphereRegionMergeCommand::execute(CommandContext& context) {
    if (!context.document.hasShape()) {
        result_ = {};
        result_.failure_reason = RegionMergeFailureReason::NotSupported;
        result_.message = "No loaded shape for SphereRegionMergeCommand.";
        if (resultOut_ != nullptr) {
            *resultOut_ = result_;
        }
        return Result::error(result_.message);
    }

    beforeDocument_ = context.document;
    SphereRegionMerger merger;
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

bool SphereRegionMergeCommand::undoable() const {
    return true;
}

Result SphereRegionMergeCommand::undo(CommandContext& context) {
    if (!beforeDocument_.hasShape()) {
        return Result::error("No sphere region merge state to undo.");
    }

    context.document = beforeDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

Result SphereRegionMergeCommand::redo(CommandContext& context) {
    if (!afterDocument_.hasShape()) {
        return Result::error("No sphere region merge state to redo.");
    }

    context.document = afterDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

const RegionMergeResult& SphereRegionMergeCommand::result() const {
    return result_;
}

}
