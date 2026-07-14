#pragma once

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"

#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <string>
#include <vector>

namespace spo {

enum class SurfaceRefitFailureReason {
    None,
    InvalidInput,
    UnsupportedCandidateType,
    BoundaryInvalid,
    SampleGenerationFailed,
    SampleSetOverlap,
    SamplingConfigurationUnsupported,
    LocalG1ConstraintInvalid,
    FillingFailed,
    NullFace,
    BoundaryRepresentationInvalid,
    BRepCheckFailed,
    StepRoundtripInvalid,
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

enum class SurfaceRefitSampleSet {
    Boundary,
    Corner,
    Selection,
    Holdout
};

const char* surfaceRefitSampleSetToString(SurfaceRefitSampleSet sample_set);

enum class SurfaceRefitSamplingStrategy {
    AreaWeighted,
    FaceStratifiedAreaWeighted
};

const char* surfaceRefitSamplingStrategyToString(SurfaceRefitSamplingStrategy strategy);

struct SurfaceRefitSampleDiagnostic {
    SurfaceRefitSampleCategory category = SurfaceRefitSampleCategory::Boundary;
    SurfaceRefitSampleSet sample_set = SurfaceRefitSampleSet::Boundary;
    gp_Pnt source_point;
    gp_Pnt projected_point;
    double distance = 0.0;
    bool projection_success = false;
    bool over_tolerance = false;
    FaceId face_id = -1;
    EdgeId edge_id = -1;
    bool nearest_training_distance_defined = false;
    double nearest_training_distance = 0.0;
};

struct SurfaceRefitNormalSampleDiagnostic {
    SurfaceRefitSampleSet sample_set = SurfaceRefitSampleSet::Holdout;
    gp_Pnt source_point;
    gp_Pnt projected_point;
    gp_Vec source_normal;
    gp_Vec fitted_normal;
    double angle_degrees = 0.0;
    double u = 0.0;
    double v = 0.0;
    FaceId face_id = -1;
    bool projection_success = false;
    bool fitted_normal_defined = false;
    bool over_tolerance = false;
};

struct SurfaceRefitFaceSamplingStatistics {
    FaceId face_id = -1;
    int training_count = 0;
    int validation_count = 0;
    int validation_over_tolerance_count = 0;
    int selection_count = 0;
    int selection_over_tolerance_count = 0;
    int holdout_count = 0;
    int holdout_over_tolerance_count = 0;
};

struct SurfaceRefitTrainingSampleDiagnostic {
    int training_index = -1;
    gp_Pnt source_point;
    gp_Vec source_normal;
    FaceId face_id = -1;
    double source_u = 0.0;
    double source_v = 0.0;
    bool support_face_available = false;
    bool support_uv_valid = false;
    gp_Pnt support_point;
    double support_point_distance = 0.0;
    std::string constraint_type = "G0";
};

struct SurfaceRefitLocalG1ConstraintDiagnostic {
    int rank = 0;
    int selection_sample_index = -1;
    double score = 0.0;
    double selection_distance = 0.0;
    double selection_normal_angle_degrees = 0.0;
    FaceId selection_face_id = -1;
    double selection_u = 0.0;
    double selection_v = 0.0;
    gp_Pnt selection_source_point;
    gp_Vec selection_source_normal;
    int training_index = -1;
    FaceId training_face_id = -1;
    double training_u = 0.0;
    double training_v = 0.0;
    gp_Pnt training_source_point;
    gp_Vec training_source_normal;
    double mapping_distance = 0.0;
    std::string constraint_type = "G1";
};

struct SurfaceRefitBRepContextStatus {
    std::string shape_type;
    int shape_index = -1;
    std::vector<std::string> statuses;
};

struct SurfaceRefitBRepSubshapeStatus {
    std::string shape_type;
    int shape_index = -1;
    int output_edge_index = -1;
    std::vector<std::string> statuses;
    std::vector<SurfaceRefitBRepContextStatus> contextual_statuses;
};

struct SurfaceRefitOutputEdgeDiagnostic {
    int edge_index = -1;
    EdgeId source_boundary_edge_id = -1;
    double tolerance = 0.0;
    bool degenerated = false;
    bool same_range = false;
    bool same_parameter = false;
    bool has_curve3d = false;
    double curve3d_first = 0.0;
    double curve3d_last = 0.0;
    bool has_pcurve = false;
    double pcurve_first = 0.0;
    double pcurve_last = 0.0;
    bool curve_on_surface_deviation_computed = false;
    bool deviation_used_normalized_parameter_mapping = false;
    double curve_on_surface_max_deviation = 0.0;
};

struct SurfaceRefitBRepStageDiagnostic {
    bool performed = false;
    bool valid = false;
    int face_count = 0;
    int wire_count = 0;
    int edge_count = 0;
    int vertex_count = 0;
    std::vector<SurfaceRefitBRepSubshapeStatus> subshape_statuses;
    std::vector<SurfaceRefitOutputEdgeDiagnostic> edges;
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
    SurfaceRefitSamplingStrategy sampling_strategy = SurfaceRefitSamplingStrategy::AreaWeighted;
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
    int holdout_sample_count = 96;
    int local_g1_count = 0;
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
    SurfaceRefitSamplingStrategy sampling_strategy = SurfaceRefitSamplingStrategy::AreaWeighted;
    int candidate_id = -1;
    int candidate_face_count = 0;
    bool boundary_valid = false;
    bool filling_done = false;
    bool brep_check_valid = false;
    bool filling_face_brep_check_valid = false;
    bool oriented_face_brep_check_valid = false;
    bool step_roundtrip_brep_check_performed = false;
    bool step_roundtrip_brep_check_valid = false;
    bool step_roundtrip_read_success = false;
    int step_roundtrip_face_count = 0;
    int step_roundtrip_edge_count = 0;
    std::string step_roundtrip_message;
    bool output_orientation_reversed = false;
    int output_edge_count = 0;
    int missing_pcurve_count = 0;
    int same_parameter_failure_count = 0;
    int requested_training_sample_count = 0;
    int requested_validation_sample_count = 0;
    int requested_selection_sample_count = 0;
    int requested_holdout_sample_count = 0;
    int training_sample_count = 0;
    int validation_sample_count = 0;
    int selection_sample_count = 0;
    int holdout_sample_count = 0;
    int training_validation_overlap_count = 0;
    int training_selection_overlap_count = 0;
    int training_holdout_overlap_count = 0;
    int selection_holdout_overlap_count = 0;
    bool sample_sets_disjoint = false;
    int configured_local_g1_count = 0;
    int actual_local_g1_count = 0;
    int g0_training_constraint_count = 0;
    int total_training_constraint_count = 0;
    double local_g1_nms_radius = 0.0;
    double g0_error = 0.0;
    double g1_error = 0.0;
    double g2_error = 0.0;
    std::vector<EdgeId> ordered_boundary_edges;
    SurfaceRefitErrorStatistics boundary_error;
    SurfaceRefitErrorStatistics corner_error;
    SurfaceRefitErrorStatistics interior_error;
    SurfaceRefitNormalStatistics normal_error;
    SurfaceRefitErrorStatistics selection_interior_error;
    SurfaceRefitNormalStatistics selection_normal_error;
    SurfaceRefitErrorStatistics holdout_interior_error;
    SurfaceRefitNormalStatistics holdout_normal_error;
    SurfaceRefitJacobianReport jacobian;
    SurfaceRefitComplexityReport complexity;
    SurfaceRefitBRepStageDiagnostic filling_face_brep;
    SurfaceRefitBRepStageDiagnostic oriented_face_brep;
    std::vector<SurfaceRefitFaceSamplingStatistics> face_sampling;
    std::vector<SurfaceRefitTrainingSampleDiagnostic> training_samples;
    std::vector<SurfaceRefitLocalG1ConstraintDiagnostic> local_g1_constraints;
    std::vector<SurfaceRefitSampleDiagnostic> surface_error_samples;
    std::vector<SurfaceRefitSampleDiagnostic> selection_error_samples;
    std::vector<SurfaceRefitNormalSampleDiagnostic> normal_error_samples;
    std::vector<SurfaceRefitNormalSampleDiagnostic> selection_normal_error_samples;
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
