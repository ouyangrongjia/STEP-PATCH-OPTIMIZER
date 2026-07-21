#include "brep/ShapeDocument.h"
#include "reduction/region_refit/RegionConstraintExtractor.h"
#include "reduction/region_refit/RegionalSurfaceRefitEngine.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>

namespace {

struct TwoFaceFixture {
    spo::ShapeDocument document;
    spo::EdgeId sharedEdge = 0;
};

TwoFaceFixture make_two_face_fixture() {
    const gp_Pnt p00(0.0, 0.0, 0.0);
    const gp_Pnt p10(1.0, 0.0, 0.0);
    const gp_Pnt p20(2.0, 0.0, 0.0);
    const gp_Pnt p01(0.0, 1.0, 0.0);
    const gp_Pnt p11(1.0, 1.0, 0.0);
    const gp_Pnt p21(2.0, 1.0, 0.0);

    const auto bottomLeft = BRepBuilderAPI_MakeEdge(p00, p10).Edge();
    const auto shared = BRepBuilderAPI_MakeEdge(p10, p11).Edge();
    const auto topLeft = BRepBuilderAPI_MakeEdge(p11, p01).Edge();
    const auto left = BRepBuilderAPI_MakeEdge(p01, p00).Edge();
    const auto bottomRight = BRepBuilderAPI_MakeEdge(p10, p20).Edge();
    const auto right = BRepBuilderAPI_MakeEdge(p20, p21).Edge();
    const auto topRight = BRepBuilderAPI_MakeEdge(p21, p11).Edge();

    BRepBuilderAPI_MakeWire leftWire;
    leftWire.Add(bottomLeft);
    leftWire.Add(shared);
    leftWire.Add(topLeft);
    leftWire.Add(left);
    BRepBuilderAPI_MakeWire rightWire;
    rightWire.Add(bottomRight);
    rightWire.Add(right);
    rightWire.Add(topRight);
    rightWire.Add(TopoDS::Edge(shared.Reversed()));

    BRepBuilderAPI_Sewing sewing;
    sewing.Add(BRepBuilderAPI_MakeFace(leftWire.Wire()).Face());
    sewing.Add(BRepBuilderAPI_MakeFace(rightWire.Wire()).Face());
    sewing.Perform();

    TwoFaceFixture fixture;
    fixture.document = spo::ShapeDocument(sewing.SewedShape(), {});
    for (spo::EdgeId edgeId = 0; edgeId < fixture.document.topology().edgeCount(); ++edgeId) {
        const auto* adjacency = fixture.document.topology().adjacencyForEdge(edgeId);
        if (adjacency != nullptr && adjacency->faces.size() == 2) {
            fixture.sharedEdge = edgeId;
            break;
        }
    }
    return fixture;
}

spo::ShapeDocument make_face_with_hole_fixture() {
    BRepBuilderAPI_MakeWire outer;
    outer.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(3, 0, 0)).Edge());
    outer.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(3, 0, 0), gp_Pnt(3, 3, 0)).Edge());
    outer.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(3, 3, 0), gp_Pnt(0, 3, 0)).Edge());
    outer.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 3, 0), gp_Pnt(0, 0, 0)).Edge());

    BRepBuilderAPI_MakeWire inner;
    inner.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(1, 1, 0), gp_Pnt(1, 2, 0)).Edge());
    inner.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(1, 2, 0), gp_Pnt(2, 2, 0)).Edge());
    inner.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(2, 2, 0), gp_Pnt(2, 1, 0)).Edge());
    inner.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(2, 1, 0), gp_Pnt(1, 1, 0)).Edge());

    BRepBuilderAPI_MakeFace face(outer.Wire());
    face.Add(inner.Wire());
    return spo::ShapeDocument(face.Face(), {});
}

void test_constraint_extractor_accepts_one_connected_outer_loop() {
    const auto fixture = make_two_face_fixture();
    spo::RegionRefitRequest request;
    request.selectedFaces = {1, 0, 1};

    const auto result = spo::RegionConstraintExtractor().extract(fixture.document, request);

    assert(result.success());
    assert(result.constraints.selectedFaces == std::vector<spo::FaceId>({0, 1}));
    assert(result.constraints.internalEdges.size() == 1);
    assert(result.constraints.orderedBoundaryEdges.size() == 6);
    assert(result.constraints.boundaryEdges.size() == 6);
    assert(!result.constraints.outerWire.IsNull());
    for (const auto& edge : result.constraints.boundaryEdges) {
        assert(edge.hardPositionConstraint);
        assert(!edge.originalEdge.IsNull());
        assert(!edge.selectedPcurve.IsNull());
        assert(edge.loopOrientation == TopAbs_FORWARD || edge.loopOrientation == TopAbs_REVERSED);
    }
}

