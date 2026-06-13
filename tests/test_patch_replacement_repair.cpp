#include "patch/PatchReplacementRepair.h"
#include "patch/PatchReplacementReport.h"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

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

TopoDS_Shape make_loose_face() {
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(50.0, 0.0, 0.0), gp_Pnt(51.0, 0.0, 0.0)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(51.0, 0.0, 0.0), gp_Pnt(51.0, 1.0, 0.0)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(51.0, 1.0, 0.0), gp_Pnt(50.0, 1.0, 0.0)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(50.0, 1.0, 0.0), gp_Pnt(50.0, 0.0, 0.0)).Edge());
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}

TopoDS_Shape make_box_with_loose_face() {
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    builder.Add(compound, make_box());
    builder.Add(compound, make_loose_face());
    return compound;
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

void test_repair_discards_non_solid_leftovers_when_solid_available() {
    spo::PatchReplacementRepairOptions options;
    options.runSewing = false;
    options.runAdaptiveSewing = false;

    const auto result = spo::repairPatchReplacementShape(make_box_with_loose_face(), options);

    assert(result.success);
    assert(result.report.solidCountAfterRepair > 0);
    assert(result.report.freeEdgesAfterRepair == 0);
}

}

void run_patch_replacement_repair_tests() {
    test_adaptive_sewing_records_attempts_and_prefers_preferred_tolerance();
    test_shell_to_solid_path_generates_solid_for_closed_shell();
    test_collapsed_sewing_result_is_reported_as_diagnostic_only();
    test_repair_discards_non_solid_leftovers_when_solid_available();
}
