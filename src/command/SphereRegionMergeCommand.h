#pragma once

#include "command/Command.h"
#include "merge/SphereRegionMerger.h"

namespace spo {

class SphereRegionMergeCommand final : public Command {
public:
    SphereRegionMergeCommand(
        MergeCandidate candidate,
        SphereRegionMergeOptions options,
        RegionMergeResult* resultOut = nullptr);
    const char* name() const override;
    Result execute(CommandContext& context) override;
    bool undoable() const override;
    Result undo(CommandContext& context) override;
    Result redo(CommandContext& context) override;
    const RegionMergeResult& result() const;

private:
    MergeCandidate candidate_;
    SphereRegionMergeOptions options_;
    RegionMergeResult* resultOut_ = nullptr;
    ShapeDocument beforeDocument_;
    ShapeDocument afterDocument_;
    RegionMergeResult result_;
};

}
