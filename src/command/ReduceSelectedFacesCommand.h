#pragma once

#include "brep/ShapeDocument.h"
#include "command/Command.h"
#include "command/LockedEdgeRef.h"
#include "reduction/SurfaceReductionEngine.h"

#include <vector>

namespace spo {

class ReduceSelectedFacesCommand final : public Command {
public:
    explicit ReduceSelectedFacesCommand(SurfaceReductionResult preparedResult);

    const char* name() const override;
    Result execute(CommandContext& context) override;
    bool undoable() const override;
    Result undo(CommandContext& context) override;
    Result redo(CommandContext& context) override;

    const SurfaceReductionReport& report() const;

private:
    SurfaceReductionResult preparedResult_;
    ShapeDocument beforeDocument_;
    ShapeDocument afterDocument_;
    std::vector<LockedEdgeRef> beforeLockedEdges_;
    std::vector<LockedEdgeRef> afterLockedEdges_;
};

}
