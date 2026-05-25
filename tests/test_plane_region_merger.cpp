#include "brep/ShapeDocument.h"
#include "merge/PlaneRegionMerger.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Shape.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace {

struct PlaneFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
    spo::EdgeId shared_edge = 0;
};

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
}

PlaneFixture make_two_face_plane_fixture(bool convert_to_nurbs = false) {
    const gp_Pnt p00(0.0, 0.0, 0.0);
    const gp_Pnt p10(1.0, 0.0, 0.0);
    const gp_Pnt p20(2.0, 0.0, 0.0);
    const gp_Pnt p01(0.0, 1.0, 0.0);
    const gp_Pnt p11(1.0, 1.0, 0.0);
    const gp_Pnt p21(2.0, 1.0, 0.0);

    const auto bottom_left = BRepBuilderAPI_MakeEdge(p00, p10).Edge();
    const auto shared = BRepBuilderAPI_MakeEdge(p10, p11).Edge();
    const auto top_left = BRepBuilderAPI_MakeEdge(p11, p01).Edge();
    const auto left = BRepBuilderAPI_MakeEdge(p01, p00).Edge();
    const auto bottom_right = BRepBuilderAPI_MakeEdge(p10, p20).Edge();
    const auto right = BRepBuilderAPI_MakeEdge(p20, p21).Edge();
    const auto top_right = BRepBuilderAPI_MakeEdge(p21, p11).Edge();

    BRepBuilderAPI_MakeWire left_wire;
    left_wire.Add(bottom_left);
    left_wire.Add(shared);
    left_wire.Add(top_left);
    left_wire.Add(left);
    const auto left_face = BRepBuilderAPI_MakeFace(left_wire.Wire()).Face();

    BRepBuilderAPI_MakeWire right_wire;
    right_wire.Add(bottom_right);
    right_wire.Add(right);
    right_wire.Add(top_right);
    right_wire.Add(TopoDS::Edge(shared.Reversed()));
    const auto right_face = BRepBuilderAPI_MakeFace(right_wire.Wire()).Face();

    BRepBuilderAPI_Sewing sewing;
    sewing.Add(left_face);
    sewing.Add(right_face);
    sewing.Perform();
    TopoDS_Shape shape = sewing.SewedShape();
    if (convert_to_nurbs) {
        BRepBuilderAPI_NurbsConvert converter(shape);
        shape = converter.Shape();
    }

    PlaneFixture fixture;
    fixture.document = spo::ShapeDocument(shape, {});
    for (spo::EdgeId edge = 0; edge < fixture.document.topology().edgeCount(); ++edge) {
        const auto* adjacency = fixture.document.topology().adjacencyForEdge(edge);
        assert(adjacency != nullptr);
        if (adjacency->faces.size() == 2) {
            fixture.shared_edge = edge;
            fixture.candidate.internal_edges.push_back(edge);
        } else if (adjacency->faces.size() == 1) {
            fixture.candidate.boundary_edges.push_back(edge);
        }
    }

    fixture.candidate.candidate_id = 7;
    fixture.candidate.candidate_type = spo::MergeCandidateType::PlaneLike;
    fixture.candidate.status = spo::MergeCandidateStatus::Accepted;
    fixture.candidate.faces = {0, 1};
    fixture.candidate.face_count = static_cast<int>(fixture.candidate.faces.size());
    fixture.candidate.boundary_edge_count = static_cast<int>(fixture.candidate.boundary_edges.size());
    fixture.candidate.internal_edge_count = static_cast<int>(fixture.candidate.internal_edges.size());
    assert(fixture.candidate.boundary_edges.size() == 6);
    assert(fixture.candidate.internal_edges.size() == 1);
    return fixture;
}

TopoDS_Face make_quad_face(const gp_Pnt& p1, const gp_Pnt& p2, const gp_Pnt& p3, const gp_Pnt& p4) {
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p3, p4).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p4, p1).Edge());
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}

