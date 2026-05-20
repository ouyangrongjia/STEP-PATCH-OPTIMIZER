#pragma once

#include "command/Command.h"

namespace spo {

class DetectFeatureCommand final : public Command {
public:
    DetectFeatureCommand(double angularThresholdDegrees, double minEdgeLength = 0.0);
    const char* name() const override;
    Result execute(CommandContext& context) override;

private:
    double angularThresholdDegrees_ = 25.0;
    double minEdgeLength_ = 0.0;
};

}
