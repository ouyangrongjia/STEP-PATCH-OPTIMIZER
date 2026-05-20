#include "command/MergePatchCommand.h"

#include "command/CommandContext.h"
#include "feature/FeatureEdgeDetector.h"

#include <vector>

namespace spo {

MergePatchCommand::MergePatchCommand(double angularThresholdDegrees, double minEdgeLength, double linearTolerance)
    : angularThresholdDegrees_(angularThresholdDegrees),
      minEdgeLength_(minEdgeLength),
      linearTolerance_(linearTolerance) {}

const char* MergePatchCommand::name() const {
    return "MergePatchCommand";
}

Result MergePatchCommand::execute(CommandContext& context) {
    if (!context.document.hasShape()) {
        result_ = {};
        return Result::error("当前没有已加载的模型。");
    }

    FeatureEdgeDetector detector;
    context.featureEdges = detector.detect(context.document.topology(), angularThresholdDegrees_, minEdgeLength_);

    std::vector<EdgeId> protectedEdges;
    protectedEdges.reserve(context.featureEdges.edges.size());
    for (const auto& edge : context.featureEdges.edges) {
        protectedEdges.push_back(edge.edge);
    }

    SameDomainUnifyOptions options;
    options.angular_tolerance_degrees = angularThresholdDegrees_;
    options.linear_tolerance = linearTolerance_;

    beforeDocument_ = context.document;
    SameDomainUnifier unifier;
    result_ = unifier.unify(context.document, protectedEdges, options);
    if (!result_.document.hasShape()) {
        return Result::error("同域合并未生成有效模型。");
    }

    afterDocument_ = result_.document;
    context.document = result_.document;
    context.dirty = true;
    context.featureEdges = detector.detect(context.document.topology(), angularThresholdDegrees_, minEdgeLength_);
    return Result::ok();
}

const SameDomainUnifyResult& MergePatchCommand::result() const {
    return result_;
}

}
