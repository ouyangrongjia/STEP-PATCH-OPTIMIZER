#include <cassert>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

std::filesystem::path source_root() {
#if defined(SPO_SOURCE_DIR)
    return std::filesystem::path(SPO_SOURCE_DIR);
#else
    return std::filesystem::current_path();
#endif
}

std::filesystem::path temp_root(const std::string& name) {
    auto root = std::filesystem::temp_directory_path() / name;
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    return root;
}

std::string quote_for_command(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

std::string without_ascii_space(std::string value) {
    std::string compact;
    compact.reserve(value.size());
    for (const unsigned char ch : value) {
        if (!std::isspace(ch)) {
            compact.push_back(static_cast<char>(ch));
        }
    }
    return compact;
}

void test_corner_baseline_probe_supports_auto_candidate_selection() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "corner_baseline_probe.cpp");

    assert(source.find("--candidate-id <id|auto>") != std::string::npos);
    assert(source.find("select_candidate_for_baseline") != std::string::npos);
    assert(source.find("candidate_selection_mode") != std::string::npos);
    assert(source.find("generated_candidate_count") != std::string::npos);
}

void test_scripted_baseline_gate_is_not_hardcoded_to_one_sample() {
    const auto root = source_root();
    const auto script = read_text_file(root / "scripts" / "run_corner_baseline_gate.ps1");

    assert(script.find("param(") != std::string::npos);
    assert(script.find("CandidateId = \"auto\"") != std::string::npos);
    assert(script.find("Experiment = \"A0\"") != std::string::npos);
    assert(script.find("Find-DefaultSourceStep") != std::string::npos);
    assert(script.find("Find-ExistingPatchForCandidate") != std::string::npos);
    assert(script.find("corner_baseline_probe") != std::string::npos);
    assert(script.find("--b1-corner-feature-sampling") != std::string::npos);
    assert(script.find("B2.2") != std::string::npos);
    assert(script.find("B2.3") != std::string::npos);
    assert(script.find("--b2-boundary-guard-band") == std::string::npos);
    assert(script.find("--b2-over-cover-strip") == std::string::npos);
    assert(script.find("--over-cover-width") == std::string::npos);
    assert(script.find("--b2-adjacent-face-support-collar") != std::string::npos);
    assert(script.find("--b2-corner-safe-support-collar") != std::string::npos);
    assert(script.find("--adaptive-support-collar-width") != std::string::npos);
    assert(script.find("-SharpenContours") != std::string::npos);
    assert(script.find("NoSharpenContours") != std::string::npos);
    assert(script.find("-RealGeomagic") != std::string::npos);
    assert(script.find("CandidateOverCoverDisableOuterRings") != std::string::npos);
    assert(script.find("CandidateOverCoverBodyBridgeMode") != std::string::npos);
    assert(script.find("CandidateOverCoverSmoothStrip") != std::string::npos);
    assert(script.find("CandidateOverCoverSmoothTransition") != std::string::npos);
    assert(script.find("CandidateOverCoverDisableBodyBridge") != std::string::npos);
    assert(script.find("--candidate-id") != std::string::npos);
    assert(script.find("candidate_0179") == std::string::npos);
    assert(script.find("03_") == std::string::npos);
}

void test_verify_real_geomagic_runs_scripted_baseline_gate() {
    const auto root = source_root();
    const auto script = read_text_file(root / "scripts" / "verify_spo.ps1");

    assert(script.find("run_corner_baseline_gate.ps1") != std::string::npos);
    assert(script.find("-RealGeomagic") != std::string::npos);
}

void test_long_running_tests_are_split_into_explicit_suites() {
    const auto root = source_root();
    const auto cmake = read_text_file(root / "CMakeLists.txt");
    const auto validation = read_text_file(root / "tests" / "test_validation.cpp");
    const auto verify = read_text_file(root / "scripts" / "verify_spo.ps1");

    assert(cmake.find("spo_tests --suite unit") != std::string::npos);
    assert(cmake.find("spo_real_file_tests") != std::string::npos);
    assert(cmake.find("spo_real_geomagic_tests") != std::string::npos);
    assert(validation.find("--suite unit|real-files|real-geomagic") != std::string::npos);
    assert(verify.find("SPO_ENABLE_REAL_GEOMAGIC_TESTS") == std::string::npos);
}

