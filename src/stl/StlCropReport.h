#pragma once

#include "common/GeometryTypes.h"
#include "stl/StlMesh.h"

#include <string>
#include <vector>

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

    bool boundary_loop_coverage_evaluated = false;
    bool boundary_loop_coverage_repair_applied = false;
    int boundary_loop_sample_count = 0;
    int boundary_loop_missing_point_count_before = 0;
    int boundary_loop_missing_point_count_after = 0;
    int boundary_loop_repair_triangle_count = 0;
    int boundary_loop_orphan_repair_candidate_count = 0;
    double boundary_loop_coverage_tolerance = 0.0;
    double boundary_loop_max_distance_before = 0.0;
    double boundary_loop_max_distance_after = 0.0;
    double boundary_loop_average_distance_before = 0.0;
    double boundary_loop_average_distance_after = 0.0;
    std::vector<EdgeId> boundary_loop_missing_edge_ids_before;
    std::vector<EdgeId> boundary_loop_missing_edge_ids_after;

    StlBoundingBox candidate_bbox;
    StlBoundingBox expanded_bbox;
    StlBoundingBox output_bbox;

    double margin = 0.0;
    std::string warning_message;
};

}
