#include "reduction/region_refit/RegionRefitResourceMonitor.h"

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#else
#include <ctime>
#endif

namespace spo {

RegionProcessResourceSnapshot captureRegionProcessResources() {
    RegionProcessResourceSnapshot snapshot;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        snapshot.workingSetBytes = counters.WorkingSetSize;
        snapshot.peakWorkingSetBytes = counters.PeakWorkingSetSize;
        snapshot.privateBytes = counters.PrivateUsage;
        snapshot.peakPrivateBytes = counters.PrivateUsage;
    }
    FILETIME created {};
    FILETIME exited {};
    FILETIME kernel {};
    FILETIME user {};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER kernelTicks {};
        kernelTicks.LowPart = kernel.dwLowDateTime;
        kernelTicks.HighPart = kernel.dwHighDateTime;
        ULARGE_INTEGER userTicks {};
        userTicks.LowPart = user.dwLowDateTime;
        userTicks.HighPart = user.dwHighDateTime;
        snapshot.processCpuMilliseconds =
            static_cast<double>(kernelTicks.QuadPart + userTicks.QuadPart) / 10000.0;
    }
#else
    snapshot.processCpuMilliseconds =
        static_cast<double>(std::clock()) * 1000.0 / CLOCKS_PER_SEC;
#endif
    return snapshot;
}

}
