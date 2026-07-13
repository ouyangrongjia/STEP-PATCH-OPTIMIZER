#pragma once

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"

#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>

#include <string>
#include <vector>

namespace spo {

enum class SurfaceRefitFailureReason {
    None,
    InvalidInput,
    UnsupportedCandidateType,
    BoundaryInvalid,
    SampleGenerationFailed,
    FillingFailed,
    NullFace,
    BoundaryRepresentationInvalid,
    BRepCheckFailed,
    BoundaryErrorExceeded,
    CornerErrorExceeded,
    InteriorErrorExceeded,
    NormalErrorExceeded,
    JacobianInvalid,
    ComplexityExceeded
};

const char* surfaceRefitFailureReasonToString(SurfaceRefitFailureReason reason);

enum class SurfaceRefitSampleCategory {
    Boundary,
    Corner,
    Interior
};

const char* surfaceRefitSampleCategoryToString(SurfaceRefitSampleCategory category);

struct SurfaceRefitSampleDiagnostic {
    SurfaceRefitSampleCategory category = SurfaceRefitSampleCategory::Boundary;
    gp_Pnt source_point;
    gp_Pnt projected_point;
    double distance = 0.0;
    bool projection_success = false;
    bool over_tolerance = false;
    FaceId face_id = -1;
    EdgeId edge_id = -1;
};

struct SurfaceRefitErrorStatistics {
    int sample_count = 0;
    int projection_failure_count = 0;
    int over_tolerance_count = 0;
    double max = 0.0;
    double p95 = 0.0;
    double rms = 0.0;
    gp_Pnt worst_point;
    FaceId worst_face_id = -1;
    EdgeId worst_edge_id = -1;
};

struct SurfaceRefitNormalStatistics {
    int sample_count = 0;
    int undefined_count = 0;
    int over_tolerance_count = 0;
    double max_angle_degrees = 0.0;
    double p95_angle_degrees = 0.0;
    double rms_angle_degrees = 0.0;
    gp_Pnt worst_point;
    FaceId worst_face_id = -1;
};

struct SurfaceRefitJacobianReport {
    int sample_count = 0;
    int near_singular_count = 0;
    int flipped_pair_count = 0;
    double min_magnitude = 0.0;
    double max_magnitude = 0.0;
    double min_normalized = 0.0;
    double worst_u = 0.0;
    double worst_v = 0.0;
};

struct SurfaceRefitComplexityReport {
    std::string surface_type;
    int u_degree = 0;
    int v_degree = 0;
    int u_poles = 0;
    int v_poles = 0;
    int total_poles = 0;
    int u_knots = 0;
    int v_knots = 0;
    int u_segments = 0;
    int v_segments = 0;
    int total_segments = 0;
};

struct SurfaceRefitOptions {
    double tolerance2d = 1.0e-5;
    double tolerance3d = 0.03;
    double tolerance_angle = 0.01;
    double tolerance_curvature = 0.1;
    double normal_tolerance_degrees = 15.0;
    double mesh_deflection = 0.1;
    double mesh_angular_deflection = 0.2;
    double sample_deduplication_tolerance = 1.0e-7;
    double minimum_normalized_jacobian = 1.0e-5;
    int plate_degree = 3;
    int points_on_curve = 24;
    int iterations = 3;
    int max_degree = 8;
    int max_segments = 12;
    int training_sample_count = 48;
    int validation_sample_count = 96;
    int boundary_samples_per_edge = 16;
    int jacobian_grid_size = 12;
    int max_surface_degree = 8;
    int max_surface_pole_count = 1024;
    int max_surface_knot_count = 128;
    int max_surface_segment_count = 256;
};

struct SurfaceRefitReport {
    bool success = false;
    SurfaceRefitFailureReason failure_reason = SurfaceRefitFailureReason::None;
    std::vector<SurfaceRefitFailureReason> failed_gates;
    std::string message;
    int candidate_id = -1;
    int candidate_face_count = 0;
    bool boundary_valid = false;
    bool filling_done = false;
    bool brep_check_valid = false;
    bool output_orientation_reversed = false;
    int output_edge_count = 0;
    int missing_pcurve_count = 0;
    int same_parameter_failure_count = 0;
    int requested_training_sample_count = 0;
    int requested_validation_sample_count = 0;
    int training_sample_count = 0;
    int validation_sample_count = 0;
    int training_validation_overlap_count = 0;
    double g0_error = 0.0;
    double g1_error = 0.0;
    double g2_error = 0.0;
    std::vector<EdgeId> ordered_boundary_edges;
    SurfaceRefitErrorStatistics boundary_error;
    SurfaceRefitErrorStatistics corner_error;
    SurfaceRefitErrorStatistics interior_error;
    SurfaceRefitNormalStatistics normal_error;
    SurfaceRefitJacobianReport jacobian;
    SurfaceRefitComplexityReport complexity;
    std::vector<SurfaceRefitSampleDiagnostic> surface_error_samples;
};

struct SurfaceRefitResult {
    TopoDS_Face face;
    SurfaceRefitReport report;

    bool success() const { return report.success && !face.IsNull(); }
};

class SurfaceRefitter {
public:
    SurfaceRefitResult fitSingleFace(
        const ShapeDocument& document,
        const MergeCandidate& candidate,
        const SurfaceRefitOptions& options = {}) const;
};

}
