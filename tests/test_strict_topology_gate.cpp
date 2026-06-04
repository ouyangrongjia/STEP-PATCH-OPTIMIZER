#include "brep/ShapeDocument.h"
#include "patch/PatchReplacementReport.h"
#include "validate/StrictTopologyGate.h"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <filesystem>
#include <string>

namespace {

TopoDS_Shape make_box(double dx = 10.0, double dy = 20.0, double dz = 30.0) {
    return BRepPrimAPI_MakeBox(dx, dy, dz).Shape();
}

TopoDS_Shape make_open_face() {
    const gp_Pnt p00(0.0, 0.0, 0.0);
    const gp_Pnt p10(10.0, 0.0, 0.0);
    const gp_Pnt p11(10.0, 10.0, 0.0);
    const gp_Pnt p01(0.0, 10.0, 0.0);

    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p00, p10).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p10, p11).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p11, p01).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p01, p00).Edge());
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}

std::filesystem::path temp_step_path(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

spo::StrictTopologyGateReport evaluate(
    const spo::ShapeDocument& before,
    const spo::ShapeDocument& after,
    const spo::PatchReplacementReport* replacementReport = nullptr,
    const std::filesystem::path& stepPath = {}) {
    spo::StrictTopologyGateInput input;
    input.beforeDocument = &before;
    input.afterDocument = &after;
    input.replacementReport = replacementReport;
    input.temporaryStepPath = stepPath;
    return spo::StrictTopologyGate().evaluate(input);
}

void test_identical_valid_box_passes() {
    const spo::ShapeDocument before(make_box(), {});
    const spo::ShapeDocument after(make_box(), {});

    const auto report = evaluate(before, after);

    assert(report.passed);
    assert(report.failureReason == spo::StrictTopologyFailureReason::None);
    assert(report.beforeBRepCheckValid);
    assert(report.afterBRepCheckValid);
    assert(report.beforeFreeEdges == 0);
    assert(report.afterFreeEdges == 0);
    assert(report.stepExportOk);
    assert(report.stepRoundtripOk);
    assert(!report.warningMessage.empty());
}

void test_after_missing_shape_fails() {
    const spo::ShapeDocument before(make_box(), {});
    const spo::ShapeDocument after;

    const auto report = evaluate(before, after);

    assert(!report.passed);
    assert(report.failureReason == spo::StrictTopologyFailureReason::MissingAfterShape);
    assert(!report.message.empty());
}

void test_free_edge_increase_fails() {
    const spo::ShapeDocument before(make_box(), {});
    const spo::ShapeDocument after(make_open_face(), {});

    const auto report = evaluate(before, after);

    assert(!report.passed);
    assert(report.failureReason == spo::StrictTopologyFailureReason::FreeEdgeIncreased);
    assert(report.afterFreeEdges > report.beforeFreeEdges);
}

void test_multi_face_replacement_is_accepted() {
    const spo::ShapeDocument before(make_box(), {});
    const spo::ShapeDocument after(make_box(), {});
    spo::PatchReplacementReport replacement;
    replacement.success = true;
    replacement.replacementFaceCount = 12;

    const auto report = evaluate(before, after, &replacement);

    assert(report.passed);
    assert(report.multiFaceReplacement);
    assert(report.replacementFaceCount == 12);
    assert(report.warningMessage.find("multi-face") != std::string::npos);
}

void test_multi_face_replacement_can_be_disallowed() {
    const spo::ShapeDocument before(make_box(), {});
    const spo::ShapeDocument after(make_box(), {});
    spo::PatchReplacementReport replacement;
    replacement.success = true;
    replacement.replacementFaceCount = 12;

    spo::StrictTopologyGateInput input;
    input.beforeDocument = &before;
    input.afterDocument = &after;
    input.replacementReport = &replacement;
    input.allowMultiFaceReplacement = false;

    const auto report = spo::StrictTopologyGate().evaluate(input);

    assert(!report.passed);
    assert(report.failureReason == spo::StrictTopologyFailureReason::ReplacementStatsInvalid);
}

void test_face_count_not_reduced_warns_without_failing() {
    const spo::ShapeDocument before(make_box(), {});
    const spo::ShapeDocument after(make_box(), {});

    const auto report = evaluate(before, after);

    assert(report.passed);
    assert(report.afterStats.faces >= report.beforeStats.faces);
    assert(report.warningMessage.find("Face count did not decrease") != std::string::npos);
}

void test_step_export_and_roundtrip_use_requested_path() {
    const spo::ShapeDocument before(make_box(), {});
    const spo::ShapeDocument after(make_box(), {});
    const auto path = temp_step_path("spo_strict_topology_gate_roundtrip.stp");
    std::filesystem::remove(path);

    const auto report = evaluate(before, after, nullptr, path);

    assert(report.passed);
    assert(report.stepExportOk);
    assert(report.stepRoundtripOk);
    assert(std::filesystem::exists(path));

    std::filesystem::remove(path);
}

}

void run_strict_topology_gate_tests() {
    test_identical_valid_box_passes();
    test_after_missing_shape_fails();
    test_free_edge_increase_fails();
    test_multi_face_replacement_is_accepted();
    test_multi_face_replacement_can_be_disallowed();
    test_face_count_not_reduced_warns_without_failing();
    test_step_export_and_roundtrip_use_requested_path();
}
