#include "reduction/region_refit/RegionalSurfaceRefitEngine.h"

#include "reduction/region_refit/RegionConstraintExtractor.h"

#include <chrono>

namespace spo {

RegionRefitResult RegionalSurfaceRefitEngine::run(
    const ShapeDocument& document,
    const RegionRefitRequest& request) const {
    RegionRefitResult result;
    result.report.inputFaceCount = static_cast<int>(request.selectedFaces.size());

    if (request.options.maximumWallClockMilliseconds <= 0 ||
        request.options.constraintsBudgetMilliseconds <= 0 ||
        request.options.maximumMemoryMiB == 0 ||
        request.options.maximumSampleTriangles == 0 ||
        request.options.maximumFullFitCount <= 0 ||
        request.options.maximumAdaptivePatchCount <= 0 ||
        (request.layoutMode == RegionRefitLayoutMode::FixedPatchCount &&
            request.options.fixedPatchCount <= 0)) {
        result.report.status = RegionRefitStatus::InvalidRequest;
        result.report.failureStage = RegionRefitStage::Constraints;
        result.report.message = "Region refit resource and layout budgets must be positive.";
        return result;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto extraction = RegionConstraintExtractor().extract(document, request);
    const auto ended = std::chrono::steady_clock::now();
    RegionRefitStageReport stage;
    stage.stage = RegionRefitStage::Constraints;
    stage.success = extraction.success();
    stage.wallMilliseconds = std::chrono::duration<double, std::milli>(ended - started).count();
    stage.message = extraction.message;
    result.report.stages.push_back(stage);
    result.report.totalWallMilliseconds = stage.wallMilliseconds;

    if (!extraction.success()) {
        result.report.status = extraction.status;
        result.report.failureStage = RegionRefitStage::Constraints;
        result.report.message = extraction.message;
        return result;
    }
    if (stage.wallMilliseconds > static_cast<double>(request.options.constraintsBudgetMilliseconds) ||
        stage.wallMilliseconds > static_cast<double>(request.options.maximumWallClockMilliseconds)) {
        result.report.status = RegionRefitStatus::TimeBudgetExceeded;
        result.report.failureStage = RegionRefitStage::Constraints;
        result.report.message = "Constraint extraction exceeded its wall-clock budget.";
        return result;
    }

    result.constraints = extraction.constraints;
    result.report.inputFaceCount = static_cast<int>(result.constraints.selectedFaces.size());
    result.report.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
    result.report.failureStage = RegionRefitStage::Layout;
    result.report.message = "Constraint extraction passed; sampling and layout are not complete yet.";
    return result;
}

}
