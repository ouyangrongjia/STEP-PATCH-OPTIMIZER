#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "merge/MergePlanner.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "merge/SurfaceRefitDiagnostics.h"
#include "merge/SurfaceRefitter.h"

#include <BRepCheck_Analyzer.hxx>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::filesystem::path source_step;
    std::filesystem::path output_patch;
    std::filesystem::path report;
    int candidate_id = -1;
    bool auto_candidate = false;
    double feature_angle_degrees = 25.0;
    double min_edge_length = 0.0;
    spo::SurfaceRefitOptions refit;
};

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.c_str()), utf8.size()};
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const char character : value) {
        switch (character) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                const char* digits = "0123456789ABCDEF";
                output << "\\u00"
                       << digits[(static_cast<unsigned char>(character) >> 4) & 0x0F]
                       << digits[static_cast<unsigned char>(character) & 0x0F];
            } else {
                output << character;
            }
            break;
        }
    }
    return output.str();
}

std::string quote_json(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

const char* bool_json(const bool value) {
    return value ? "true" : "false";
}

void print_usage() {
    std::cerr
        << "Usage: native_surface_fit_probe --source-step <model.stp> "
        << "--candidate-id <id|auto> --output-patch <patch.stp> --report <report.json> "
        << "[--tolerance3d <value>] [--normal-tolerance-degrees <value>] "
        << "[--training-samples <count>] [--validation-samples <count>] "
        << "[--sampling-strategy <area_weighted|face_stratified_area_weighted>]\n";
}

bool parse_int(const char* text, int& value) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoi(text, &consumed);
        if (consumed != std::string(text).size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_double(const char* text, double& value) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stod(text, &consumed);
        if (consumed != std::string(text).size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_sampling_strategy(
    const char* text,
    spo::SurfaceRefitSamplingStrategy& strategy) {
    const std::string value = text;
    if (value == "area_weighted") {
        strategy = spo::SurfaceRefitSamplingStrategy::AreaWeighted;
        return true;
    }
    if (value == "face_stratified_area_weighted") {
        strategy = spo::SurfaceRefitSamplingStrategy::FaceStratifiedAreaWeighted;
        return true;
    }
    return false;
}

bool parse_options(int argc, char* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto require_value = [&](const char* name) -> const char* {
            if (index + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++index];
        };

        if (argument == "--source-step") {
            const auto* value = require_value("--source-step");
            if (value == nullptr) return false;
            options.source_step = value;
        } else if (argument == "--candidate-id") {
            const auto* value = require_value("--candidate-id");
            if (value == nullptr) return false;
            if (std::string(value) == "auto") {
                options.auto_candidate = true;
                options.candidate_id = -1;
            } else if (!parse_int(value, options.candidate_id) || options.candidate_id < 0) {
                std::cerr << "Invalid candidate id: " << value << "\n";
                return false;
            }
        } else if (argument == "--output-patch") {
            const auto* value = require_value("--output-patch");
            if (value == nullptr) return false;
            options.output_patch = value;
        } else if (argument == "--report") {
            const auto* value = require_value("--report");
            if (value == nullptr) return false;
            options.report = value;
        } else if (argument == "--angle") {
            const auto* value = require_value("--angle");
            if (value == nullptr || !parse_double(value, options.feature_angle_degrees)) return false;
        } else if (argument == "--min-edge-length") {
            const auto* value = require_value("--min-edge-length");
            if (value == nullptr || !parse_double(value, options.min_edge_length)) return false;
        } else if (argument == "--tolerance3d") {
            const auto* value = require_value("--tolerance3d");
            if (value == nullptr || !parse_double(value, options.refit.tolerance3d)) return false;
        } else if (argument == "--normal-tolerance-degrees") {
            const auto* value = require_value("--normal-tolerance-degrees");
            if (value == nullptr || !parse_double(value, options.refit.normal_tolerance_degrees)) return false;
        } else if (argument == "--training-samples") {
            const auto* value = require_value("--training-samples");
            if (value == nullptr || !parse_int(value, options.refit.training_sample_count)) return false;
        } else if (argument == "--validation-samples") {
            const auto* value = require_value("--validation-samples");
            if (value == nullptr || !parse_int(value, options.refit.validation_sample_count)) return false;
        } else if (argument == "--sampling-strategy") {
            const auto* value = require_value("--sampling-strategy");
            if (value == nullptr || !parse_sampling_strategy(value, options.refit.sampling_strategy)) {
                std::cerr << "Invalid sampling strategy: " << (value != nullptr ? value : "") << "\n";
                return false;
            }
        } else if (argument == "--boundary-samples-per-edge") {
            const auto* value = require_value("--boundary-samples-per-edge");
            if (value == nullptr || !parse_int(value, options.refit.boundary_samples_per_edge)) return false;
        } else if (argument == "--jacobian-grid-size") {
            const auto* value = require_value("--jacobian-grid-size");
            if (value == nullptr || !parse_int(value, options.refit.jacobian_grid_size)) return false;
        } else {
            std::cerr << "Unknown argument: " << argument << "\n";
            return false;
        }
    }

    return !options.source_step.empty() &&
        !options.output_patch.empty() &&
        !options.report.empty() &&
        (options.auto_candidate || options.candidate_id >= 0);
}

spo::MergePlannerOptions planner_options() {
    spo::MergePlannerOptions options;
    options.enable_feature_bounded_refit_candidates = true;
    options.min_feature_bounded_region_faces = 2;
    return options;
}

const spo::MergeCandidate* select_candidate(
    const spo::ShapeDocument& document,
    const std::vector<spo::MergeCandidate>& candidates,
    const Options& options,
    std::string& message) {
    if (!options.auto_candidate) {
        const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
            return candidate.candidate_id == options.candidate_id;
        });
        if (found == candidates.end()) {
            message = "Requested candidate id was not found.";
            return nullptr;
        }
        return &*found;
    }

    std::vector<const spo::MergeCandidate*> ranked;
    for (const auto& candidate : candidates) {
        if (!candidate.valid ||
            candidate.candidate_type != spo::MergeCandidateType::FeatureBoundedRefit ||
            candidate.status == spo::MergeCandidateStatus::Rejected ||
            candidate.status == spo::MergeCandidateStatus::Hidden) {
            continue;
        }
        if (spo::RegionBoundaryAnalyzer().analyze(document, candidate).valid) {
            ranked.push_back(&candidate);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto* left, const auto* right) {
        if (left->faces.size() != right->faces.size()) {
            return left->faces.size() > right->faces.size();
        }
        if (left->boundary_edges.size() != right->boundary_edges.size()) {
            return left->boundary_edges.size() > right->boundary_edges.size();
        }
        return left->candidate_id < right->candidate_id;
    });
    if (ranked.empty()) {
        message = "Auto selection found no valid FeatureBoundedRefit candidate.";
        return nullptr;
    }
    message = "Auto-selected the largest valid FeatureBoundedRefit candidate.";
    return ranked.front();
}

