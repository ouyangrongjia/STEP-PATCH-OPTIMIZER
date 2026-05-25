#pragma once

namespace spo {

struct RegionMergeOptions {
    double max_deviation = 0.02;
    int min_region_faces = 2;
    bool allow_pending_candidate = false;
    bool require_accepted_candidate = true;
};

}
