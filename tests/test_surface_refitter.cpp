#include "brep/ShapeDocument.h"
#include "merge/SurfaceRefitter.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <GeomAPI_PointsToBSplineSurface.hxx>
#include <Geom_BSplineSurface.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <cmath>

namespace {

struct RefitFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
};

spo::MergeCandidate make_all_faces_candidate(const spo::ShapeDocument& document) {
    spo::MergeCandidate candidate;
    candidate.candidate_id = 17;
    candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    candidate.status = spo::MergeCandidateStatus::Accepted;
    for (spo::FaceId face_id = 0; face_id < document.topology().faceCount(); ++face_id) {
        candidate.faces.push_back(face_id);
    }
    candidate.face_count = static_cast<int>(candidate.faces.size());
    for (spo::EdgeId edge_id = 0; edge_id < document.topology().edgeCount(); ++edge_id) {
        const auto* adjacency = document.topology().adjacencyForEdge(edge_id);
        assert(adjacency != nullptr);
        if (adjacency->faces.size() == 1) {
            candidate.boundary_edges.push_back(edge_id);
        } else {
            candidate.internal_edges.push_back(edge_id);
        }
    }
    candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());
    candidate.internal_edge_count = static_cast<int>(candidate.internal_edges.size());
    return candidate;
}

Handle(Geom_BSplineSurface) make_smooth_freeform_surface() {
    TColgp_Array2OfPnt points(1, 7, 1, 7);
    for (int u_index = 1; u_index <= 7; ++u_index) {
        for (int v_index = 1; v_index <= 7; ++v_index) {
            const auto x = 1.5 * static_cast<double>(u_index - 1);
            const auto y = 1.5 * static_cast<double>(v_index - 1);
            const auto z = 0.35 * std::sin(0.35 * x) * std::cos(0.3 * y) + 0.015 * x * y;
            points.SetValue(u_index, v_index, gp_Pnt(x, y, z));
        }
    }

    GeomAPI_PointsToBSplineSurface builder(points, 3, 5, GeomAbs_C2, 1.0e-5);
    assert(builder.IsDone());
    auto surface = builder.Surface();
    assert(!surface.IsNull());
    return surface;
}

TopoDS_Face make_smooth_freeform_face() {
    const auto surface = make_smooth_freeform_surface();
    double u_min = 0.0;
    double u_max = 0.0;
    double v_min = 0.0;
    double v_max = 0.0;
    surface->Bounds(u_min, u_max, v_min, v_max);
    const auto face = BRepBuilderAPI_MakeFace(surface, u_min, u_max, v_min, v_max, 1.0e-7).Face();
    assert(!face.IsNull());
    assert(BRepCheck_Analyzer(face).IsValid());
    return face;
}

RefitFixture make_freeform_fixture() {
    RefitFixture fixture;
    fixture.document = spo::ShapeDocument(make_smooth_freeform_face(), {});
    fixture.candidate = make_all_faces_candidate(fixture.document);
    assert(fixture.document.topology().faceCount() == 1);
    assert(fixture.candidate.boundary_edges.size() == 4);
    return fixture;
}

RefitFixture make_split_freeform_fixture() {
    const auto surface = make_smooth_freeform_surface();
    double u_min = 0.0;
    double u_max = 0.0;
    double v_min = 0.0;
    double v_max = 0.0;
    surface->Bounds(u_min, u_max, v_min, v_max);
    const auto u_middle = 0.5 * (u_min + u_max);

    BRepBuilderAPI_Sewing sewing;
    sewing.Add(BRepBuilderAPI_MakeFace(surface, u_min, u_middle, v_min, v_max, 1.0e-7).Face());
    sewing.Add(BRepBuilderAPI_MakeFace(surface, u_middle, u_max, v_min, v_max, 1.0e-7).Face());
    sewing.Perform();

    RefitFixture fixture;
    fixture.document = spo::ShapeDocument(sewing.SewedShape(), {});
    fixture.candidate = make_all_faces_candidate(fixture.document);
    assert(fixture.document.topology().faceCount() == 2);
    assert(fixture.candidate.internal_edges.size() == 1);
    assert(fixture.candidate.boundary_edges.size() == 6);
    return fixture;
}