PlaneFixture make_split_solid_top_fixture() {
    const gp_Pnt p000(0.0, 0.0, 0.0);
    const gp_Pnt p100(1.0, 0.0, 0.0);
    const gp_Pnt p200(2.0, 0.0, 0.0);
    const gp_Pnt p010(0.0, 1.0, 0.0);
    const gp_Pnt p110(1.0, 1.0, 0.0);
    const gp_Pnt p210(2.0, 1.0, 0.0);
    const gp_Pnt p001(0.0, 0.0, 1.0);
    const gp_Pnt p101(1.0, 0.0, 1.0);
    const gp_Pnt p201(2.0, 0.0, 1.0);
    const gp_Pnt p011(0.0, 1.0, 1.0);
    const gp_Pnt p111(1.0, 1.0, 1.0);
    const gp_Pnt p211(2.0, 1.0, 1.0);

    const auto bottom = make_quad_face(p010, p210, p200, p000);
    const auto top_left = make_quad_face(p001, p101, p111, p011);
    const auto top_right = make_quad_face(p101, p201, p211, p111);
    const auto front = make_quad_face(p000, p200, p201, p001);
    const auto right = make_quad_face(p200, p210, p211, p201);
    const auto back = make_quad_face(p210, p010, p011, p211);
    const auto left = make_quad_face(p010, p000, p001, p011);

    BRepBuilderAPI_Sewing sewing;
    sewing.Add(bottom);
    sewing.Add(top_left);
    sewing.Add(top_right);
    sewing.Add(front);
    sewing.Add(right);
    sewing.Add(back);
    sewing.Add(left);
    sewing.Perform();

    TopoDS_Shell shell;
    for (TopExp_Explorer explorer(sewing.SewedShape(), TopAbs_SHELL); explorer.More(); explorer.Next()) {
        shell = TopoDS::Shell(explorer.Current());
        break;
    }
    assert(!shell.IsNull());

    BRep_Builder builder;
    TopoDS_Solid solid;
    builder.MakeSolid(solid);
    builder.Add(solid, shell);

    PlaneFixture fixture;
    fixture.document = spo::ShapeDocument(solid, {});
    assert(fixture.document.stats().solids == 1);

    std::vector<spo::FaceId> top_faces;
    for (spo::FaceId face = 0; face < fixture.document.topology().faceCount(); ++face) {
        const auto sample = fixture.document.topology().face(face);
        double uMin = 0.0;
        double uMax = 0.0;
        double vMin = 0.0;
        double vMax = 0.0;
        BRepTools::UVBounds(sample, uMin, uMax, vMin, vMax);
        BRepAdaptor_Surface surface(sample);
        const gp_Pnt point = surface.Value((uMin + uMax) * 0.5, (vMin + vMax) * 0.5);
        if (std::abs(point.Z() - 1.0) < 1.0e-6) {
            top_faces.push_back(face);
        }
    }
    assert(top_faces.size() == 2);

    fixture.candidate.candidate_id = 9;
    fixture.candidate.candidate_type = spo::MergeCandidateType::PlaneLike;
    fixture.candidate.status = spo::MergeCandidateStatus::Accepted;
    fixture.candidate.faces = top_faces;
    fixture.candidate.face_count = static_cast<int>(fixture.candidate.faces.size());
    for (spo::EdgeId edge = 0; edge < fixture.document.topology().edgeCount(); ++edge) {
        const auto* adjacency = fixture.document.topology().adjacencyForEdge(edge);
        assert(adjacency != nullptr);
        bool touchesCandidate = false;
        int candidateFaceRefs = 0;
        for (const auto faceId : adjacency->faces) {
            if (std::find(top_faces.begin(), top_faces.end(), faceId) != top_faces.end()) {
                touchesCandidate = true;
                ++candidateFaceRefs;
            }
        }
        if (!touchesCandidate) {
            continue;
        }
        if (candidateFaceRefs == 2) {
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

void test_plane_region_merger_rejects_invalid_candidate_states() {
    const auto fixture = make_two_face_plane_fixture();
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    auto wrong_type = fixture.candidate;
    wrong_type.candidate_type = spo::MergeCandidateType::CylinderLike;
    const auto wrong_type_result = merger.merge(fixture.document, wrong_type, options);
    assert(!wrong_type_result.success);
    assert(wrong_type_result.failure_reason == spo::RegionMergeFailureReason::UnsupportedCandidateType);

    auto rejected = fixture.candidate;
    rejected.status = spo::MergeCandidateStatus::Rejected;
    const auto rejected_result = merger.merge(fixture.document, rejected, options);
    assert(!rejected_result.success);
    assert(rejected_result.failure_reason == spo::RegionMergeFailureReason::RejectedCandidate);

    auto hidden = fixture.candidate;
    hidden.status = spo::MergeCandidateStatus::Hidden;
    const auto hidden_result = merger.merge(fixture.document, hidden, options);
    assert(!hidden_result.success);
    assert(hidden_result.failure_reason == spo::RegionMergeFailureReason::HiddenCandidate);
}

void test_plane_region_merger_rejects_small_or_protected_regions() {
    const auto fixture = make_two_face_plane_fixture();
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    auto small = fixture.candidate;
    small.face_count = 1;
    small.faces = {small.faces.front()};
    const auto small_result = merger.merge(fixture.document, small, options);
    assert(!small_result.success);
    assert(small_result.failure_reason == spo::RegionMergeFailureReason::InsufficientFaces);

    auto protected_candidate = fixture.candidate;
    protected_candidate.protected_edges = {fixture.shared_edge};
    const auto protected_result = merger.merge(fixture.document, protected_candidate, options);
    assert(!protected_result.success);
    assert(protected_result.failure_reason == spo::RegionMergeFailureReason::ProtectedEdgeConflict);
}

void test_plane_region_merger_rejects_invalid_boundary_without_changing_stats() {
    const auto fixture = make_two_face_plane_fixture();
    const auto before = fixture.document.stats();
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    auto invalid_boundary = fixture.candidate;
    invalid_boundary.boundary_edges = {invalid_boundary.boundary_edges.front()};
    const auto result = merger.merge(fixture.document, invalid_boundary, options);
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::BoundaryLoopInvalid);
    assert(same_stats(fixture.document.stats(), before));
}

void test_plane_region_merger_merges_simple_coplanar_region() {
    const auto fixture = make_two_face_plane_fixture();
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    assert(result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::None);
    assert(result.document.hasShape());
    assert(result.face_count_after < result.face_count_before);
    assert(result.document.stats().faces > 0);
    assert(result.document.stats().edges > 0);
}

void test_plane_region_merger_accepts_nurbs_backed_planar_region() {
    const auto fixture = make_two_face_plane_fixture(true);
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    assert(result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::None);
    assert(result.document.hasShape());
    assert(result.face_count_after < result.face_count_before);
    assert(result.max_deviation <= options.max_deviation);
}

void test_plane_region_merger_preserves_solid_container() {
    const auto fixture = make_split_solid_top_fixture();
    const auto before = fixture.document.stats();
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);

    assert(result.success);
    assert(result.document.stats().solids == before.solids);
    assert(result.document.stats().faces < before.faces);
    assert(result.document.stats().edges < before.edges - 1);
}

}

void run_plane_region_merger_tests() {
    test_plane_region_merger_rejects_invalid_candidate_states();
    test_plane_region_merger_rejects_small_or_protected_regions();
    test_plane_region_merger_rejects_invalid_boundary_without_changing_stats();
    test_plane_region_merger_merges_simple_coplanar_region();
    test_plane_region_merger_accepts_nurbs_backed_planar_region();
    test_plane_region_merger_preserves_solid_container();
}