void test_boundary_runner_avoids_duplicate_generation_and_validation_work() {
    const auto root = source_root();
    const auto probe = read_text_file(root / "tools" / "corner_baseline_probe.cpp");
    const auto script = read_text_file(root / "scripts" / "run_boundary_trim_fill_experiments.ps1");

    assert(probe.find("--generate-only") != std::string::npos);
    assert(probe.find("--apply-only") != std::string::npos);
    assert(probe.find("generated_patch") != std::string::npos);
    assert(script.find("--generate-only") != std::string::npos);
    assert(script.find("--apply-only") != std::string::npos);
    assert(script.find("New-StepStatsFromStrictGate") != std::string::npos);
    assert(script.find("Invoke-StepStats") == std::string::npos);

    assert(script.find("step_unit_normalizer") != std::string::npos);
    assert(script.find("--target-step", script.find("step_unit_normalizer")) != std::string::npos);
    assert(script.find("Invoke-CreoDiagnostic") != std::string::npos);
    assert(script.find("Invoke-CreoToolkitMergeVariant") != std::string::npos);
}

void test_corner_baseline_probe_reports_b2_2_support_collar_fields() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "corner_baseline_probe.cpp");

    assert(source.find("--b2-boundary-guard-band") == std::string::npos);
    assert(source.find("--b2-over-cover-strip") == std::string::npos);
    assert(source.find("boundary_guard_band") == std::string::npos);
    assert(source.find("boundary_over_cover") == std::string::npos);
    assert(source.find("--candidate-surface-over-cover") != std::string::npos);
    assert(source.find("--candidate-over-cover-width") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_enabled") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_triangle_count") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_boundary_coverage") != std::string::npos);
    assert(source.find("--b2-adjacent-face-support-collar") != std::string::npos);
    assert(source.find("--support-collar-width") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_enabled") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_triangle_count") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_boundary_coverage") != std::string::npos);
    assert(source.find("seam_continuity") != std::string::npos);
}

void test_corner_baseline_probe_exports_candidate_over_cover_rejection_artifacts() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "corner_baseline_probe.cpp");

    assert(source.find("candidate_surface_over_cover_body_bridge_rejected_count") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_boundary_to_ring_rejected_count") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_ring_to_ring_rejected_count") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_edge_build_rejected_count") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_problem_edges") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_ring_point_count") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_index_mismatch_count") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_source_face_transition_count") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_edge_source_distribution") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_max_normal_leakage_edge_id") != std::string::npos);
    assert(source.find("--candidate-over-cover-disable-outer-rings") != std::string::npos);
    assert(source.find("--candidate-over-cover-body-bridge-mode") != std::string::npos);
    assert(source.find("--candidate-over-cover-disable-body-bridge") != std::string::npos);
    assert(source.find("--candidate-over-cover-smooth-transition") != std::string::npos);
    assert(source.find("--candidate-over-cover-smooth-strip") != std::string::npos);
    assert(source.find("--geomagic-no-sharpen-contours") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_emitted_ring_count") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_body_bridge_mode") != std::string::npos);
    assert(source.find("candidate_surface_over_cover_body_bridge_triangle_count") != std::string::npos);
    assert(source.find("overcover_rejections.csv") != std::string::npos);
    assert(source.find("overcover_ring_diagnostics.csv") != std::string::npos);
    assert(source.find("overcover_rejected_body_bridge.stl") != std::string::npos);
    assert(source.find("overcover_rejected_boundary_to_ring.stl") != std::string::npos);
    assert(source.find("overcover_rejected_ring_to_ring.stl") != std::string::npos);
    assert(source.find("overcover_worst_points.obj") != std::string::npos);
    assert(source.find("overcover_experiment_summary.json") != std::string::npos);
    assert(source.find("overcover_experiment_summary.csv") != std::string::npos);
    assert(source.find("options.outputDir / \"artifacts\"") != std::string::npos);
    assert(source.find("options.outputDir / \"diagnostics\" / \"candidate_surface_over_cover\"") != std::string::npos);
    assert(source.find("raw_patch_step") != std::string::npos);
    assert(source.find("fitting_stl") != std::string::npos);
}

void test_corner_baseline_probe_reports_b2_3_corner_safe_and_sharpen_fields() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "corner_baseline_probe.cpp");

    assert(source.find("--b2-corner-safe-support-collar") != std::string::npos);
    assert(source.find("--support-collar-max-offset-scale") != std::string::npos);
    assert(source.find("--geomagic-sharpen-contours") != std::string::npos);
    assert(source.find("--geomagic-no-sharpen-contours") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_corner_clamp_enabled") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_corner_clamp_count") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_max_offset") != std::string::npos);
    assert(source.find("geomagic_sharpen_contours") != std::string::npos);
}