void write_point(std::ostream& output, const gp_Pnt& point) {
    output << "[" << point.X() << ", " << point.Y() << ", " << point.Z() << "]";
}

void write_vector(std::ostream& output, const gp_Vec& vector) {
    output << "[" << vector.X() << ", " << vector.Y() << ", " << vector.Z() << "]";
}

void write_id(std::ostream& output, const spo::FaceId id) {
    if (id == std::numeric_limits<spo::FaceId>::max()) {
        output << "null";
    } else {
        output << id;
    }
}

void write_edge_id(std::ostream& output, const spo::EdgeId id) {
    if (id == std::numeric_limits<spo::EdgeId>::max()) {
        output << "null";
    } else {
        output << id;
    }
}

void write_distance_statistics(
    std::ostream& output,
    const spo::SurfaceRefitErrorStatistics& statistics,
    const char* indent) {
    output << indent << "\"sample_count\": " << statistics.sample_count << ",\n";
    output << indent << "\"projection_failure_count\": " << statistics.projection_failure_count << ",\n";
    output << indent << "\"over_tolerance_count\": " << statistics.over_tolerance_count << ",\n";
    output << indent << "\"max\": " << statistics.max << ",\n";
    output << indent << "\"p95\": " << statistics.p95 << ",\n";
    output << indent << "\"rms\": " << statistics.rms << ",\n";
    output << indent << "\"worst_point\": ";
    write_point(output, statistics.worst_point);
    output << ",\n" << indent << "\"worst_face_id\": ";
    write_id(output, statistics.worst_face_id);
    output << ",\n" << indent << "\"worst_edge_id\": ";
    write_edge_id(output, statistics.worst_edge_id);
    output << "\n";
}

void write_string_array(std::ostream& output, const std::vector<std::string>& values) {
    output << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) output << ", ";
        output << quote_json(values[index]);
    }
    output << "]";
}