RefitFixture make_two_face_planar_fixture() {
    const gp_Pnt p00(0.0, 0.0, 0.0);
    const gp_Pnt p10(2.0, 0.0, 0.0);
    const gp_Pnt p20(4.0, 0.0, 0.0);
    const gp_Pnt p01(0.0, 3.0, 0.0);
    const gp_Pnt p11(2.0, 3.0, 0.0);
    const gp_Pnt p21(4.0, 3.0, 0.0);
    const auto shared = BRepBuilderAPI_MakeEdge(p10, p11).Edge();

    BRepBuilderAPI_MakeWire left;
    left.Add(BRepBuilderAPI_MakeEdge(p00, p10).Edge());
    left.Add(shared);
    left.Add(BRepBuilderAPI_MakeEdge(p11, p01).Edge());
    left.Add(BRepBuilderAPI_MakeEdge(p01, p00).Edge());

    BRepBuilderAPI_MakeWire right;
    right.Add(BRepBuilderAPI_MakeEdge(p10, p20).Edge());
    right.Add(BRepBuilderAPI_MakeEdge(p20, p21).Edge());
    right.Add(BRepBuilderAPI_MakeEdge(p21, p11).Edge());
    right.Add(TopoDS::Edge(shared.Reversed()));

    BRepBuilderAPI_Sewing sewing;
    sewing.Add(BRepBuilderAPI_MakeFace(left.Wire()).Face());
    sewing.Add(BRepBuilderAPI_MakeFace(right.Wire()).Face());
    sewing.Perform();

    RefitFixture fixture;
    fixture.document = spo::ShapeDocument(sewing.SewedShape(), {});
    fixture.candidate = make_all_faces_candidate(fixture.document);
    assert(fixture.document.topology().faceCount() == 2);
    assert(fixture.candidate.internal_edges.size() == 1);
    assert(fixture.candidate.boundary_edges.size() == 6);
    return fixture;
}

RefitFixture make_hole_fixture() {
    BRepBuilderAPI_MakeWire outer;
    outer.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(5.0, 0.0, 0.0)).Edge());
    outer.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(5.0, 0.0, 0.0), gp_Pnt(5.0, 5.0, 0.0)).Edge());
    outer.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(5.0, 5.0, 0.0), gp_Pnt(0.0, 5.0, 0.0)).Edge());
    outer.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(0.0, 5.0, 0.0), gp_Pnt(0.0, 0.0, 0.0)).Edge());

    BRepBuilderAPI_MakeWire inner;
    inner.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(1.5, 1.5, 0.0), gp_Pnt(1.5, 3.5, 0.0)).Edge());
    inner.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(1.5, 3.5, 0.0), gp_Pnt(3.5, 3.5, 0.0)).Edge());
    inner.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(3.5, 3.5, 0.0), gp_Pnt(3.5, 1.5, 0.0)).Edge());
    inner.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(3.5, 1.5, 0.0), gp_Pnt(1.5, 1.5, 0.0)).Edge());

    BRepBuilderAPI_MakeFace face_builder(outer.Wire());
    face_builder.Add(inner.Wire());
    RefitFixture fixture;
    fixture.document = spo::ShapeDocument(face_builder.Face(), {});
    fixture.candidate = make_all_faces_candidate(fixture.document);
    assert(fixture.candidate.boundary_edges.size() == 8);
    return fixture;
}

spo::SurfaceRefitOptions synthetic_options() {
    spo::SurfaceRefitOptions options;
    options.tolerance3d = 0.15;
    options.normal_tolerance_degrees = 25.0;
    options.mesh_deflection = 0.15;
    options.training_sample_count = 16;
    options.validation_sample_count = 24;
    options.boundary_samples_per_edge = 8;
    options.jacobian_grid_size = 8;
    options.max_surface_pole_count = 4096;
    options.max_surface_knot_count = 256;
    options.max_surface_segment_count = 1024;
    return options;
}

