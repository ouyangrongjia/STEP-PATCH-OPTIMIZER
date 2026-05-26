#pragma once

#include "feature/FeatureEdgeDetector.h"
#include "merge/MergeCandidate.h"

#include <set>
#include <vector>

namespace spo {

class ShapeDocument;

struct MergePlannerOptions {
    double max_normal_angle_degrees = 5.0;
    double max_plane_distance = 0.01;
    double max_cylinder_axis_angle_degrees = 5.0;
    double max_cylinder_axis_position_delta = 0.02;
    double max_cylinder_radius_delta = 0.01;
    double max_cylinder_fit_error = 0.02;
    double max_sphere_center_delta = 0.02;
    double max_sphere_radius_delta = 0.01;
    double max_cone_axis_angle_degrees = 5.0;
    double max_cone_semi_angle_delta_degrees = 3.0;
    double max_cone_apex_delta = 0.05;
    double max_cone_fit_error = 0.02;
    double max_torus_axis_angle_degrees = 5.0;
    double max_torus_center_delta = 0.05;
    double max_torus_major_radius_delta = 0.03;
    double max_torus_minor_radius_delta = 0.02;
    int min_region_faces = 2;
    int min_analytic_region_faces = 2;

    bool enable_plane_candidates = true;
    bool enable_cylinder_candidates = false;
    bool enable_sphere_candidates = false;
    bool enable_cone_candidates = false;
    bool enable_torus_candidates = false;
    bool enable_freeform_candidates = false;
};

struct MergePlannerResult {
    std::vector<MergeCandidate> candidates;
    int visited_faces = 0;
    int rejected_regions = 0;
    int protected_edge_count = 0;
};

class MergePlanner {
public:
    MergePlannerResult plan(
        const ShapeDocument& document,
        const FeatureEdgeDetectionResult& featureEdges,
        const std::set<EdgeId>& lockedEdges,
        const MergePlannerOptions& options) const;
};

}
