#pragma once

#include "external/geomagic/GeomagicAutoSurfaceConfig.h"
#include "external/geomagic/GeomagicAutoSurfaceResult.h"

namespace spo {

class GeomagicAutoSurfaceBackend {
public:
    GeomagicAutoSurfaceResult run(const GeomagicAutoSurfaceConfig& config) const;
};

}
