#pragma once

#include <cstdint>

namespace spo {

struct RegionProcessResourceSnapshot {
    std::uint64_t workingSetBytes = 0;
    std::uint64_t peakWorkingSetBytes = 0;
    std::uint64_t privateBytes = 0;
    std::uint64_t peakPrivateBytes = 0;
    double processCpuMilliseconds = 0.0;
};

RegionProcessResourceSnapshot captureRegionProcessResources();

}