void test_route2_runner_uses_candidate_over_cover_as_default_expansion() {
    const auto root = source_root();
    const auto script = read_text_file(root / "scripts" / "run_boundary_trim_fill_experiments.ps1");

    assert(script.find("$OverCoverWidth") == std::string::npos);
    assert(script.find("$OverCoverRings") == std::string::npos);
    assert(script.find("--b2-over-cover-strip") == std::string::npos);
    assert(script.find("--candidate-surface-over-cover") != std::string::npos);
    assert(script.find("--candidate-over-cover-width") != std::string::npos);
    assert(script.find("--candidate-over-cover-rings") != std::string::npos);
    assert(script.find("--candidate-over-cover-disable-outer-rings") != std::string::npos);
    assert(script.find("--candidate-over-cover-body-bridge-mode") != std::string::npos);
    assert(script.find("--candidate-over-cover-smooth-strip") != std::string::npos);
    assert(script.find("--candidate-over-cover-smooth-transition") != std::string::npos);
    assert(script.find("--candidate-over-cover-disable-body-bridge") != std::string::npos);
    assert(script.find("--geomagic-no-sharpen-contours") != std::string::npos);
    assert(script.find("EnableAuxiliarySupportCollar") != std::string::npos);
    assert(script.find("if ($EnableAuxiliarySupportCollar)") != std::string::npos);
    assert(script.find("--support-collar-width") != std::string::npos);
    assert(script.find("--support-collar-rings") != std::string::npos);
    assert(script.find("--adaptive-support-collar-width") != std::string::npos);
}

void test_route2_runner_exports_compact_artifact_directory() {
    const auto root = source_root();
    const auto script = read_text_file(root / "scripts" / "run_boundary_trim_fill_experiments.ps1");

    assert(script.find("route2_artifacts") != std::string::npos);
    assert(script.find("$runContext.directories.artifacts") != std::string::npos);
    assert(script.find("$runContext.directories.diagnostics") != std::string::npos);
    assert(script.find("$runContext.directories.logs") != std::string::npos);
    assert(script.find("ROUTE2_SUMMARY.md") != std::string::npos);
    assert(script.find("GEOMAGIC_RUN.md") != std::string::npos);
    assert(script.find("crop_stl_candidate_") != std::string::npos);
    assert(script.find("raw_geomagic_crop_stp_candidate_") != std::string::npos);
    assert(script.find("normalized_patch_mm.stp") != std::string::npos);
    assert(script.find("base_removed_candidate.stp") != std::string::npos);
    assert(script.find("occt_applied_merged.stp") != std::string::npos);
    assert(script.find("creo_api_merged.stp") != std::string::npos);
    assert(script.find("geomagic_fit_region.log") != std::string::npos);
    assert(script.find("candidate_surface_over_cover_diagnostics") != std::string::npos);
    assert(script.find("geomagic_generation.stdout.txt") != std::string::npos);
    assert(script.find("retained_route2_inputs") == std::string::npos);
    assert(script.find("artifact_inventory.json") == std::string::npos);
}

void test_run_artifact_layout_and_manifest_contract() {
    const auto root = source_root();
    const auto helperPath = root / "scripts" / "run_artifacts.ps1";
    const auto listPath = root / "scripts" / "list_runs.ps1";
    assert(std::filesystem::exists(helperPath));
    assert(std::filesystem::exists(listPath));

    const auto helper = read_text_file(helperPath);
    const auto list = read_text_file(listPath);
    const auto testScript = read_text_file(root / "scripts" / "test.ps1");
    const auto cornerScript = read_text_file(root / "scripts" / "run_corner_baseline_gate.ps1");
    const auto boundaryScript = read_text_file(root / "scripts" / "run_boundary_trim_fill_experiments.ps1");
    const auto creoDiagnostic = read_text_file(root / "scripts" / "run_creo_step_diagnostic.ps1");
    const auto creoBaseline = read_text_file(root / "scripts" / "run_creo_toolkit_baseline_probe.ps1");
    const auto creoPhase1 = read_text_file(root / "scripts" / "run_creo_toolkit_phase1_repair.ps1");
    const auto probe = read_text_file(root / "tools" / "corner_baseline_probe.cpp");

    assert(helper.find("data\\runs\\$runId") != std::string::npos);
    assert(helper.find("yyyyMMdd_HHmmss_fff") != std::string::npos);
    assert(helper.find("cand-{0:D4}") != std::string::npos);
    assert(helper.find("resolved_candidate_token") != std::string::npos);
    assert(helper.find("run.json") != std::string::npos);
    assert(helper.find("RUN.md") != std::string::npos);
    assert(helper.find("artifacts") != std::string::npos);
    assert(helper.find("diagnostics") != std::string::npos);
    assert(helper.find("logs") != std::string::npos);
    assert(helper.find("rev-parse HEAD") != std::string::npos);
    assert(helper.find("status --porcelain") != std::string::npos);
    assert(helper.find("System.Security.Cryptography.SHA256") != std::string::npos);

    assert(list.find("run.json") != std::string::npos);
    assert(list.find("CandidateId") != std::string::npos);
    assert(list.find("Workflow") != std::string::npos);
    assert(list.find("Status") != std::string::npos);
    assert(list.find("Latest") != std::string::npos);
    assert(list.find("resolved_candidate_token") != std::string::npos);

    for (const auto* script : {&testScript, &cornerScript, &boundaryScript, &creoDiagnostic, &creoBaseline, &creoPhase1}) {
        assert(script->find("run_artifacts.ps1") != std::string::npos);
        assert(script->find("New-SpoRunContext") != std::string::npos);
        assert(script->find("Complete-SpoRun") != std::string::npos);
    }

    assert(testScript.find("build.log") != std::string::npos);
    assert(testScript.find("ctest.log") != std::string::npos);
    assert(cornerScript.find("data\\baseline_runs\\scripted_") == std::string::npos);
    assert(boundaryScript.find("data\\baseline_runs\\boundary_trim_fill_experiments") == std::string::npos);
    assert(probe.find("repo_root() / \"data\" / \"runs\"") != std::string::npos);
    assert(probe.find("options.outputDir / \"artifacts\"") != std::string::npos);
    assert(probe.find("options.outputDir / \"diagnostics\"") != std::string::npos);
}

