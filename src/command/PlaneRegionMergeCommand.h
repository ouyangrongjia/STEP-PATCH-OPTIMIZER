#pragma once

#include "command/Command.h"
#include "merge/PlaneRegionMerger.h"

namespace spo {

class PlaneRegionMergeCommand final : public Command {
public:
    PlaneRegionMergeCommand(
        MergeCandidate candidate,
        PlaneRegionMergeOptions options,
        RegionMergeResult* resultOut = nullptr);
    const char* name() const override;
    Result execute(CommandContext& context) override;
    bool undoable() const override;
    Result undo(CommandContext& context) override;
    Result redo(CommandContext& context) override;
    const RegionMergeResult& result() const;

private:
    MergeCandidate candidate_;
    PlaneRegionMergeOptions options_;
    RegionMergeResult* resultOut_ = nullptr;
    ShapeDocument beforeDocument_;
    ShapeDocument afterDocument_;
    RegionMergeResult result_;
};

}
