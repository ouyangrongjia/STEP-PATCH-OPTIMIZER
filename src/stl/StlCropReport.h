#pragma once

#include "stl/StlMesh.h"

#include <string>

namespace spo {

struct StlCropReport {
    bool success = false;
    std::string message;

    int candidate_id = -1;
    int source_triangle_count = 0;
    int output_triangle_count = 0;

    StlBoundingBox candidate_bbox;
    StlBoundingBox expanded_bbox;
    StlBoundingBox output_bbox;

    double margin = 0.0;
};

}
