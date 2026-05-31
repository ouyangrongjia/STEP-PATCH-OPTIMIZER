#include "brep/ShapeDocument.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "merge/PlaneRegionMerger.h"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>
#include <string>

namespace {

struct BoundaryFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
};

BoundaryFixture make_two_face_boundary_fixture() {
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

    BoundaryFixture fixture;
    fixture.document = spo::ShapeDocument(sewing.SewedShape(), {});
    fixture.candidate.candidate_id = 3;
    fixture.candidate.candidate_type = spo::MergeCandidateType::PlaneLike;
    fixture.candidate.status = spo::MergeCandidateStatus::Accepted;
    fixture.candidate.faces = {0, 1};
    fixture.candidate.face_count = 2;

    for (spo::EdgeId edge = 0; edge < fixture.document.topology().edgeCount(); ++edge) {
        const auto* adjacency = fixture.document.topology().adjacencyForEdge(edge);
        assert(adjacency != nullptr);
        if (adjacency->faces.size() == 2) {
            fixture.candidate.internal_edges.push_back(edge);
        } else {
            fixture.candidate.boundary_edges.push_back(edge);
        }
    }
    fixture.candidate.boundary_edge_count = static_cast<int>(fixture.candidate.boundary_edges.size());
    fixture.candidate.internal_edge_count = static_cast<int>(fixture.candidate.internal_edges.size());
    assert(fixture.candidate.boundary_edges.size() == 6);
    assert(fixture.candidate.internal_edges.size() == 1);
    return fixture;
}

BoundaryFixture make_face_with_hole_boundary_fixture() {
    const gp_Pnt p00(0.0, 0.0, 0.0);
    const gp_Pnt p30(3.0, 0.0, 0.0);
    const gp_Pnt p33(3.0, 3.0, 0.0);
    const gp_Pnt p03(0.0, 3.0, 0.0);
    const gp_Pnt h11(1.0, 1.0, 0.0);
    const gp_Pnt h21(2.0, 1.0, 0.0);
    const gp_Pnt h22(2.0, 2.0, 0.0);
    const gp_Pnt h12(1.0, 2.0, 0.0);

    BRepBuilderAPI_MakeWire outerWire;
    outerWire.Add(BRepBuilderAPI_MakeEdge(p00, p30).Edge());
    outerWire.Add(BRepBuilderAPI_MakeEdge(p30, p33).Edge());
    outerWire.Add(BRepBuilderAPI_MakeEdge(p33, p03).Edge());
    outerWire.Add(BRepBuilderAPI_MakeEdge(p03, p00).Edge());

    BRepBuilderAPI_MakeWire innerWire;
    innerWire.Add(BRepBuilderAPI_MakeEdge(h11, h12).Edge());
    innerWire.Add(BRepBuilderAPI_MakeEdge(h12, h22).Edge());
    innerWire.Add(BRepBuilderAPI_MakeEdge(h22, h21).Edge());
    innerWire.Add(BRepBuilderAPI_MakeEdge(h21, h11).Edge());

    BRepBuilderAPI_MakeFace faceBuilder(outerWire.Wire());
    faceBuilder.Add(innerWire.Wire());

    BoundaryFixture fixture;
    fixture.document = spo::ShapeDocument(faceBuilder.Face(), {});
    fixture.candidate.candidate_id = 9;
    fixture.candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    fixture.candidate.status = spo::MergeCandidateStatus::Accepted;
    fixture.candidate.faces = {0};
    fixture.candidate.face_count = 1;
    for (spo::EdgeId edge = 0; edge < fixture.document.topology().edgeCount(); ++edge) {
        fixture.candidate.boundary_edges.push_back(edge);
    }
    fixture.candidate.boundary_edge_count = static_cast<int>(fixture.candidate.boundary_edges.size());
    assert(fixture.candidate.boundary_edges.size() == 8);
    return fixture;
}

