#pragma once

#include "io/StepEntityStatistics.h"

#include <cstdint>
#include <string>

namespace spo {

struct StepFaceLineageReport {
    std::uint64_t originalFaceCount = 0;
    std::uint64_t replacedFaceCount = 0;
    std::uint64_t replacementFaceCount = 0;
    std::uint64_t expectedWholeFaceCount = 0;
    std::uint64_t replacementStepAdvancedFaceCount = 0;
    std::uint64_t wholeStepAdvancedFaceCount = 0;
    std::uint64_t replacementUniqueSurfaceCount = 0;
    std::uint64_t wholeUniqueSurfaceCount = 0;
    bool replacementCountPassed = false;
    bool wholeCountPassed = false;
    bool passed = false;
    std::string failureReason;
};

StepFaceLineageReport makeStepFaceLineageReport(
    std::uint64_t originalFaceCount,
    std::uint64_t replacedFaceCount,
    std::uint64_t replacementFaceCount,
    const StepEntityStatistics& replacementStep,
    const StepEntityStatistics& wholeStep);

}