void test_native_surface_fit_tier_runner_cleans_incomplete_invocation_runs() {
    const auto root = source_root();
    const auto script = read_text_file(root / "scripts" / "run_native_surface_fit_tiers.ps1");

    assert(script.find("$createdRunDirectories = @()") != std::string::npos);
    assert(script.find("$createdRunDirectories += $runContext.output_dir") != std::string::npos);
    assert(script.find("$createdRunDirectories += $comparisonContext.output_dir") != std::string::npos);
    assert(script.find("[System.StringComparison]::OrdinalIgnoreCase") != std::string::npos);
    assert(script.find("Refusing to remove an incomplete run outside data/runs") != std::string::npos);
    assert(script.find("Remove-Item -LiteralPath $resolvedRunDirectory -Recurse -Force") !=
        std::string::npos);
    assert(script.find("[Parameter(Mandatory = $true)][string]$SourceStep") != std::string::npos);
    assert(script.find("[int]$CandidateId") != std::string::npos);
}

void test_native_surface_fit_probe_reports_nsr_1_2_diagnostics_contract() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "native_surface_fit_probe.cpp");

    for (const auto* required : {
             "filling_face_brep_check_valid",
             "oriented_face_brep_check_valid",
             "step_roundtrip_brep_check_valid",
             "subshape_statuses",
             "contextual_statuses",
             "curve_on_surface_max_deviation",
             "nearest_training_distance",
             "normal_error_samples",
             "surface_normal_error_samples_csv"}) {
        assert(source.find(required) != std::string::npos);
    }
    assert(source.find("ShapeFix") == std::string::npos);
    assert(source.find("BRepBuilderAPI_Sewing") == std::string::npos);
    assert(source.find("--source-step <model.stp>") != std::string::npos);
    assert(source.find("--candidate-id <id|auto>") != std::string::npos);
}

void test_native_surface_fit_sampling_ab_runner_has_fixed_dynamic_contract() {
    const auto root = source_root();
    const auto script = read_text_file(root / "scripts" / "run_native_surface_fit_sampling_ab.ps1");

    for (const auto* required : {
             "area_weighted",
             "face_stratified_area_weighted",
             "$trainingSamples = 96",
             "$validationSamples = 192",
             "$boundarySamplesPerEdge = 32",
             "$jacobianGridSize = 16",
             "$tolerance3d = 0.03",
             "$normalToleranceDegrees = 15.0",
             "surface_normal_error_samples.csv",
             "per_face_training_zero_count",
             "filling_face_brep_check_valid",
             "oriented_face_brep_check_valid",
             "step_roundtrip_brep_check_valid",
             "native_surface_fit_sampling_ab.csv",
             "$createdRunDirectories = @()",
             "Refusing to remove an incomplete run outside data/runs"}) {
        assert(script.find(required) != std::string::npos);
    }
    assert(script.find("[Parameter(Mandatory = $true)][string]$SourceStep") != std::string::npos);
    assert(script.find("[int]$CandidateId") != std::string::npos);
}

