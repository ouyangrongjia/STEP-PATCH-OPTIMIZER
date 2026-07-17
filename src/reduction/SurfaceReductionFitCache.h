#pragma once

#include "common/GeometryTypes.h"
#include "reduction/RestrictedBSplinePatchFitter.h"
#include "reduction/SurfaceReductionEngine.h"

#include <optional>
#include <string>
#include <vector>

namespace spo {

struct SurfaceReductionFitCacheEntry {
    bool success = false;
    TopoDS_Face face;
    Handle(Geom_Surface) surface;
    RestrictedBSplinePatchFitReport report;
};

class SurfaceReductionFitCache {
public:
    explicit SurfaceReductionFitCache(SurfaceReductionExecutionOptions execution);

    std::string key(
        const std::vector<FaceId>& faces,
        const RestrictedBSplinePatchFitOptions& options) const;
    std::optional<SurfaceReductionFitCacheEntry> load(const std::string& key) const;
    bool store(const std::string& key, const SurfaceReductionFitCacheEntry& entry) const;

private:
    SurfaceReductionExecutionOptions execution_;
};

}
