#pragma once

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionMergeOptions.h"
#include "merge/RegionMergeResult.h"

namespace spo {

class SphereRegionMerger {
public:
    RegionMergeResult merge(
        const ShapeDocument& document,
        const MergeCandidate& candidate,
        const RegionMergeOptions& options) const;
};

}
