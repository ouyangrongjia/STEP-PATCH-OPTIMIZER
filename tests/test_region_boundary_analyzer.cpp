#include "brep/ShapeDocument.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "merge/PlaneRegionMerger.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>

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
    assert(analysis.boundary_loops.size() == 1);
    assert(analysis.ordered_boundary_edges.size() == fixture.candidate.boundary_edges.size());
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

void test_region_boundary_analyzer_rejects_invalid_edge_id() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.boundary_edges.push_back(static_cast<spo::EdgeId>(fixture.document.topology().edgeCount() + 10));

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert(!analysis.valid);
    assert(analysis.failure_reason == spo::RegionMergeFailureReason::BoundaryLoopInvalid);
}

void test_region_boundary_analyzer_rejects_open_boundary() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.boundary_edges.pop_back();

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert(!analysis.valid);
    assert(analysis.failure_reason == spo::RegionMergeFailureReason::BoundaryLoopInvalid);
}

void test_region_boundary_analyzer_rejects_disconnected_region() {
    auto fixture = make_two_face_boundary_fixture();
    fixture.candidate.boundary_edges.insert(
        fixture.candidate.boundary_edges.end(),
        fixture.candidate.internal_edges.begin(),
        fixture.candidate.internal_edges.end());

    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);

    assert(!analysis.valid);
    assert(analysis.failure_reason == spo::RegionMergeFailureReason::MultipleOuterLoopsNotSupported);
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
    test_region_boundary_analyzer_rejects_invalid_edge_id();
    test_region_boundary_analyzer_rejects_open_boundary();
    test_region_boundary_analyzer_rejects_disconnected_region();
    test_plane_region_merger_still_merges_with_boundary_analyzer();
    test_plane_region_merger_uses_analyzer_ordered_boundary_edges();
}
