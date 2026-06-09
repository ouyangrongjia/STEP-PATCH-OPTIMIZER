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
    int centroid_keep_triangle_count = 0;
    int vertex_keep_triangle_count = 0;
    int edge_midpoint_keep_triangle_count = 0;
    int boundary_band_keep_triangle_count = 0;
    int conservative_keep_triangle_count = 0;
    int rejected_outside_bbox_count = 0;
    int rejected_outside_candidate_count = 0;

    StlBoundingBox candidate_bbox;
    StlBoundingBox expanded_bbox;
    StlBoundingBox output_bbox;

    double margin = 0.0;
    std::string warning_message;
};

}
