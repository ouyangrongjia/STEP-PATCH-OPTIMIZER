void run_step_io_tests();
void run_topology_graph_tests();
void run_feature_edges_tests();
void run_command_tests();
void run_same_domain_merge_tests();
void run_merge_planner_tests();
void run_region_merge_stub_tests();
void run_plane_region_merger_tests();
void run_region_boundary_analyzer_tests();
void run_plane_region_merge_command_tests();
void run_face_inspect_tests();
void run_analytic_candidate_detection_tests();
void run_candidate_type_statistics_tests();
void run_feature_bounded_region_builder_tests();
void run_boundary_wire_builder_tests();
void run_sphere_region_merger_tests();
void run_sphere_region_merge_command_tests();
void run_stl_io_tests();
void run_stl_mesh_tests();
void run_stl_region_extractor_tests();
void run_geomagic_output_path_resolver_tests();
void run_geomagic_autosurface_config_tests();
void run_geomagic_backend_mock_tests();
void run_geomagic_pipeline_script_tests();
void run_patch_import_service_tests();
void run_patch_import_service_real_tests();
void run_patch_artifact_locator_tests();
void run_patch_preview_report_tests();
void run_patch_apply_state_tests();
void run_multiface_patch_analyzer_tests();
void run_strict_topology_gate_tests();
void run_commercial_cad_quality_gate_tests();
void run_boundary_constrained_patch_builder_tests();
void run_crop_boundary_diagnostics_tests();
void run_patch_replacement_repair_tests();
void run_patch_replacement_command_tests();
void run_stp_sampled_fitting_mesh_tests();
void run_stl_cut_chain_cutter_tests();

#include "brep/ShapeDocument.h"
#include "validate/ShapeValidator.h"

#include <cassert>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace {

void configure_test_process_error_reporting() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif

#if defined(_MSC_VER)
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

}

void run_validation_tests() {
    const spo::ShapeDocument document;
    const spo::ShapeValidator validator;
    const auto report = validator.validate(document);
    assert(!report.has_shape);
    assert(report.free_edges == 0);
    assert(report.multiple_edges == 0);
    assert(!report.brep_check_valid);
}

int main() {
    configure_test_process_error_reporting();

    run_step_io_tests();
    run_topology_graph_tests();
    run_feature_edges_tests();
    run_command_tests();
    run_same_domain_merge_tests();
    run_merge_planner_tests();
    run_region_merge_stub_tests();
    run_plane_region_merger_tests();
    run_region_boundary_analyzer_tests();
    run_plane_region_merge_command_tests();
    run_face_inspect_tests();
    run_analytic_candidate_detection_tests();
    run_candidate_type_statistics_tests();
    run_feature_bounded_region_builder_tests();
    run_boundary_wire_builder_tests();
    run_sphere_region_merger_tests();
    run_sphere_region_merge_command_tests();
    run_stl_io_tests();
    run_stl_mesh_tests();
    run_stl_region_extractor_tests();
    run_geomagic_output_path_resolver_tests();
    run_geomagic_autosurface_config_tests();
    run_geomagic_backend_mock_tests();
    run_geomagic_pipeline_script_tests();
    run_patch_import_service_tests();
    run_patch_import_service_real_tests();
    run_patch_artifact_locator_tests();
    run_patch_preview_report_tests();
    run_patch_apply_state_tests();
    run_multiface_patch_analyzer_tests();
    run_strict_topology_gate_tests();
    run_commercial_cad_quality_gate_tests();
    run_boundary_constrained_patch_builder_tests();
    run_crop_boundary_diagnostics_tests();
    run_patch_replacement_repair_tests();
    run_patch_replacement_command_tests();
    run_stp_sampled_fitting_mesh_tests();
    run_stl_cut_chain_cutter_tests();
    run_validation_tests();
    return 0;
}