void test_native_surface_fit_local_g1_runner_and_probe_contract() {
    const auto root = source_root();
    const auto script = read_text_file(
        root / "scripts" / "run_native_surface_fit_local_g1_tiers.ps1");
    const auto probe = read_text_file(root / "tools" / "native_surface_fit_probe.cpp");
    const auto refitter = read_text_file(root / "src" / "merge" / "SurfaceRefitter.cpp");
    const auto tests = read_text_file(root / "tests" / "test_surface_refitter.cpp");

    for (const auto* required : {
             "$trainingSamples = 96",
             "$selectionSamples = 192",
             "$holdoutSamples = 192",
             "$g1Tiers = @(0, 1, 4, 8)",
             "$boundarySamplesPerEdge = 32",
             "$jacobianGridSize = 16",
             "$tolerance3d = 0.03",
             "$normalToleranceDegrees = 15.0",
             "--local-g1-count",
             "configured_g1_count",
             "actual_g1_count",
             "training_selection_overlap_count",
             "training_holdout_overlap_count",
             "selection_holdout_overlap_count",
             "interior_selection_error",
             "interior_holdout_error",
             "normal_selection_error",
             "normal_holdout_error",
             "native_surface_fit_local_g1_tiers.csv",
             "native_surface_fit_local_g1_tiers.md",
             "$runnerFailures +=",
             "New-FailureComparisonRow",
             "artifacts and comparison were retained"}) {
        assert(script.find(required) != std::string::npos);
    }
    assert(script.find("[Parameter(Mandatory = $true)][string]$SourceStep") != std::string::npos);
    assert(script.find("[int]$CandidateId") != std::string::npos);
    assert(script.find("Remove-Item") == std::string::npos);

    for (const auto* required : {
             "selection_sample_count",
             "holdout_sample_count",
             "configured_g1_count",
             "actual_g1_count",
             "local_g1_constraints",
             "sample_sets_disjoint",
             "support_face_available",
             "support_point_distance",
             "interior_selection_error",
             "interior_holdout_error",
             "normal_selection_error",
             "normal_holdout_error",
             "legacy_alias_for_selection"}) {
        assert(probe.find(required) != std::string::npos);
    }
    assert(refitter.find("report.holdout_interior_error.projection_failure_count") != std::string::npos);
    assert(refitter.find("report.holdout_normal_error.undefined_count") != std::string::npos);
    assert(refitter.find("filling.Add(sample.u, sample.v, sample.support_face, GeomAbs_G1)") !=
        std::string::npos);

    const auto real_candidate = std::string("17") + "9";
    const auto first_real_face = std::string("170") + "5";
    const auto second_real_face = std::string("170") + "9";
    for (const auto& forbidden : {
             std::string("candidate ") + real_candidate,
             std::string("candidate_") + real_candidate,
             std::string("face ") + first_real_face,
             std::string("face ") + second_real_face}) {
        assert(script.find(forbidden) == std::string::npos);
        assert(probe.find(forbidden) == std::string::npos);
        assert(refitter.find(forbidden) == std::string::npos);
        assert(tests.find(forbidden) == std::string::npos);
    }
}

void test_gui_exposes_candidate_surface_over_cover_switch() {
    const auto root = source_root();
    const auto header = read_text_file(root / "src" / "app" / "MainWindow.h");
    const auto source = read_text_file(root / "src" / "app" / "MainWindow.cpp");

    assert(header.find("useStpSupportCollarAction_") != std::string::npos);
    assert(source.find("启用 STP 候选面外扩带") != std::string::npos);
    assert(source.find("enableCandidateSurfaceOverCover") != std::string::npos);
    assert(source.find("candidateSurfaceOverCoverWidth = 0.25") != std::string::npos);
    assert(source.find("candidateSurfaceOverCoverRingCount = 3") != std::string::npos);
    assert(source.find("useStpSupportCollarAction_->isChecked()") != std::string::npos);
}

void test_gui_uses_native_surface_reduction_as_primary_fitting_workflow() {
    const auto root = source_root();
    const auto state = read_text_file(root / "src" / "app" / "MainWindowWorkflowState.h");
    const auto header = read_text_file(root / "src" / "app" / "MainWindow.h");
    const auto source = read_text_file(root / "src" / "app" / "MainWindow.cpp");

    assert(state.find("enum class SurfaceReductionInputMode") != std::string::npos);
    assert(state.find("SurfaceReductionInputMode::StepBrepOnly") != std::string::npos);
    assert(header.find("surfaceReductionStepOnlyAction_") != std::string::npos);
    assert(header.find("surfaceReductionScanReferenceAction_") != std::string::npos);
    assert(source.find("仅 STEP B-rep") != std::string::npos);
    assert(source.find("STEP B-rep + 扫描 STL 软参考") != std::string::npos);
    assert(source.find("menuBar()->addMenu(\"曲面减面\")") != std::string::npos);
    assert(source.find("addMenuButton(\"曲面减面\", reductionToolMenu)") != std::string::npos);
    assert(source.find("Geomagic 后备（旧流程）") != std::string::npos);
    assert(source.find("addMenuButton(\"Patch\"") == std::string::npos);
    assert(source.find("execution.fitWorkerCount = 4") != std::string::npos);
    assert(source.find("reduceSelectedFacesData(documentSnapshot, request, execution)") != std::string::npos);
    assert(source.find(
        "inputMode == SurfaceReductionInputMode::StepBrepWithScanReference") != std::string::npos);
}