void write_brep_stage(
    std::ostream& output,
    const spo::SurfaceRefitBRepStageDiagnostic& stage,
    const char* indent) {
    output << indent << "{\n";
    output << indent << "  \"performed\": " << bool_json(stage.performed) << ",\n";
    output << indent << "  \"valid\": " << bool_json(stage.valid) << ",\n";
    output << indent << "  \"face_count\": " << stage.face_count << ",\n";
    output << indent << "  \"wire_count\": " << stage.wire_count << ",\n";
    output << indent << "  \"edge_count\": " << stage.edge_count << ",\n";
    output << indent << "  \"vertex_count\": " << stage.vertex_count << ",\n";
    output << indent << "  \"subshape_statuses\": [\n";
    for (std::size_t index = 0; index < stage.subshape_statuses.size(); ++index) {
        const auto& status = stage.subshape_statuses[index];
        output << indent << "    {\n";
        output << indent << "      \"shape_type\": " << quote_json(status.shape_type) << ",\n";
        output << indent << "      \"shape_index\": " << status.shape_index << ",\n";
        output << indent << "      \"output_edge_index\": ";
        if (status.output_edge_index >= 0) {
            output << status.output_edge_index;
        } else {
            output << "null";
        }
        output << ",\n" << indent << "      \"statuses\": ";
        write_string_array(output, status.statuses);
        output << ",\n" << indent << "      \"contextual_statuses\": [\n";
        for (std::size_t context_index = 0;
             context_index < status.contextual_statuses.size();
             ++context_index) {
            const auto& context = status.contextual_statuses[context_index];
            output << indent << "        {\"shape_type\": " << quote_json(context.shape_type)
                   << ", \"shape_index\": " << context.shape_index << ", \"statuses\": ";
            write_string_array(output, context.statuses);
            output << "}" << (context_index + 1 < status.contextual_statuses.size() ? "," : "") << "\n";
        }
        output << indent << "      ]\n";
        output << indent << "    }" << (index + 1 < stage.subshape_statuses.size() ? "," : "") << "\n";
    }
    output << indent << "  ],\n";
    output << indent << "  \"edges\": [\n";
    for (std::size_t index = 0; index < stage.edges.size(); ++index) {
        const auto& edge = stage.edges[index];
        output << indent << "    {\n";
        output << indent << "      \"edge_index\": " << edge.edge_index << ",\n";
        output << indent << "      \"source_boundary_edge_id\": ";
        write_edge_id(output, edge.source_boundary_edge_id);
        output << ",\n" << indent << "      \"tolerance\": " << edge.tolerance << ",\n";
        output << indent << "      \"degenerated\": " << bool_json(edge.degenerated) << ",\n";
        output << indent << "      \"same_range\": " << bool_json(edge.same_range) << ",\n";
        output << indent << "      \"same_parameter\": " << bool_json(edge.same_parameter) << ",\n";
        output << indent << "      \"has_curve3d\": " << bool_json(edge.has_curve3d) << ",\n";
        output << indent << "      \"curve3d_range\": ";
        if (edge.has_curve3d) {
            output << "[" << edge.curve3d_first << ", " << edge.curve3d_last << "]";
        } else {
            output << "null";
        }
        output << ",\n" << indent << "      \"has_pcurve\": " << bool_json(edge.has_pcurve) << ",\n";
        output << indent << "      \"pcurve_range\": ";
        if (edge.has_pcurve) {
            output << "[" << edge.pcurve_first << ", " << edge.pcurve_last << "]";
        } else {
            output << "null";
        }
        output << ",\n" << indent << "      \"curve_on_surface_deviation_computed\": "
               << bool_json(edge.curve_on_surface_deviation_computed) << ",\n";
        output << indent << "      \"deviation_used_normalized_parameter_mapping\": "
               << bool_json(edge.deviation_used_normalized_parameter_mapping) << ",\n";
        output << indent << "      \"curve_on_surface_max_deviation\": ";
        if (edge.curve_on_surface_deviation_computed) {
            output << edge.curve_on_surface_max_deviation;
        } else {
            output << "null";
        }
        output << "\n" << indent << "    }" << (index + 1 < stage.edges.size() ? "," : "") << "\n";
    }
    output << indent << "  ]\n";
    output << indent << "}";
}

