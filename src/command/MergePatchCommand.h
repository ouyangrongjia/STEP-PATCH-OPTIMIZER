#pragma once

#include "command/Command.h"
#include "merge/SameDomainUnifier.h"

namespace spo {

class MergePatchCommand final : public Command {
public:
    MergePatchCommand(double angularThresholdDegrees, double minEdgeLength, double linearTolerance);
    const char* name() const override;
    Result execute(CommandContext& context) override;
    const SameDomainUnifyResult& result() const;

private:
    double angularThresholdDegrees_ = 25.0;
    double minEdgeLength_ = 0.0;
    double linearTolerance_ = 0.001;
    ShapeDocument beforeDocument_;
    ShapeDocument afterDocument_;
    SameDomainUnifyResult result_;
};

}