void test_corner_baseline_probe_exports_applied_step_for_acceptance() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "corner_baseline_probe.cpp");
    const auto script = read_text_file(root / "scripts" / "run_corner_baseline_gate.ps1");

    assert(source.find("#include \"io/StepWriter.h\"") != std::string::npos);
    assert(source.find("applied_step_export") != std::string::npos);
    assert(source.find("readback_success") != std::string::npos);
    assert(source.find("StepWriter().write") != std::string::npos);
    assert(source.find("StepReader().read") != std::string::npos);
    assert(script.find("applied_step_export") != std::string::npos);
    assert(script.find("readback_success") != std::string::npos);
    assert(source.find("failed_quality_gate") != std::string::npos);
    assert(script.find("failed_quality_gate") != std::string::npos);
}

void test_corner_baseline_probe_reports_b2_8_external_cad_diagnostics_route() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "corner_baseline_probe.cpp");

    assert(source.find("external_cad_diagnostics") != std::string::npos);
    assert(source.find("raw_patch_preflight_role") != std::string::npos);
    assert(source.find("final_applied_step_diagnostic_eligible") != std::string::npos);
    assert(source.find("final_applied_step_diagnostic_skipped_reason") != std::string::npos);
    assert(source.find("AppliedStepAfterSuccessfulApply") != std::string::npos);
}

void test_creo_step_diagnostic_script_is_optional_background_runner() {
    const auto root = source_root();
    const auto script = read_text_file(root / "scripts" / "run_creo_step_diagnostic.ps1");

    assert(script.find("param(") != std::string::npos);
    assert(script.find("[string]$StepPath") != std::string::npos);
    assert(script.find("[string]$CreoRoot") != std::string::npos);
    assert(script.find("dbatchc.exe") != std::string::npos);
    assert(script.find("-nographics") != std::string::npos);
    assert(script.find("-process") != std::string::npos);
    assert(script.find("DSQM=\"_LOCAL\"") != std::string::npos);
    assert(script.find("MC_BATCH_REPORT_DIR") != std::string::npos);
    assert(script.find("step_3d_import.ttd") != std::string::npos);
    assert(script.find("modelcheck.ttd") != std::string::npos);
    assert(script.find("creo_step_diagnostic_result.json") != std::string::npos);
    assert(script.find("generated_prt") != std::string::npos);
    assert(script.find("*.prt.*") != std::string::npos);
    assert(script.find("@(Find-CreoPrtFiles") != std::string::npos);
    assert(script.find("CreoStepImportNoPrt") != std::string::npos);
    assert(script.find("CreoModelCheckReportReady") != std::string::npos);
    assert(script.find("Get-ModelCheckSummary") != std::string::npos);
    assert(script.find("diagnostic_passed") != std::string::npos);
    assert(script.find("error_count") != std::string::npos);
    assert(script.find("warning_count") != std::string::npos);
    assert(script.find("key_checks") != std::string::npos);
    assert(script.find("import_validation") != std::string::npos);
    assert(script.find("PTC_VAL_IMP_SCORE") != std::string::npos);
    assert(script.find("PTC_VAL_IMP_PART_STATUS") != std::string::npos);
    assert(script.find("GEOM_CHECKS") != std::string::npos);
    assert(script.find("SHORT_EDGES") != std::string::npos);
    assert(script.find("candidate_0179") == std::string::npos);
    assert(script.find("03_") == std::string::npos);
}

