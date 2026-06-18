#include <cassert>
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
    assert(script.find("--b2-boundary-guard-band") != std::string::npos);
    assert(script.find("B2.1") != std::string::npos);
    assert(script.find("B2.2") != std::string::npos);
    assert(script.find("B2.3") != std::string::npos);
    assert(script.find("--b2-over-cover-strip") != std::string::npos);
    assert(script.find("--over-cover-width") != std::string::npos);
    assert(script.find("--b2-adjacent-face-support-collar") != std::string::npos);
    assert(script.find("--b2-corner-safe-support-collar") != std::string::npos);
    assert(script.find("-SharpenContours") != std::string::npos);
    assert(script.find("-RealGeomagic") != std::string::npos);
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

void test_corner_baseline_probe_reports_b2_1_over_cover_fields() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "corner_baseline_probe.cpp");

    assert(source.find("--b2-over-cover-strip") != std::string::npos);
    assert(source.find("--over-cover-width") != std::string::npos);
    assert(source.find("boundary_over_cover_strip_enabled") != std::string::npos);
    assert(source.find("boundary_over_cover_triangle_count") != std::string::npos);
    assert(source.find("boundary_over_cover_boundary_coverage") != std::string::npos);
}

void test_corner_baseline_probe_reports_b2_2_support_collar_fields() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "corner_baseline_probe.cpp");

    assert(source.find("--b2-adjacent-face-support-collar") != std::string::npos);
    assert(source.find("--support-collar-width") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_enabled") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_triangle_count") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_boundary_coverage") != std::string::npos);
    assert(source.find("seam_continuity") != std::string::npos);
}

void test_corner_baseline_probe_reports_b2_3_corner_safe_and_sharpen_fields() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "corner_baseline_probe.cpp");

    assert(source.find("--b2-corner-safe-support-collar") != std::string::npos);
    assert(source.find("--support-collar-max-offset-scale") != std::string::npos);
    assert(source.find("--geomagic-sharpen-contours") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_corner_clamp_enabled") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_corner_clamp_count") != std::string::npos);
    assert(source.find("adjacent_face_support_collar_max_offset") != std::string::npos);
    assert(source.find("geomagic_sharpen_contours") != std::string::npos);
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

}

void run_scripted_baseline_gate_tests() {
    test_corner_baseline_probe_supports_auto_candidate_selection();
    test_scripted_baseline_gate_is_not_hardcoded_to_one_sample();
    test_verify_real_geomagic_runs_scripted_baseline_gate();
    test_corner_baseline_probe_reports_b2_1_over_cover_fields();
    test_corner_baseline_probe_reports_b2_2_support_collar_fields();
    test_corner_baseline_probe_reports_b2_3_corner_safe_and_sharpen_fields();
    test_corner_baseline_probe_exports_applied_step_for_acceptance();
    test_corner_baseline_probe_reports_b2_8_external_cad_diagnostics_route();
    test_creo_step_diagnostic_script_is_optional_background_runner();
}
