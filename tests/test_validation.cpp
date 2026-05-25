void run_step_io_tests();
void run_topology_graph_tests();
void run_feature_edges_tests();
void run_command_tests();
void run_same_domain_merge_tests();
void run_merge_planner_tests();
void run_region_merge_stub_tests();
void run_plane_region_merger_tests();
void run_plane_region_merge_command_tests();
void run_face_inspect_tests();

#include "brep/ShapeDocument.h"
#include "validate/ShapeValidator.h"

#include <cassert>

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
    run_step_io_tests();
    run_topology_graph_tests();
    run_feature_edges_tests();
    run_command_tests();
    run_same_domain_merge_tests();
    run_merge_planner_tests();
    run_region_merge_stub_tests();
    run_plane_region_merger_tests();
    run_plane_region_merge_command_tests();
    run_face_inspect_tests();
    run_validation_tests();
    return 0;
}
