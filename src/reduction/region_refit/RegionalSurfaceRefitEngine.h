#pragma once

#include "brep/ShapeDocument.h"
#include "reduction/region_refit/RegionalSurfaceRefitTypes.h"

namespace spo {

class RegionalSurfaceRefitEngine {
public:
    RegionRefitResult run(
        const ShapeDocument& document,
        const RegionRefitRequest& request) const;
};

}