bool write_report(
    const Options& options,
    const spo::MergeCandidate* candidate,
    const spo::SurfaceRefitReport& report,
    const bool fit_success,
    const bool patch_written,
    const bool overall_success,
    const std::string& stage) {
    const auto diagnostic_directory =
        options.report.parent_path() / "diagnostics" / "native_surface_fit";
    const auto diagnostic_artifacts =
        spo::writeSurfaceRefitDiagnosticArtifacts(report, diagnostic_directory);
    const bool diagnostic_patch = patch_written && !overall_success;

    std::error_code error;
    const auto parent = options.report.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return false;
    }

    std::ofstream output(options.report, std::ios::binary);
    if (!output.good()) return false;
    output << std::setprecision(17);
    output << "{\n";
    output << "  \"schema_version\": 3,\n";
    output << "  \"tool\": \"native_surface_fit_probe\",\n";
    output << "  \"stage\": " << quote_json(stage) << ",\n";
    output << "  \"source_step\": " << quote_json(path_to_utf8(options.source_step)) << ",\n";
    output << "  \"output_patch\": " << quote_json(path_to_utf8(options.output_patch)) << ",\n";
    output << "  \"report_path\": " << quote_json(path_to_utf8(options.report)) << ",\n";
    output << "  \"requested_candidate\": "
           << (options.auto_candidate ? quote_json("auto") : std::to_string(options.candidate_id)) << ",\n";
    output << "  \"selected_candidate_id\": "
           << (candidate != nullptr ? std::to_string(candidate->candidate_id) : "null") << ",\n";
    output << "  \"selected_candidate_type\": "
           << (candidate != nullptr ? quote_json(spo::toString(candidate->candidate_type)) : "null") << ",\n";
    output << "  \"candidate_face_count\": " << report.candidate_face_count << ",\n";
    output << "  \"boundary_valid\": " << bool_json(report.boundary_valid) << ",\n";
    output << "  \"success\": " << bool_json(overall_success) << ",\n";
    output << "  \"fit_success\": " << bool_json(fit_success) << ",\n";
    output << "  \"patch_written\": " << bool_json(patch_written) << ",\n";
    output << "  \"diagnostic_patch\": " << bool_json(diagnostic_patch) << ",\n";
    output << "  \"patch_acceptable\": " << bool_json(overall_success && patch_written) << ",\n";
    output << "  \"patch_role\": " << quote_json(
        diagnostic_patch ? "failed_fit_diagnostic_only" :
        (overall_success && patch_written ? "accepted_native_surface_fit" : "not_written")) << ",\n";
    output << "  \"failure_reason\": " << quote_json(spo::surfaceRefitFailureReasonToString(report.failure_reason)) << ",\n";
    output << "  \"failed_gates\": [";
    if (report.failed_gates.empty() && report.failure_reason != spo::SurfaceRefitFailureReason::None) {
        output << quote_json(spo::surfaceRefitFailureReasonToString(report.failure_reason));
    } else {
        for (std::size_t index = 0; index < report.failed_gates.size(); ++index) {
            if (index > 0) output << ", ";
            output << quote_json(spo::surfaceRefitFailureReasonToString(report.failed_gates[index]));
        }
    }
    output << "],\n";
    output << "  \"message\": " << quote_json(report.message) << ",\n";
    output << "  \"configuration\": {\n";
    output << "    \"sampling_strategy\": "
           << quote_json(spo::surfaceRefitSamplingStrategyToString(options.refit.sampling_strategy)) << ",\n";
    output << "    \"feature_angle_degrees\": " << options.feature_angle_degrees << ",\n";
    output << "    \"min_edge_length\": " << options.min_edge_length << ",\n";
    output << "    \"tolerance2d\": " << options.refit.tolerance2d << ",\n";
    output << "    \"tolerance3d\": " << options.refit.tolerance3d << ",\n";
    output << "    \"tolerance_angle\": " << options.refit.tolerance_angle << ",\n";
    output << "    \"tolerance_curvature\": " << options.refit.tolerance_curvature << ",\n";
    output << "    \"normal_tolerance_degrees\": " << options.refit.normal_tolerance_degrees << ",\n";
    output << "    \"mesh_deflection\": " << options.refit.mesh_deflection << ",\n";
    output << "    \"mesh_angular_deflection\": " << options.refit.mesh_angular_deflection << ",\n";
    output << "    \"sample_deduplication_tolerance\": " << options.refit.sample_deduplication_tolerance << ",\n";
    output << "    \"minimum_normalized_jacobian\": " << options.refit.minimum_normalized_jacobian << ",\n";
    output << "    \"plate_degree\": " << options.refit.plate_degree << ",\n";
    output << "    \"points_on_curve\": " << options.refit.points_on_curve << ",\n";
    output << "    \"iterations\": " << options.refit.iterations << ",\n";
    output << "    \"max_degree\": " << options.refit.max_degree << ",\n";
    output << "    \"max_segments\": " << options.refit.max_segments << ",\n";
    output << "    \"training_sample_count\": " << options.refit.training_sample_count << ",\n";
    output << "    \"validation_sample_count\": " << options.refit.validation_sample_count << ",\n";
    output << "    \"boundary_samples_per_edge\": " << options.refit.boundary_samples_per_edge << ",\n";
    output << "    \"jacobian_grid_size\": " << options.refit.jacobian_grid_size << ",\n";
    output << "    \"max_surface_degree\": " << options.refit.max_surface_degree << ",\n";
    output << "    \"max_surface_pole_count\": " << options.refit.max_surface_pole_count << ",\n";
    output << "    \"max_surface_knot_count\": " << options.refit.max_surface_knot_count << ",\n";
    output << "    \"max_surface_segment_count\": " << options.refit.max_surface_segment_count << "\n";
    output << "  },\n";
    output << "  \"constraints\": {\n";
    output << "    \"boundary_order\": \"G0\",\n";
    output << "    \"boundary_source\": \"original_ordered_STEP_edges\",\n";
    output << "    \"shape_source\": \"original_STEP_candidate_face_interior_points\",\n";
    output << "    \"ordered_boundary_edge_ids\": [";
    for (std::size_t index = 0; index < report.ordered_boundary_edges.size(); ++index) {
        if (index > 0) output << ", ";
        output << report.ordered_boundary_edges[index];
    }
    output << "]\n  },\n";
    output << "  \"sampling\": {\n";
    output << "    \"strategy\": "
           << quote_json(spo::surfaceRefitSamplingStrategyToString(report.sampling_strategy)) << ",\n";
    output << "    \"requested_training_count\": " << report.requested_training_sample_count << ",\n";
    output << "    \"requested_validation_count\": " << report.requested_validation_sample_count << ",\n";
    output << "    \"training_count\": " << report.training_sample_count << ",\n";
    output << "    \"validation_count\": " << report.validation_sample_count << ",\n";
    output << "    \"training_validation_overlap_count\": " << report.training_validation_overlap_count << ",\n";
    output << "    \"per_face\": [\n";
    for (std::size_t index = 0; index < report.face_sampling.size(); ++index) {
        const auto& face = report.face_sampling[index];
        output << "      {\"face_id\": ";
        write_id(output, face.face_id);
        output << ", \"training_count\": " << face.training_count
               << ", \"validation_count\": " << face.validation_count
               << ", \"validation_over_tolerance_count\": "
               << face.validation_over_tolerance_count << "}"
               << (index + 1 < report.face_sampling.size() ? "," : "") << "\n";
    }
    output << "    ]\n";
    output << "  },\n";
    output << "  \"filling\": {\n";
    output << "    \"done\": " << bool_json(report.filling_done) << ",\n";
    output << "    \"g0_error\": " << report.g0_error << ",\n";
    output << "    \"g1_error\": " << report.g1_error << ",\n";
    output << "    \"g2_error\": " << report.g2_error << "\n";
    output << "  },\n";
    output << "  \"boundary_error\": {\n";
    write_distance_statistics(output, report.boundary_error, "    ");
    output << "  },\n";
    output << "  \"corner_error\": {\n";
    write_distance_statistics(output, report.corner_error, "    ");
    output << "  },\n";
    output << "  \"interior_validation_error\": {\n";
    write_distance_statistics(output, report.interior_error, "    ");
    output << "  },\n";
    output << "  \"normal_validation_error\": {\n";
    output << "    \"sample_count\": " << report.normal_error.sample_count << ",\n";
    output << "    \"undefined_count\": " << report.normal_error.undefined_count << ",\n";
    output << "    \"over_tolerance_count\": " << report.normal_error.over_tolerance_count << ",\n";
    output << "    \"max_angle_degrees\": " << report.normal_error.max_angle_degrees << ",\n";
    output << "    \"p95_angle_degrees\": " << report.normal_error.p95_angle_degrees << ",\n";
    output << "    \"rms_angle_degrees\": " << report.normal_error.rms_angle_degrees << ",\n";
    output << "    \"worst_point\": ";
    write_point(output, report.normal_error.worst_point);
    output << ",\n    \"worst_face_id\": ";
    write_id(output, report.normal_error.worst_face_id);
    output << "\n  },\n";
    output << "  \"surface_error_samples\": [\n";
    for (std::size_t index = 0; index < report.surface_error_samples.size(); ++index) {
        const auto& sample = report.surface_error_samples[index];
        output << "    {\n";
        output << "      \"category\": "
               << quote_json(spo::surfaceRefitSampleCategoryToString(sample.category)) << ",\n";
        output << "      \"source_point\": ";
        write_point(output, sample.source_point);
        output << ",\n      \"projected_point\": ";
        if (sample.projection_success) {
            write_point(output, sample.projected_point);
        } else {
            output << "null";
        }
        output << ",\n      \"distance\": ";
        if (sample.projection_success) {
            output << sample.distance;
        } else {
            output << "null";
        }
        output << ",\n      \"projection_success\": " << bool_json(sample.projection_success) << ",\n";
        output << "      \"over_tolerance\": " << bool_json(sample.over_tolerance) << ",\n";
        output << "      \"face_id\": ";
        write_id(output, sample.face_id);
        output << ",\n      \"edge_id\": ";
        write_edge_id(output, sample.edge_id);
        output << ",\n      \"nearest_training_distance\": ";
        if (sample.nearest_training_distance_defined) {
            output << sample.nearest_training_distance;
        } else {
            output << "null";
        }
        output << "\n    }" << (index + 1 < report.surface_error_samples.size() ? "," : "") << "\n";
    }
    output << "  ],\n";
    output << "  \"normal_error_samples\": [\n";
    for (std::size_t index = 0; index < report.normal_error_samples.size(); ++index) {
        const auto& sample = report.normal_error_samples[index];
        output << "    {\n";
        output << "      \"source_point\": ";
        write_point(output, sample.source_point);
        output << ",\n      \"projected_point\": ";
        if (sample.projection_success) {
            write_point(output, sample.projected_point);
        } else {
            output << "null";
        }
        output << ",\n      \"source_normal\": ";
        write_vector(output, sample.source_normal);
        output << ",\n      \"fitted_normal\": ";
        if (sample.fitted_normal_defined) {
            write_vector(output, sample.fitted_normal);
        } else {
            output << "null";
        }
        output << ",\n      \"angle_degrees\": ";
        if (sample.fitted_normal_defined) {
            output << sample.angle_degrees;
        } else {
            output << "null";
        }
        output << ",\n      \"uv\": ";
        if (sample.projection_success) {
            output << "[" << sample.u << ", " << sample.v << "]";
        } else {
            output << "null";
        }
        output << ",\n      \"face_id\": ";
        write_id(output, sample.face_id);
        output << ",\n      \"projection_success\": " << bool_json(sample.projection_success) << ",\n";
        output << "      \"fitted_normal_defined\": " << bool_json(sample.fitted_normal_defined) << ",\n";
        output << "      \"over_tolerance\": " << bool_json(sample.over_tolerance) << "\n";
        output << "    }" << (index + 1 < report.normal_error_samples.size() ? "," : "") << "\n";
    }
    output << "  ],\n";
    output << "  \"diagnostics\": {\n";
    output << "    \"directory\": " << quote_json(path_to_utf8(diagnostic_directory)) << ",\n";
    output << "    \"surface_error_samples_csv\": "
           << quote_json(path_to_utf8(diagnostic_artifacts.csv_path)) << ",\n";
    output << "    \"surface_error_vectors_obj\": "
           << quote_json(path_to_utf8(diagnostic_artifacts.obj_path)) << ",\n";
    output << "    \"surface_normal_error_samples_csv\": "
           << quote_json(path_to_utf8(diagnostic_artifacts.normal_csv_path)) << ",\n";
    output << "    \"csv_written\": " << bool_json(diagnostic_artifacts.csv_written) << ",\n";
    output << "    \"obj_written\": " << bool_json(diagnostic_artifacts.obj_written) << ",\n";
    output << "    \"normal_csv_written\": " << bool_json(diagnostic_artifacts.normal_csv_written) << ",\n";
    output << "    \"message\": " << quote_json(diagnostic_artifacts.message) << "\n";
    output << "  },\n";
    output << "  \"jacobian\": {\n";
    output << "    \"sample_count\": " << report.jacobian.sample_count << ",\n";
    output << "    \"near_singular_count\": " << report.jacobian.near_singular_count << ",\n";
    output << "    \"flipped_pair_count\": " << report.jacobian.flipped_pair_count << ",\n";
    output << "    \"min_magnitude\": " << report.jacobian.min_magnitude << ",\n";
    output << "    \"max_magnitude\": " << report.jacobian.max_magnitude << ",\n";
    output << "    \"min_normalized\": " << report.jacobian.min_normalized << ",\n";
    output << "    \"worst_uv\": [" << report.jacobian.worst_u << ", " << report.jacobian.worst_v << "]\n";
    output << "  },\n";
    output << "  \"surface_complexity\": {\n";
    output << "    \"surface_type\": " << quote_json(report.complexity.surface_type) << ",\n";
    output << "    \"u_degree\": " << report.complexity.u_degree << ",\n";
    output << "    \"v_degree\": " << report.complexity.v_degree << ",\n";
    output << "    \"u_poles\": " << report.complexity.u_poles << ",\n";
    output << "    \"v_poles\": " << report.complexity.v_poles << ",\n";
    output << "    \"total_poles\": " << report.complexity.total_poles << ",\n";
    output << "    \"u_knots\": " << report.complexity.u_knots << ",\n";
    output << "    \"v_knots\": " << report.complexity.v_knots << ",\n";
    output << "    \"u_segments\": " << report.complexity.u_segments << ",\n";
    output << "    \"v_segments\": " << report.complexity.v_segments << ",\n";
    output << "    \"total_segments\": " << report.complexity.total_segments << "\n";
    output << "  },\n";
    output << "  \"brep\": {\n";
    output << "    \"brep_check_valid\": " << bool_json(report.brep_check_valid) << ",\n";
    output << "    \"filling_face_brep_check_valid\": " << bool_json(report.filling_face_brep_check_valid) << ",\n";
    output << "    \"oriented_face_brep_check_valid\": " << bool_json(report.oriented_face_brep_check_valid) << ",\n";
    output << "    \"step_roundtrip_read_success\": " << bool_json(report.step_roundtrip_read_success) << ",\n";
    output << "    \"step_roundtrip_brep_check_performed\": " << bool_json(report.step_roundtrip_brep_check_performed) << ",\n";
    output << "    \"step_roundtrip_brep_check_valid\": " << bool_json(report.step_roundtrip_brep_check_valid) << ",\n";
    output << "    \"step_roundtrip_face_count\": " << report.step_roundtrip_face_count << ",\n";
    output << "    \"step_roundtrip_edge_count\": " << report.step_roundtrip_edge_count << ",\n";
    output << "    \"step_roundtrip_message\": " << quote_json(report.step_roundtrip_message) << ",\n";
    output << "    \"output_orientation_reversed\": " << bool_json(report.output_orientation_reversed) << ",\n";
    output << "    \"output_face_count\": " << (report.filling_done ? 1 : 0) << ",\n";
    output << "    \"output_edge_count\": " << report.output_edge_count << ",\n";
    output << "    \"missing_pcurve_count\": " << report.missing_pcurve_count << ",\n";
    output << "    \"same_parameter_failure_count\": " << report.same_parameter_failure_count << ",\n";
    output << "    \"filling_face\": ";
    write_brep_stage(output, report.filling_face_brep, "    ");
    output << ",\n    \"oriented_face\": ";
    write_brep_stage(output, report.oriented_face_brep, "    ");
    output << "\n";
    output << "  }\n";
    output << "}\n";
    return output.good();
}