BoundaryFixture make_branching_boundary_fixture() {
    const gp_Pnt p00(0.0, 0.0, 0.0);
    const gp_Pnt p10(1.0, 0.0, 0.0);
    const gp_Pnt p11(1.0, 1.0, 0.0);
    const gp_Pnt p01(0.0, 1.0, 0.0);
    const gp_Pnt pBranch(-0.5, 0.0, 0.0);

    const auto bottom = BRepBuilderAPI_MakeEdge(p00, p10).Edge();
    BRepBuilderAPI_MakeWire wire;
    wire.Add(bottom);
    wire.Add(BRepBuilderAPI_MakeEdge(p10, p11).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p11, p01).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p01, p00).Edge());
    const auto face = BRepBuilderAPI_MakeFace(wire.Wire()).Face();
    TopoDS_Vertex sharedVertex;
    TopoDS_Vertex unusedVertex;
    TopExp::Vertices(bottom, sharedVertex, unusedVertex);
    const auto branchEnd = BRepBuilderAPI_MakeVertex(pBranch).Vertex();
    const auto branchEdge = BRepBuilderAPI_MakeEdge(sharedVertex, branchEnd).Edge();

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    builder.Add(compound, face);
    builder.Add(compound, branchEdge);

    BoundaryFixture fixture;
    fixture.document = spo::ShapeDocument(compound, {});
    fixture.candidate.candidate_id = 10;
    fixture.candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    fixture.candidate.status = spo::MergeCandidateStatus::Accepted;
    fixture.candidate.faces = {0};
    fixture.candidate.face_count = 1;
    for (spo::EdgeId edge = 0; edge < fixture.document.topology().edgeCount(); ++edge) {
        fixture.candidate.boundary_edges.push_back(edge);
    }
    fixture.candidate.boundary_edge_count = static_cast<int>(fixture.candidate.boundary_edges.size());
    assert(fixture.candidate.boundary_edges.size() == 5);
    return fixture;
}

void assert_failure_has_message(
    const spo::RegionBoundaryAnalysis& analysis,
    spo::RegionMergeFailureReason reason) {
    assert(!analysis.valid);
    assert(analysis.failure_reason == reason);
    assert(!analysis.message.empty());
}

void test_region_boundary_analyzer_accepts_simple_closed_boundary() {
    const auto fixture = make_two_face_boundary_fixture();
    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert(analysis.valid);
    assert(analysis.failure_reason == spo::RegionMergeFailureReason::None);
    assert(analysis.connected_component_count == 1);
    assert(analysis.outer_wire_count == 1);
    assert(analysis.inner_wire_count == 0);
    assert(analysis.boundary_closed);
    assert(!analysis.has_holes);
    assert(!analysis.has_non_manifold_edges);
    assert(!analysis.has_branching_boundary);
    assert(analysis.boundary_loops.size() == 1);
    assert(analysis.ordered_boundary_edges.size() == fixture.candidate.boundary_edges.size());
    assert(analysis.message == "Candidate boundary contains one closed outer loop.");
}

void test_region_boundary_analyzer_orders_unsorted_boundary_edges() {
    auto fixture = make_two_face_boundary_fixture();
    std::reverse(fixture.candidate.boundary_edges.begin(), fixture.candidate.boundary_edges.end());

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert(analysis.valid);
    assert(analysis.boundary_closed);
    assert(analysis.boundary_loops.size() == 1);
    assert(analysis.ordered_boundary_edges.size() == fixture.candidate.boundary_edges.size());
}

void test_region_boundary_analyzer_rejects_invalid_face_id() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.faces.push_back(static_cast<spo::FaceId>(fixture.document.topology().faceCount() + 5));

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert_failure_has_message(analysis, spo::RegionMergeFailureReason::InvalidCandidate);
}

void test_region_boundary_analyzer_rejects_invalid_edge_id() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.boundary_edges.push_back(static_cast<spo::EdgeId>(fixture.document.topology().edgeCount() + 10));

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert_failure_has_message(analysis, spo::RegionMergeFailureReason::BoundaryLoopInvalid);
}

void test_region_boundary_analyzer_rejects_empty_faces() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.faces.clear();
    fixture.candidate.face_count = 0;

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert_failure_has_message(analysis, spo::RegionMergeFailureReason::InvalidCandidate);
}

void test_region_boundary_analyzer_rejects_empty_boundary_edges() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.boundary_edges.clear();
    fixture.candidate.boundary_edge_count = 0;

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert_failure_has_message(analysis, spo::RegionMergeFailureReason::BoundaryLoopInvalid);
}

