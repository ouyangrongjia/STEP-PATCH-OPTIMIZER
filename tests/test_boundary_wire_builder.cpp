#include "brep/BoundaryWireBuilder.h"
#include "brep/ShapeDocument.h"
#include "merge/RegionBoundaryAnalyzer.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
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
    fixture.candidate.candidate_id = 4;
    fixture.candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
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
    return fixture;
}

spo::RegionBoundaryAnalysis make_valid_analysis(const BoundaryFixture& fixture) {
    const spo::RegionBoundaryAnalyzer analyzer;
    const auto analysis = analyzer.analyze(fixture.document, fixture.candidate);
    assert(analysis.valid);
    return analysis;
}

void assert_build_failed(const spo::BoundaryWireBuildResult& result) {
    assert(!result.success);
    assert(result.wire.IsNull());
    assert(!result.message.empty());
}

void test_boundary_wire_builder_builds_simple_closed_wire() {
    const auto fixture = make_two_face_boundary_fixture();
    const auto analysis = make_valid_analysis(fixture);
    const spo::BoundaryWireBuilder builder;

    const auto result = builder.buildOuterWire(fixture.document, analysis);

    assert(result.success);
    assert(!result.wire.IsNull());
    assert(result.wire.Closed());
}

void test_boundary_wire_builder_rejects_invalid_analysis() {
    const auto fixture = make_two_face_boundary_fixture();
    auto analysis = make_valid_analysis(fixture);
    analysis.valid = false;
    const spo::BoundaryWireBuilder builder;

    assert_build_failed(builder.buildOuterWire(fixture.document, analysis));
}

void test_boundary_wire_builder_rejects_outer_wire_count_not_one() {
    const auto fixture = make_two_face_boundary_fixture();
    auto analysis = make_valid_analysis(fixture);
    analysis.outer_wire_count = 2;
    const spo::BoundaryWireBuilder builder;

    assert_build_failed(builder.buildOuterWire(fixture.document, analysis));
}

void test_boundary_wire_builder_rejects_inner_wires() {
    const auto fixture = make_two_face_boundary_fixture();
    auto analysis = make_valid_analysis(fixture);
    analysis.inner_wire_count = 1;
    const spo::BoundaryWireBuilder builder;

    assert_build_failed(builder.buildOuterWire(fixture.document, analysis));
}

void test_boundary_wire_builder_rejects_open_boundary() {
    const auto fixture = make_two_face_boundary_fixture();
    auto analysis = make_valid_analysis(fixture);
    analysis.boundary_closed = false;
    const spo::BoundaryWireBuilder builder;

    assert_build_failed(builder.buildOuterWire(fixture.document, analysis));
}

void test_boundary_wire_builder_rejects_empty_ordered_edges() {
    const auto fixture = make_two_face_boundary_fixture();
    auto analysis = make_valid_analysis(fixture);
    analysis.ordered_boundary_edges.clear();
    const spo::BoundaryWireBuilder builder;

    assert_build_failed(builder.buildOuterWire(fixture.document, analysis));
}

void test_boundary_wire_builder_rejects_out_of_range_edge_id() {
    const auto fixture = make_two_face_boundary_fixture();
    auto analysis = make_valid_analysis(fixture);
    analysis.ordered_boundary_edges.push_back(static_cast<spo::EdgeId>(fixture.document.topology().edgeCount() + 1));
    const spo::BoundaryWireBuilder builder;

    assert_build_failed(builder.buildOuterWire(fixture.document, analysis));
}

void test_boundary_wire_builder_uses_analyzer_ordered_edges_only() {
    auto fixture = make_two_face_boundary_fixture();
    std::reverse(fixture.candidate.boundary_edges.begin(), fixture.candidate.boundary_edges.end());
    const auto analysis = make_valid_analysis(fixture);
    fixture.candidate.boundary_edges.clear();
    const spo::BoundaryWireBuilder builder;

    const auto result = builder.buildOuterWire(fixture.document, analysis);

    assert(result.success);
    assert(!result.wire.IsNull());
}

}

void run_boundary_wire_builder_tests() {
    test_boundary_wire_builder_builds_simple_closed_wire();
    test_boundary_wire_builder_rejects_invalid_analysis();
    test_boundary_wire_builder_rejects_outer_wire_count_not_one();
    test_boundary_wire_builder_rejects_inner_wires();
    test_boundary_wire_builder_rejects_open_boundary();
    test_boundary_wire_builder_rejects_empty_ordered_edges();
    test_boundary_wire_builder_rejects_out_of_range_edge_id();
    test_boundary_wire_builder_uses_analyzer_ordered_edges_only();
}