spo::SurfaceRefitReport failure_report(
    const Options& options,
    const spo::SurfaceRefitFailureReason reason,
    std::string message) {
    spo::SurfaceRefitReport report;
    report.candidate_id = options.candidate_id;
    report.requested_training_sample_count = options.refit.training_sample_count;
    report.requested_validation_sample_count = options.refit.validation_sample_count;
    report.failure_reason = reason;
    report.failed_gates.push_back(reason);
    report.message = std::move(message);
    return report;
}

void add_probe_failure(
    spo::SurfaceRefitReport& report,
    const spo::SurfaceRefitFailureReason reason,
    const std::string& message) {
    report.success = false;
    if (report.failure_reason == spo::SurfaceRefitFailureReason::None) {
        report.failure_reason = reason;
        report.message = message;
    } else if (!message.empty()) {
        report.message += " " + message;
    }
    if (std::find(report.failed_gates.begin(), report.failed_gates.end(), reason) ==
        report.failed_gates.end()) {
        report.failed_gates.push_back(reason);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }

    if (!std::filesystem::exists(options.source_step)) {
        const auto report = failure_report(options, spo::SurfaceRefitFailureReason::InvalidInput, "Source STEP does not exist.");
        write_report(options, nullptr, report, false, false, false, "input");
        std::cerr << report.message << "\n";
        return 1;
    }

    const auto read = spo::StepReader().read(options.source_step);
    if (!read.status.success()) {
        const auto report = failure_report(options, spo::SurfaceRefitFailureReason::InvalidInput, read.status.message());
        write_report(options, nullptr, report, false, false, false, "read_step");
        std::cerr << report.message << "\n";
        return 1;
    }

    const auto feature_edges = spo::FeatureEdgeDetector().detect(
        read.document.topology(),
        options.feature_angle_degrees,
        options.min_edge_length);
    const auto plan = spo::MergePlanner().plan(read.document, feature_edges, {}, planner_options());
    std::string selection_message;
    const auto* candidate = select_candidate(read.document, plan.candidates, options, selection_message);
    if (candidate == nullptr) {
        const auto report = failure_report(options, spo::SurfaceRefitFailureReason::InvalidInput, selection_message);
        write_report(options, nullptr, report, false, false, false, "select_candidate");
        std::cerr << report.message << "\n";
        return 1;
    }

    auto result = spo::SurfaceRefitter().fitSingleFace(read.document, *candidate, options.refit);
    const bool fit_success = result.success();
    bool patch_written = false;
    std::string patch_write_error;
    std::error_code directory_error;
    const auto output_parent = options.output_patch.parent_path();
    const bool diagnostic_face_available = !result.face.IsNull() && result.report.filling_done;
    if (diagnostic_face_available && !output_parent.empty()) {
        std::filesystem::create_directories(output_parent, directory_error);
    }
    if (directory_error) {
        patch_write_error = "Could not create patch output directory: " + directory_error.message();
    } else if (diagnostic_face_available) {
        const spo::ShapeDocument patch_document(result.face, options.output_patch);
        const auto write = spo::StepWriter().write(patch_document, options.output_patch);
        if (write.success()) {
            patch_written = true;
        } else {
            patch_write_error = write.message();
        }
    }

    bool roundtrip_acceptable = false;
    if (patch_written) {
        const auto roundtrip = spo::StepReader().read(options.output_patch);
        result.report.step_roundtrip_read_success = roundtrip.status.success();
        if (roundtrip.status.success()) {
            result.report.step_roundtrip_face_count = static_cast<int>(roundtrip.document.topology().faceCount());
            result.report.step_roundtrip_edge_count = static_cast<int>(roundtrip.document.topology().edgeCount());
            result.report.step_roundtrip_brep_check_performed = true;
            result.report.step_roundtrip_brep_check_valid =
                BRepCheck_Analyzer(roundtrip.document.shape()).IsValid();
            result.report.step_roundtrip_message = "STEP roundtrip read and BRepCheck completed.";
            roundtrip_acceptable = result.report.step_roundtrip_face_count == 1 &&
                result.report.step_roundtrip_brep_check_valid;
        } else {
            result.report.step_roundtrip_message = roundtrip.status.message();
        }
        if (!roundtrip_acceptable) {
            add_probe_failure(
                result.report,
                spo::SurfaceRefitFailureReason::StepRoundtripInvalid,
                "Diagnostic patch failed STEP roundtrip read, one-face, or BRepCheck validation.");
        }
    }

    if (!fit_success) {
        if (!patch_write_error.empty()) {
            result.report.message += " Diagnostic patch export also failed: " + patch_write_error;
        }
        write_report(options, candidate, result.report, false, patch_written, false, "fit_single_face");
        std::cerr << result.report.message << "\n";
        return 1;
    }
    if (!patch_written) {
        result.report.message = patch_write_error.empty()
            ? "Fitted face was not eligible for STEP export."
            : patch_write_error;
        write_report(options, candidate, result.report, true, false, false, "write_patch");
        std::cerr << result.report.message << "\n";
        return 1;
    }
    if (!roundtrip_acceptable) {
        write_report(options, candidate, result.report, true, true, false, "step_roundtrip");
        std::cerr << result.report.message << "\n";
        return 1;
    }
    if (!write_report(options, candidate, result.report, true, true, true, "complete")) {
        std::cerr << "Could not write JSON report: " << path_to_utf8(options.report) << "\n";
        return 1;
    }

    std::cout << "Native surface patch: " << path_to_utf8(options.output_patch) << "\n";
    std::cout << "JSON report: " << path_to_utf8(options.report) << "\n";
    std::cout << "candidate_id: " << candidate->candidate_id << "\n";
    std::cout << "validation_max_distance: " << result.report.interior_error.max << "\n";
    std::cout << "validation_max_normal_degrees: " << result.report.normal_error.max_angle_degrees << "\n";
    return 0;
}