void test_surface_refitter_fits_smooth_freeform_surface() {
    const auto fixture = make_split_freeform_fixture();
    const auto options = synthetic_options();
    const auto result = spo::SurfaceRefitter().fitSingleFace(fixture.document, fixture.candidate, options);

    assert(result.success());
    assert(result.report.boundary_valid);
    assert(result.report.candidate_face_count == 2);
    assert(result.report.filling_done);
    assert(result.report.brep_check_valid);
    assert(result.report.training_sample_count == options.training_sample_count);
    assert(result.report.validation_sample_count == options.validation_sample_count);
    assert(result.report.training_validation_overlap_count == 0);
    assert(result.report.interior_error.sample_count == options.validation_sample_count);
    assert(result.report.normal_error.sample_count == options.validation_sample_count);
    assert(result.report.jacobian.sample_count > 0);
    assert(result.report.jacobian.near_singular_count == 0);
    assert(result.report.jacobian.flipped_pair_count == 0);
    assert(!result.report.complexity.surface_type.empty());
    assert(BRepCheck_Analyzer(result.face).IsValid());

    int face_count = 0;
    for (TopExp_Explorer explorer(result.face, TopAbs_FACE); explorer.More(); explorer.Next()) {
        ++face_count;
    }
    assert(face_count == 1);
}

void test_surface_refitter_merges_coplanar_trims_into_one_face() {
    const auto fixture = make_two_face_planar_fixture();
    const auto options = synthetic_options();
    const auto result = spo::SurfaceRefitter().fitSingleFace(fixture.document, fixture.candidate, options);

    assert(result.success());
    assert(result.report.candidate_face_count == 2);
    assert(result.report.boundary_error.over_tolerance_count == 0);
    assert(result.report.interior_error.over_tolerance_count == 0);
    assert(result.report.normal_error.over_tolerance_count == 0);
    assert(result.report.brep_check_valid);
}

void test_surface_refitter_preserves_sharp_trim_corners() {
    const auto fixture = make_freeform_fixture();
    const auto options = synthetic_options();
    const auto result = spo::SurfaceRefitter().fitSingleFace(fixture.document, fixture.candidate, options);

    assert(result.success());
    assert(result.report.corner_error.sample_count == 4);
    assert(result.report.corner_error.projection_failure_count == 0);
    assert(result.report.corner_error.over_tolerance_count == 0);
    assert(result.report.corner_error.max <= options.tolerance3d);
    assert(result.report.boundary_error.over_tolerance_count == 0);
}

void test_surface_refitter_rejects_hole() {
    const auto fixture = make_hole_fixture();
    const auto result = spo::SurfaceRefitter().fitSingleFace(fixture.document, fixture.candidate, synthetic_options());

    assert(!result.success());
    assert(result.report.failure_reason == spo::SurfaceRefitFailureReason::BoundaryInvalid);
    assert(!result.report.boundary_valid);
}

void test_surface_refitter_rejects_open_boundary() {
    auto fixture = make_freeform_fixture();
    fixture.candidate.boundary_edges.pop_back();
    fixture.candidate.boundary_edge_count = static_cast<int>(fixture.candidate.boundary_edges.size());
    const auto result = spo::SurfaceRefitter().fitSingleFace(fixture.document, fixture.candidate, synthetic_options());

    assert(!result.success());
    assert(result.report.failure_reason == spo::SurfaceRefitFailureReason::BoundaryInvalid);
    assert(!result.report.boundary_valid);
}

void test_surface_refitter_rejects_non_manifold_boundary() {
    auto fixture = make_freeform_fixture();
    fixture.candidate.boundary_edges.push_back(fixture.candidate.boundary_edges.front());
    fixture.candidate.boundary_edge_count = static_cast<int>(fixture.candidate.boundary_edges.size());
    const auto result = spo::SurfaceRefitter().fitSingleFace(fixture.document, fixture.candidate, synthetic_options());

    assert(!result.success());
    assert(result.report.failure_reason == spo::SurfaceRefitFailureReason::BoundaryInvalid);
    assert(!result.report.boundary_valid);
}

} // namespace

void run_surface_refitter_tests() {
    test_surface_refitter_fits_smooth_freeform_surface();
    test_surface_refitter_merges_coplanar_trims_into_one_face();
    test_surface_refitter_preserves_sharp_trim_corners();
    test_surface_refitter_rejects_hole();
    test_surface_refitter_rejects_open_boundary();
    test_surface_refitter_rejects_non_manifold_boundary();
}
