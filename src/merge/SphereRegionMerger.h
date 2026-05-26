#pragma once

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionMergeOptions.h"
#include "merge/RegionMergeResult.h"

#include <vector>

namespace spo {

struct SphereRegionMergeOptions : RegionMergeOptions {
    double sphere_radius_tolerance = 0.02;
    double sphere_center_tolerance = 0.05;
    double max_normal_angle_degrees = 10.0;
    double sphere_fit_tolerance = 0.02;
    bool reject_pole_risk = true;
    bool reject_seam_risk = true;
};

class SphereRegionMerger {
public:
    RegionMergeResult merge(
        const ShapeDocument& document,
        const MergeCandidate& candidate,
        const SphereRegionMergeOptions& options) const;

    RegionMergeResult merge(
        const ShapeDocument& document,
        const MergeCandidate& candidate,
        const RegionMergeOptions& options) const;

    RegionMergeResult mergeBatch(
        const ShapeDocument& document,
        const std::vector<MergeCandidate>& candidates,
        const SphereRegionMergeOptions& options) const;
};

}
