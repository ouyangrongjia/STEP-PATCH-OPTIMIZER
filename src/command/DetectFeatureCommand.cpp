#include "command/DetectFeatureCommand.h"

#include "command/CommandContext.h"
#include "feature/FeatureEdgeDetector.h"

namespace spo {

DetectFeatureCommand::DetectFeatureCommand(double angularThresholdDegrees, double minEdgeLength)
    : angularThresholdDegrees_(angularThresholdDegrees), minEdgeLength_(minEdgeLength) {}

const char* DetectFeatureCommand::name() const {
    return "DetectFeatureCommand";
}

Result DetectFeatureCommand::execute(CommandContext& context) {
    if (!context.document.hasShape()) {
        context.featureEdges = {};
        return Result::error("当前没有已加载的模型。");
    }

    FeatureEdgeDetector detector;
    context.featureEdges = detector.detect(context.document.topology(), angularThresholdDegrees_, minEdgeLength_);
    return Result::ok();
}

}
