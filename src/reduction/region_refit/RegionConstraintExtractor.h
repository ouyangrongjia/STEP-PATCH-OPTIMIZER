#pragma once

#include "brep/ShapeDocument.h"
#include "reduction/region_refit/RegionalSurfaceRefitTypes.h"

#include <string>

namespace spo {

struct RegionConstraintExtractionResult {
    RegionRefitStatus status = RegionRefitStatus::Failed;
    RegionTopologyConstraints constraints;
    std::string message;

    bool success() const;
};

class RegionConstraintExtractor {
public:
    RegionConstraintExtractionResult extract(
        const ShapeDocument& document,
        const RegionRefitRequest& request) const;
};

}