void test_creo_modelcheck_parse_only_exports_item_details_and_correlation() {
    const auto root = source_root();
    const auto outputRoot = temp_root("spo_creo_modelcheck_b2_9_fixture");
    const auto scriptPath = root / "scripts" / "run_creo_step_diagnostic.ps1";
    const auto fixturePath = root / "tests" / "fixtures" / "creo_modelcheck_b2_9.xml";

    std::ostringstream command;
    command << "powershell -NoProfile -ExecutionPolicy Bypass -File "
            << quote_for_command(scriptPath)
            << " -ParseModelCheckOnly"
            << " -ModelCheckXmlPath " << quote_for_command(fixturePath)
            << " -OutputDir " << quote_for_command(outputRoot)
            << " -ShortEdgeItemSampleLimit 2";

    const int exitCode = std::system(command.str().c_str());
    assert(exitCode == 0);

    const auto json = read_text_file(outputRoot / "creo_step_diagnostic_result.json");
    const auto runJson = read_text_file(outputRoot / "run.json");
    const auto compactJson = without_ascii_space(json);
    assert(compactJson.find("\"status\":\"CreoModelCheckParseOnly\"") != std::string::npos);
    assert(json.find("\"items\"") != std::string::npos);
    assert(compactJson.find("\"creo_edge_id\":\"18956\"") != std::string::npos);
    assert(compactJson.find("\"creo_feature_id\":\"4\"") != std::string::npos);
    assert(json.find("\"creo_diagnostic_correlation\"") != std::string::npos);
    assert(json.find("\"failed_checks\"") != std::string::npos);
    assert(compactJson.find("\"short_edge_items_truncated\":true") != std::string::npos);
    assert(json.find("\"imported_feature_ids\"") != std::string::npos);
    assert(json.find("\"import_validation\"") != std::string::npos);
    assert(compactJson.find("\"modelcheck_spatial_mapping_status\":\"CreoIdsOnlyNoCoordinates\"") != std::string::npos);
    assert(compactJson.find("\"occt_edge_mapping_available\":false") != std::string::npos);
    assert(std::filesystem::exists(outputRoot / "RUN.md"));
    assert(std::filesystem::exists(outputRoot / "artifacts"));
    assert(std::filesystem::exists(outputRoot / "diagnostics"));
    assert(std::filesystem::exists(outputRoot / "logs"));
    assert(without_ascii_space(runJson).find("\"workflow\":\"creo-modelcheck\"") != std::string::npos);
    assert(without_ascii_space(runJson).find("\"status\":\"passed\"") != std::string::npos);
}

void test_creo_toolkit_baseline_probe_contract_is_optional() {
    const auto root = source_root();
    const auto scriptPath = root / "scripts" / "run_creo_toolkit_baseline_probe.ps1";
    const auto sourcePath = root / "tools" / "creo_toolkit_baseline_probe" / "creo_toolkit_baseline_probe.cpp";

    assert(std::filesystem::exists(scriptPath));
    assert(std::filesystem::exists(sourcePath));

    const auto script = read_text_file(scriptPath);
    const auto source = read_text_file(sourcePath);

    assert(script.find("[string]$StepPath") != std::string::npos);
    assert(script.find("$UseLatestAppliedStep") != std::string::npos);
    assert(script.find("*_applied.stp") != std::string::npos);
    assert(script.find("protoolkit") != std::string::npos);
    assert(script.find("creo_toolkit_baseline_result.json") != std::string::npos);
    assert(script.find("candidate_0179") == std::string::npos);
    assert(script.find("03_") == std::string::npos);

    assert(source.find("ProEngineerStart") != std::string::npos);
    assert(source.find("ProIntfimportModelWithOptionsMdlnameCreate") != std::string::npos);
    assert(source.find("PRO_INTF_IMPORT_STEP") != std::string::npos);
    assert(source.find("ProIntfimportValidationscoreGet") != std::string::npos);
    assert(source.find("ProModelcheckExecute") != std::string::npos);
    assert(source.find("ProIntf3DFileWriteWithDefaultProfile") != std::string::npos);
    assert(source.find("short_edges_diagnostic_only") != std::string::npos);
    assert(source.find("SHORT_EDGES") != std::string::npos);
}

void test_creo_modelcheck_parse_only_handles_toolkit_xml_shape() {
    const auto root = source_root();
    const auto outputRoot = temp_root("spo_creo_modelcheck_toolkit_b0_fixture");
    const auto scriptPath = root / "scripts" / "run_creo_step_diagnostic.ps1";
    const auto fixturePath = root / "tests" / "fixtures" / "creo_modelcheck_toolkit_b0.xml";

    std::ostringstream command;
    command << "powershell -NoProfile -ExecutionPolicy Bypass -File "
            << quote_for_command(scriptPath)
            << " -ParseModelCheckOnly"
            << " -ModelCheckXmlPath " << quote_for_command(fixturePath)
            << " -OutputDir " << quote_for_command(outputRoot)
            << " -ShortEdgeItemSampleLimit 2";

    const int exitCode = std::system(command.str().c_str());
    assert(exitCode == 0);

    const auto json = read_text_file(outputRoot / "creo_step_diagnostic_result.json");
    const auto runJson = read_text_file(outputRoot / "run.json");
    const auto compactJson = without_ascii_space(json);
    assert(compactJson.find("\"error_count\":2") != std::string::npos);
    assert(compactJson.find("\"warning_count\":1") != std::string::npos);
    assert(compactJson.find("\"name\":\"GEOM_CHECKS\"") != std::string::npos);
    assert(compactJson.find("\"name\":\"SHORT_EDGES\"") != std::string::npos);
    assert(compactJson.find("\"short_edge_item_count\":2") != std::string::npos);
    assert(compactJson.find("\"PTC_VAL_IMP_PART_STATUS\":\"SOLID_FAILED\"") != std::string::npos);
    assert(compactJson.find("\"PTC_VAL_IMP_SCORE\":\"FAIL\"") != std::string::npos);
    assert(std::filesystem::exists(outputRoot / "RUN.md"));
    assert(without_ascii_space(runJson).find("\"status\":\"passed\"") != std::string::npos);
}

