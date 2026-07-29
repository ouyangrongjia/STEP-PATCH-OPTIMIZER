#include "io/StepFaceLineage.h"

namespace spo {

StepFaceLineageReport makeStepFaceLineageReport(
    const std::uint64_t originalFaceCount,
    const std::uint64_t replacedFaceCount,
    const std::uint64_t replacementFaceCount,
    const StepEntityStatistics& replacementStep,
    const StepEntityStatistics& wholeStep) {
    StepFaceLineageReport result;
    result.originalFaceCount = originalFaceCount;
    result.replacedFaceCount = replacedFaceCount;
    result.replacementFaceCount = replacementFaceCount;
    if (replacedFaceCount > originalFaceCount) {
        result.failureReason = "ReplacedFaceCountExceedsOriginal";
        return result;
    }
    result.expectedWholeFaceCount =
        originalFaceCount - replacedFaceCount + replacementFaceCount;
    result.replacementStepAdvancedFaceCount =
        replacementStep.advancedFaceCount;
    result.wholeStepAdvancedFaceCount =
        wholeStep.advancedFaceCount;
    result.replacementUniqueSurfaceCount = replacementStep.surfaceCount;
    result.wholeUniqueSurfaceCount = wholeStep.surfaceCount;
    result.replacementCountPassed =
        replacementStep.advancedFaceCount == replacementFaceCount;
    result.wholeCountPassed =
        wholeStep.advancedFaceCount == result.expectedWholeFaceCount;
    result.passed =
        result.replacementCountPassed && result.wholeCountPassed;
    if (!result.replacementCountPassed) {
        result.failureReason = "ReplacementAdvancedFaceCountMismatch";
    } else if (!result.wholeCountPassed) {
        result.failureReason = "WholeAdvancedFaceCountMismatch";
    }
    return result;
}

}
