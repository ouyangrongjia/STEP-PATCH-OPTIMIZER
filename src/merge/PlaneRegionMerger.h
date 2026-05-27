#pragma once

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionMergeOptions.h"
#include "merge/RegionMergeResult.h"

#include <filesystem>
#include <vector>

namespace spo {

struct PlaneRegionMergeOptions : RegionMergeOptions {
    double normal_angle_tolerance_degrees = 3.0;
    double plane_distance_tolerance = 0.01;
    bool allow_approximate_planar_surfaces = false;
    double approximate_plane_max_deviation = 0.01;
    std::filesystem::path roundtrip_temp_directory;
};

class PlaneRegionMerger {
public:
    RegionMergeResult merge(
        const ShapeDocument& document,
        const MergeCandidate& candidate,
        const PlaneRegionMergeOptions& options) const;
    RegionMergeResult mergeBatch(
        const ShapeDocument& document,
        const std::vector<MergeCandidate>& candidates,
        const PlaneRegionMergeOptions& options) const;
};

}
