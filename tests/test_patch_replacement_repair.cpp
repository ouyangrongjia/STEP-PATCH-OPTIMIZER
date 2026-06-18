#include "patch/PatchReplacementRepair.h"
#include "patch/PatchReplacementReport.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>

#include <cassert>
#include <cmath>
#include <string>

namespace {

TopoDS_Shape make_box() {
    return BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
}

TopoDS_Shape make_box_shell() {
    const auto box = make_box();
    for (TopExp_Explorer explorer(box, TopAbs_SHELL); explorer.More(); explorer.Next()) {
        return TopoDS::Shell(explorer.Current());
    }
    return {};
}

void test_adaptive_sewing_records_attempts_and_prefers_preferred_tolerance() {
    spo::PatchReplacementRepairOptions options;
    options.preferredSewingTolerance = 0.007;
    options.minSewingTolerance = 0.001;
    options.maxSewingTolerance = 0.01;

    const auto result = spo::repairPatchReplacementShape(make_box(), options);

    assert(result.success);
    assert(result.report.shapeFixShapeApplied);
    assert(result.report.unifySameDomainApplied);
    assert(result.report.sewingApplied);
    assert(result.report.sewingAttemptCount > 1);
    assert(std::abs(result.report.selectedSewingTolerance - 0.007) < 1.0e-9);
    assert(result.report.bestSewingFaceCount > 0);
    assert(result.report.bestSewingEdgeCount > 0);
    assert(result.report.bestSewingSolidCount > 0);
    assert(result.report.bestSewingBRepCheckValid);
    assert(result.report.degeneratedFreeEdgesBeforeRepair == 0);
    assert(result.report.degeneratedFreeEdgesAfterRepair == 0);
    assert(result.report.bestSewingDegeneratedFreeEdgeCount == 0);
    assert(!result.report.bestSewingCollapsed);
}

void test_shell_to_solid_path_generates_solid_for_closed_shell() {
    spo::PatchReplacementRepairOptions options;
    options.runAdaptiveSewing = false;
    options.runSewing = false;
    options.runShellToSolid = true;
    options.runUnifySameDomain = false;

    const auto result = spo::repairPatchReplacementShape(make_box_shell(), options);

    assert(result.success);
    assert(result.report.shellToSolidApplied);
    assert(result.report.solidCountAfterRepair > 0);
}

void test_collapsed_sewing_result_is_reported_as_diagnostic_only() {
    spo::PatchReplacementRepairOptions options;
    options.collapseFaceRatio = 2.0;

    const auto result = spo::repairPatchReplacementShape(make_box(), options);

    assert(result.success);
    assert(result.report.sewingAttemptCount > 1);
    assert(result.report.bestSewingCollapsed);
    assert(result.report.warningMessage.find("collapsed") != std::string::npos);
}

}

void run_patch_replacement_repair_tests() {
    test_adaptive_sewing_records_attempts_and_prefers_preferred_tolerance();
    test_shell_to_solid_path_generates_solid_for_closed_shell();
    test_collapsed_sewing_result_is_reported_as_diagnostic_only();
}
