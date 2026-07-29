#pragma once

#include <string_view>
#include <cstdint>

namespace spo {

struct StepEntityLineClassification {
    bool entity = false;
    bool edgeCurve = false;
    bool surface = false;
    bool advancedFace = false;
    bool bSplineSurface = false;
};

struct StepEntityStatistics {
    std::uint64_t entityCount = 0;
    std::uint64_t edgeCurveCount = 0;
    std::uint64_t surfaceCount = 0;
    std::uint64_t advancedFaceCount = 0;
    std::uint64_t bSplineSurfaceCount = 0;
};

StepEntityLineClassification classifyStepEntityLine(
    std::string_view line);

void accumulateStepEntityLine(
    StepEntityStatistics& statistics,
    std::string_view line);

}