void test_region_boundary_analyzer_rejects_open_boundary() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.boundary_edges.pop_back();

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert_failure_has_message(analysis, spo::RegionMergeFailureReason::BoundaryLoopInvalid);
    assert(!analysis.boundary_closed);
}

void test_region_boundary_analyzer_rejects_disconnected_region() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.boundary_edges.insert(
        fixture.candidate.boundary_edges.end(),
        fixture.candidate.internal_edges.begin(),
        fixture.candidate.internal_edges.end());

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert_failure_has_message(analysis, spo::RegionMergeFailureReason::MultipleOuterLoopsNotSupported);
    assert(analysis.connected_component_count > 1);
}

void test_region_boundary_analyzer_rejects_multiple_boundary_loops() {
    const auto fixture = make_face_with_hole_boundary_fixture();

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert_failure_has_message(analysis, spo::RegionMergeFailureReason::InnerLoopsNotSupported);
    assert(analysis.boundary_closed);
    assert(analysis.outer_wire_count == 1);
    assert(analysis.inner_wire_count == 1);
    assert(analysis.boundary_loops.size() == 2);
}

void test_region_boundary_analyzer_rejects_hole_inner_loop() {
    const auto fixture = make_face_with_hole_boundary_fixture();

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert_failure_has_message(analysis, spo::RegionMergeFailureReason::InnerLoopsNotSupported);
    assert(analysis.has_holes);
    assert(analysis.inner_wire_count > 0);
}

void test_region_boundary_analyzer_rejects_branching_boundary() {
    const auto fixture = make_branching_boundary_fixture();

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert_failure_has_message(analysis, spo::RegionMergeFailureReason::BoundaryLoopInvalid);
    assert(analysis.has_branching_boundary);
}

void test_region_boundary_analyzer_rejects_non_manifold_boundary() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.boundary_edges.push_back(fixture.candidate.boundary_edges.front());

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert_failure_has_message(analysis, spo::RegionMergeFailureReason::BoundaryLoopInvalid);
    assert(analysis.has_non_manifold_edges);
}

void test_region_boundary_analyzer_accepts_feature_bounded_refit_candidate() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert(analysis.valid);
    assert(analysis.failure_reason == spo::RegionMergeFailureReason::None);
    assert(analysis.outer_wire_count == 1);
    assert(analysis.boundary_closed);
}

void test_plane_region_merger_still_merges_with_boundary_analyzer() {
    const auto fixture = make_two_face_boundary_fixture();
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;
    const auto before = fixture.document.stats();

    const auto result = merger.merge(fixture.document, fixture.candidate, options);

    assert(result.success);
    assert(result.document.stats().faces < before.faces);
}

void test_plane_region_merger_uses_analyzer_ordered_boundary_edges() {
    auto fixture = make_two_face_boundary_fixture();
    std::reverse(fixture.candidate.boundary_edges.begin(), fixture.candidate.boundary_edges.end());
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;
    const auto before = fixture.document.stats();

    const auto result = merger.merge(fixture.document, fixture.candidate, options);

    assert(result.success);
    assert(result.document.stats().faces < before.faces);
}

}

void run_region_boundary_analyzer_tests() {
    test_region_boundary_analyzer_accepts_simple_closed_boundary();
    test_region_boundary_analyzer_orders_unsorted_boundary_edges();
    test_region_boundary_analyzer_rejects_invalid_face_id();
    test_region_boundary_analyzer_rejects_invalid_edge_id();
    test_region_boundary_analyzer_rejects_empty_faces();
    test_region_boundary_analyzer_rejects_empty_boundary_edges();
    test_region_boundary_analyzer_rejects_open_boundary();
    test_region_boundary_analyzer_rejects_disconnected_region();
    test_region_boundary_analyzer_rejects_multiple_boundary_loops();
    test_region_boundary_analyzer_rejects_hole_inner_loop();
    test_region_boundary_analyzer_rejects_branching_boundary();
    test_region_boundary_analyzer_rejects_non_manifold_boundary();
    test_region_boundary_analyzer_accepts_feature_bounded_refit_candidate();
    test_plane_region_merger_still_merges_with_boundary_analyzer();
    test_plane_region_merger_uses_analyzer_ordered_boundary_edges();
}
