#include "command/MergePatchCommand.h"

#include "command/CommandContext.h"
#include "command/LockedEdgeRef.h"
#include "feature/FeatureEdgeDetector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

namespace spo {

namespace {

double endpointDistance(const EdgeGeometrySignature& lhs, const EdgeGeometrySignature& rhs) {
    const auto direct =
        lhs.first.SquareDistance(rhs.first) +
        lhs.last.SquareDistance(rhs.last);
    const auto reversed =
        lhs.first.SquareDistance(rhs.last) +
        lhs.last.SquareDistance(rhs.first);
    return std::sqrt(std::min(direct, reversed));
}

double signatureScore(const EdgeGeometrySignature& lhs, const EdgeGeometrySignature& rhs) {
    if (!lhs.valid || !rhs.valid) {
        return std::numeric_limits<double>::infinity();
    }
    return endpointDistance(lhs, rhs) +
        std::sqrt(lhs.middle.SquareDistance(rhs.middle)) +
        std::abs(lhs.length - rhs.length);
}

std::vector<LockedEdgeRef> remapLockedEdges(
    const ShapeDocument& beforeDocument,
    const ShapeDocument& afterDocument,
    const std::vector<LockedEdgeRef>& lockedEdges,
    double tolerance) {
    std::vector<LockedEdgeRef> remapped;
    std::set<EdgeId> usedNewEdges;
    const auto& beforeTopology = beforeDocument.topology();
    const auto& afterTopology = afterDocument.topology();
    const double maxScore = std::max(tolerance * 30.0, 1.0e-4);
    const double minAmbiguityGap = std::max(maxScore * 0.25, 1.0e-6);

    for (const auto& lockedEdge : lockedEdges) {
        if (lockedEdge.edgeId >= beforeTopology.edgeCount()) {
            continue;
        }

        const auto& oldEdge = beforeTopology.edge(lockedEdge.edgeId);
        bool mappedByShape = false;
        for (EdgeId newEdgeId = 0; newEdgeId < afterTopology.edgeCount(); ++newEdgeId) {
            if (usedNewEdges.contains(newEdgeId)) {
                continue;
            }
            if (oldEdge.IsSame(afterTopology.edge(newEdgeId))) {
                auto ref = makeLockedEdgeRef(afterTopology, newEdgeId);
                if (ref.has_value()) {
                    remapped.push_back(*ref);
                    usedNewEdges.insert(newEdgeId);
                    mappedByShape = true;
                }
                break;
            }
        }
        if (mappedByShape) {
            continue;
        }

        EdgeId bestEdgeId = 0;
        double bestScore = std::numeric_limits<double>::infinity();
        double secondBestScore = std::numeric_limits<double>::infinity();
        for (EdgeId newEdgeId = 0; newEdgeId < afterTopology.edgeCount(); ++newEdgeId) {
            if (usedNewEdges.contains(newEdgeId)) {
                continue;
            }
            const auto candidate = makeLockedEdgeRef(afterTopology, newEdgeId);
            if (!candidate.has_value()) {
                continue;
            }

            const auto score = signatureScore(lockedEdge.signature, candidate->signature);
            if (score < bestScore) {
                secondBestScore = bestScore;
                bestScore = score;
                bestEdgeId = newEdgeId;
            } else if (score < secondBestScore) {
                secondBestScore = score;
            }
        }

        const bool confident =
            bestScore <= maxScore &&
            (secondBestScore == std::numeric_limits<double>::infinity() ||
             secondBestScore - bestScore >= minAmbiguityGap);
        if (!confident) {
            continue;
        }

        auto ref = makeLockedEdgeRef(afterTopology, bestEdgeId);
        if (ref.has_value()) {
            remapped.push_back(*ref);
            usedNewEdges.insert(bestEdgeId);
        }
    }

    return remapped;
}

}

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

    const auto lockedIds = lockedEdgeIds(context.lockedEdges);
    std::vector<EdgeId> protectedEdges;
    protectedEdges.reserve(context.featureEdges.edges.size() + lockedIds.size());
    for (const auto& edge : context.featureEdges.edges) {
        protectedEdges.push_back(edge.edge);
    }
    for (const auto edgeId : lockedIds) {
        protectedEdges.push_back(edgeId);
    }
    std::sort(protectedEdges.begin(), protectedEdges.end());
    protectedEdges.erase(std::unique(protectedEdges.begin(), protectedEdges.end()), protectedEdges.end());

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
    afterLockedEdges_ = remapLockedEdges(beforeDocument_, afterDocument_, context.lockedEdges, linearTolerance_);
    context.document = afterDocument_;
    context.lockedEdges = afterLockedEdges_;
    context.dirty = true;
    context.featureEdges = detector.detect(context.document.topology(), angularThresholdDegrees_, minEdgeLength_);
    return Result::ok();
}

bool MergePatchCommand::undoable() const {
    return true;
}

Result MergePatchCommand::undo(CommandContext& context) {
    if (!beforeDocument_.hasShape()) {
        return Result::error("没有可撤销的合并前模型。");
    }

    context.document = beforeDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.lockedEdges.clear();
    context.dirty = true;
    return Result::ok();
}

Result MergePatchCommand::redo(CommandContext& context) {
    if (!afterDocument_.hasShape()) {
        return Result::error("没有可重做的合并后模型。");
    }

    context.document = afterDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.lockedEdges = afterLockedEdges_;
    context.dirty = true;
    return Result::ok();
}

const SameDomainUnifyResult& MergePatchCommand::result() const {
    return result_;
}

}