void test_constraint_extractor_preserves_external_neighbor_constraint() {
    const auto fixture = make_two_face_fixture();
    spo::RegionRefitRequest request;
    request.selectedFaces = {0};

    const auto result = spo::RegionConstraintExtractor().extract(fixture.document, request);

    assert(result.success());
    const auto adjacent = std::find_if(
        result.constraints.boundaryEdges.begin(),
        result.constraints.boundaryEdges.end(),
        [](const auto& edge) { return edge.adjacentFaceId.has_value(); });
    assert(adjacent != result.constraints.boundaryEdges.end());
    assert(adjacent->edgeId == fixture.sharedEdge);
    assert(!adjacent->adjacentPcurve.IsNull());
}

void test_constraint_extractor_retains_locked_internal_edge() {
    const auto fixture = make_two_face_fixture();
    spo::RegionRefitRequest request;
    request.selectedFaces = {0, 1};
    request.lockedEdges = {fixture.sharedEdge};

    const auto result = spo::RegionConstraintExtractor().extract(fixture.document, request);

    assert(result.success());
    assert(result.constraints.lockedInternalEdges == std::vector<spo::EdgeId>({fixture.sharedEdge}));
}

void test_constraint_extractor_rejects_hole() {
    const auto document = make_face_with_hole_fixture();
    spo::RegionRefitRequest request;
    request.selectedFaces = {0};

    const auto result = spo::RegionConstraintExtractor().extract(document, request);

    assert(!result.success());
    assert(result.status == spo::RegionRefitStatus::UnsupportedTopology);
    assert(!result.message.empty());
}

void test_constraint_extractor_rejects_missing_face() {
    const auto fixture = make_two_face_fixture();
    spo::RegionRefitRequest request;
    request.selectedFaces = {fixture.document.topology().faceCount()};

    const auto result = spo::RegionConstraintExtractor().extract(fixture.document, request);

    assert(!result.success());
    assert(result.status == spo::RegionRefitStatus::InvalidRequest);
}

void test_engine_rejects_invalid_resource_budget_without_mutating_shape() {
    const auto fixture = make_two_face_fixture();
    const auto beforeFaces = fixture.document.stats().faces;
    spo::RegionRefitRequest request;
    request.selectedFaces = {0, 1};
    request.options.maximumMemoryMiB = 0;

    const auto result = spo::RegionalSurfaceRefitEngine().run(fixture.document, request);

    assert(result.report.status == spo::RegionRefitStatus::InvalidRequest);
    assert(!result.canApply());
    assert(result.finalShape.IsNull());
    assert(fixture.document.stats().faces == beforeFaces);
}

void test_engine_records_constraint_stage_before_unimplemented_layout() {
    const auto fixture = make_two_face_fixture();
    spo::RegionRefitRequest request;
    request.selectedFaces = {0, 1};

    const auto result = spo::RegionalSurfaceRefitEngine().run(fixture.document, request);

    assert(result.report.status == spo::RegionRefitStatus::NoValidatedLayoutWithinBudget);
    assert(result.report.failureStage == spo::RegionRefitStage::Layout);
    assert(result.report.stages.size() == 1);
    assert(result.report.stages.front().success);
    assert(result.constraints.boundaryEdges.size() == 6);
    assert(!result.canApply());
}

}

void run_region_refit_constraint_tests() {
    test_constraint_extractor_accepts_one_connected_outer_loop();
    test_constraint_extractor_preserves_external_neighbor_constraint();
    test_constraint_extractor_retains_locked_internal_edge();
    test_constraint_extractor_rejects_hole();
    test_constraint_extractor_rejects_missing_face();
    test_engine_rejects_invalid_resource_budget_without_mutating_shape();
    test_engine_records_constraint_stage_before_unimplemented_layout();
}