void test_creo_toolkit_phase1_repair_contract_uses_fresh_dual_inputs() {
    const auto root = source_root();
    const auto scriptPath = root / "scripts" / "run_creo_toolkit_phase1_repair.ps1";
    const auto inputExporterPath = root / "tools" / "creo_phase1_input_exporter.cpp";
    const auto toolkitProbePath =
        root / "tools" / "creo_toolkit_phase1_repair_probe" / "creo_toolkit_phase1_repair_probe.cpp";

    assert(std::filesystem::exists(scriptPath));
    assert(std::filesystem::exists(inputExporterPath));
    assert(std::filesystem::exists(toolkitProbePath));

    const auto script = read_text_file(scriptPath);
    const auto inputExporter = read_text_file(inputExporterPath);
    const auto toolkitProbe = read_text_file(toolkitProbePath);

    assert(script.find("-RealGeomagic") != std::string::npos);
    assert(script.find("run_corner_baseline_gate.ps1") != std::string::npos);
    assert(script.find("base_removed_candidate.stp") != std::string::npos);
    assert(script.find("geomagic_patch.stp") != std::string::npos);
    assert(script.find("creo_phase1_input_exporter") != std::string::npos);
    assert(script.find("creo_toolkit_phase1_repair_probe") != std::string::npos);
    assert(script.find("run_creo_step_diagnostic.ps1") != std::string::npos);
    assert(script.find("-ParseModelCheckOnly") != std::string::npos);
    assert(script.find("step_stats") != std::string::npos);
    assert(script.find("StrictTopologyGate") != std::string::npos);
    assert(script.find("phase1_result.json") != std::string::npos);
    assert(script.find("*_applied.stp") == std::string::npos);
    assert(script.find("UseLatestAppliedStep") == std::string::npos);

    assert(inputExporter.find("BRepTools_ReShape") != std::string::npos);
    assert(inputExporter.find("Remove(") != std::string::npos);
    assert(inputExporter.find("base_removed_candidate") != std::string::npos);
    assert(inputExporter.find("--base-removed-output") != std::string::npos);

    assert(toolkitProbe.find("ProImportfeatCreate") != std::string::npos);
    assert(toolkitProbe.find("ProImportfeatAttr") != std::string::npos);
    assert(toolkitProbe.find("join_surfaces") != std::string::npos);
    assert(toolkitProbe.find("attempt_make_solid") != std::string::npos);
    assert(toolkitProbe.find("ProModelcheckExecute") != std::string::npos);
    assert(toolkitProbe.find("ProIntf3DFileWriteWithDefaultProfile") != std::string::npos);
    assert(toolkitProbe.find("short_edges_diagnostic_only") != std::string::npos);
}

}

void run_scripted_baseline_gate_tests() {
    test_corner_baseline_probe_supports_auto_candidate_selection();
    test_scripted_baseline_gate_is_not_hardcoded_to_one_sample();
    test_verify_real_geomagic_runs_scripted_baseline_gate();
    test_long_running_tests_are_split_into_explicit_suites();
    test_boundary_runner_avoids_duplicate_generation_and_validation_work();
    test_corner_baseline_probe_reports_b2_2_support_collar_fields();
    test_corner_baseline_probe_exports_candidate_over_cover_rejection_artifacts();
    test_corner_baseline_probe_reports_b2_3_corner_safe_and_sharpen_fields();
    test_route2_runner_uses_candidate_over_cover_as_default_expansion();
    test_route2_runner_exports_compact_artifact_directory();
    test_run_artifact_layout_and_manifest_contract();
    test_native_surface_fit_tier_runner_cleans_incomplete_invocation_runs();
    test_native_surface_fit_probe_reports_nsr_1_2_diagnostics_contract();
    test_native_surface_fit_sampling_ab_runner_has_fixed_dynamic_contract();
    test_native_surface_fit_local_g1_runner_and_probe_contract();
    test_gui_exposes_candidate_surface_over_cover_switch();
    test_gui_uses_native_surface_reduction_as_primary_fitting_workflow();
    test_corner_baseline_probe_exports_applied_step_for_acceptance();
    test_corner_baseline_probe_reports_b2_8_external_cad_diagnostics_route();
    test_creo_step_diagnostic_script_is_optional_background_runner();
    test_creo_modelcheck_parse_only_exports_item_details_and_correlation();
    test_creo_toolkit_baseline_probe_contract_is_optional();
    test_creo_modelcheck_parse_only_handles_toolkit_xml_shape();
    test_creo_toolkit_phase1_repair_contract_uses_fresh_dual_inputs();
}
