#pragma once

#include "command/Command.h"
#include "command/LockedEdgeRef.h"
#include "merge/SameDomainUnifier.h"

#include <vector>

namespace spo {

class MergePatchCommand final : public Command {
public:
    MergePatchCommand(
        double angularThresholdDegrees,
        double minEdgeLength,
        double linearTolerance,
        bool concatBsplines);
    const char* name() const override;
    Result execute(CommandContext& context) override;
    bool undoable() const override;
    Result undo(CommandContext& context) override;
    Result redo(CommandContext& context) override;
    const SameDomainUnifyResult& result() const;

private:
    double angularThresholdDegrees_ = 25.0;
    double minEdgeLength_ = 0.0;
    double linearTolerance_ = 0.001;
    bool concatBsplines_ = false;
    ShapeDocument beforeDocument_;
    ShapeDocument afterDocument_;
    std::vector<LockedEdgeRef> afterLockedEdges_;
    SameDomainUnifyResult result_;
};

}
