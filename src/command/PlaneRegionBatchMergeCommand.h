#pragma once

#include "command/Command.h"
#include "merge/PlaneRegionMerger.h"

#include <vector>

namespace spo {

class PlaneRegionBatchMergeCommand final : public Command {
public:
    PlaneRegionBatchMergeCommand(
        std::vector<MergeCandidate> candidates,
        PlaneRegionMergeOptions options,
        RegionMergeResult* resultOut = nullptr);

    const char* name() const override;
    Result execute(CommandContext& context) override;
    bool undoable() const override;
    Result undo(CommandContext& context) override;
    Result redo(CommandContext& context) override;
    const RegionMergeResult& result() const;

private:
    std::vector<MergeCandidate> candidates_;
    PlaneRegionMergeOptions options_;
    RegionMergeResult* resultOut_ = nullptr;
    ShapeDocument beforeDocument_;
    ShapeDocument afterDocument_;
    RegionMergeResult result_;
};

}
