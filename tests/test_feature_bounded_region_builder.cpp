#include "merge/FeatureBoundedRegionBuilder.h"

#include "brep/ShapeDocument.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>
#include <set>
#include <vector>

namespace {

struct TwoFaceFixture {
    spo::ShapeDocument document;
    spo::EdgeId shared_edge = 0;
    std::vector<spo::EdgeId> boundary_edges;
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
    assert(fixture.document.topology().faceCount() == 2);

    for (spo::EdgeId edge = 0; edge < fixture.document.topology().edgeCount(); ++edge) {
        const auto* adjacency = fixture.document.topology().adjacencyForEdge(edge);
        assert(adjacency != nullptr);
        if (adjacency->faces.size() == 2) {
            fixture.shared_edge = edge;
        } else if (adjacency->faces.size() == 1) {
            fixture.boundary_edges.push_back(edge);
        }
    }

    assert(fixture.boundary_edges.size() == 6);
    return fixture;
}

bool contains_edge(const std::vector<spo::EdgeId>& edges, spo::EdgeId edge) {
    return std::find(edges.begin(), edges.end(), edge) != edges.end();
}

void test_unprotected_shared_edge_builds_single_region() {
    const auto fixture = make_two_face_fixture();
    const spo::FeatureBoundedRegionBuilder builder;

    const auto candidates = builder.build(fixture.document, {}, 2);

    assert(candidates.size() == 1);
    const auto& candidate = candidates.front();
    assert(candidate.candidate_type == spo::MergeCandidateType::FeatureBoundedRefit);
    assert(candidate.face_count == 2);
    assert(candidate.faces.size() == 2);
    assert(candidate.internal_edge_count == 1);
    assert(candidate.internal_edges.size() == 1);
    assert(candidate.internal_edges.front() == fixture.shared_edge);
    assert(candidate.boundary_edge_count == 6);
    assert(candidate.boundary_edges.size() == 6);
    assert(candidate.protected_edges.empty());
    assert(candidate.blocked_edges.empty());
}

void test_free_edges_and_model_boundary_are_boundary_edges() {
    const auto fixture = make_two_face_fixture();
    const spo::FeatureBoundedRegionBuilder builder;

    const auto candidates = builder.build(fixture.document, {}, 2);

    assert(candidates.size() == 1);
    for (const auto edge : fixture.boundary_edges) {
        assert(contains_edge(candidates.front().boundary_edges, edge));
        const auto* adjacency = fixture.document.topology().adjacencyForEdge(edge);
        assert(adjacency != nullptr);
        assert(adjacency->faces.size() == 1);
    }
}

void test_protected_shared_edge_splits_regions() {
    const auto fixture = make_two_face_fixture();
    const spo::FeatureBoundedRegionBuilder builder;
    const std::set<spo::EdgeId> protectedEdges {fixture.shared_edge};

    const auto candidates = builder.build(fixture.document, protectedEdges, 1);

    assert(candidates.size() == 2);
    for (const auto& candidate : candidates) {
        assert(candidate.candidate_type == spo::MergeCandidateType::FeatureBoundedRefit);
        assert(candidate.face_count == 1);
        assert(candidate.internal_edge_count == 0);
        assert(candidate.internal_edges.empty());
        assert(candidate.boundary_edge_count == 4);
        assert(contains_edge(candidate.boundary_edges, fixture.shared_edge));
        assert(candidate.protected_edges.size() == 1);
        assert(candidate.protected_edges.front() == fixture.shared_edge);
        assert(candidate.blocked_edges.size() == 1);
        assert(candidate.blocked_edges.front() == fixture.shared_edge);
    }
}

void test_min_region_faces_filters_small_regions() {
    const auto fixture = make_two_face_fixture();
    const spo::FeatureBoundedRegionBuilder builder;
    const std::set<spo::EdgeId> protectedEdges {fixture.shared_edge};

    const auto candidates = builder.build(fixture.document, protectedEdges, 2);

    assert(candidates.empty());
}

}

void run_feature_bounded_region_builder_tests() {
    test_unprotected_shared_edge_builds_single_region();
    test_free_edges_and_model_boundary_are_boundary_edges();
    test_protected_shared_edge_splits_regions();
    test_min_region_faces_filters_small_regions();
}
