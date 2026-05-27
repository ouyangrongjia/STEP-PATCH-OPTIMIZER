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
#include <Geom_BezierSurface.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Shape.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
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

void assert_failure_reason_string(spo::RegionMergeFailureReason reason, const char* expected) {
    const auto* text = spo::regionMergeFailureReasonToString(reason);
    assert(text != nullptr);
    assert(std::string(text) == expected);
}

std::filesystem::path make_temp_file_path(const char* name) {
    auto path = std::filesystem::temp_directory_path();
    path /= name;
    return path;
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

PlaneFixture make_three_face_wavy_nurbs_fixture() {
    const std::array<double, 4> zValues {0.0, 0.0, 0.03, 0.0};

    BRepBuilderAPI_Sewing sewing;
    for (int i = 0; i < 3; ++i) {
        sewing.Add(make_quad_face(
            gp_Pnt(static_cast<double>(i), 0.0, zValues[static_cast<std::size_t>(i)]),
            gp_Pnt(static_cast<double>(i + 1), 0.0, zValues[static_cast<std::size_t>(i + 1)]),
            gp_Pnt(static_cast<double>(i + 1), 1.0, zValues[static_cast<std::size_t>(i + 1)]),
            gp_Pnt(static_cast<double>(i), 1.0, zValues[static_cast<std::size_t>(i)])));
    }
    sewing.Perform();

    BRepBuilderAPI_NurbsConvert converter(sewing.SewedShape());

    PlaneFixture fixture;
    fixture.document = spo::ShapeDocument(converter.Shape(), {});
    assert(fixture.document.topology().faceCount() == 3);
    for (spo::FaceId face = 0; face < fixture.document.topology().faceCount(); ++face) {
        fixture.candidate.faces.push_back(face);
    }
    for (spo::EdgeId edge = 0; edge < fixture.document.topology().edgeCount(); ++edge) {
        const auto* adjacency = fixture.document.topology().adjacencyForEdge(edge);
        assert(adjacency != nullptr);
        if (adjacency->faces.size() == 2) {
            fixture.candidate.internal_edges.push_back(edge);
        } else if (adjacency->faces.size() == 1) {
            fixture.candidate.boundary_edges.push_back(edge);
        }
    }

    fixture.candidate.candidate_id = 11;
    fixture.candidate.candidate_type = spo::MergeCandidateType::PlaneLike;
    fixture.candidate.status = spo::MergeCandidateStatus::Accepted;
    fixture.candidate.face_count = static_cast<int>(fixture.candidate.faces.size());
    fixture.candidate.boundary_edge_count = static_cast<int>(fixture.candidate.boundary_edges.size());
    fixture.candidate.internal_edge_count = static_cast<int>(fixture.candidate.internal_edges.size());
    assert(fixture.candidate.boundary_edges.size() == 8);
    assert(fixture.candidate.internal_edges.size() == 2);
    return fixture;
}

TopoDS_Face make_curved_boundary_bezier_face(double x0, double x1) {
    TColgp_Array2OfPnt poles(1, 3, 1, 3);
    for (int u = 1; u <= 3; ++u) {
        const double tx = static_cast<double>(u - 1) / 2.0;
        for (int v = 1; v <= 3; ++v) {
            const double ty = static_cast<double>(v - 1) / 2.0;
            const double z = 0.03 * ty * ty;
            poles.SetValue(u, v, gp_Pnt(x0 + (x1 - x0) * tx, ty, z));
        }
    }
    Handle(Geom_BezierSurface) surface = new Geom_BezierSurface(poles);
    return BRepBuilderAPI_MakeFace(surface, 0.0, 1.0, 0.0, 1.0, Precision::Confusion()).Face();
}

PlaneFixture make_two_face_curved_boundary_nurbs_fixture() {
    BRepBuilderAPI_Sewing sewing;
    sewing.Add(make_curved_boundary_bezier_face(0.0, 1.0));
    sewing.Add(make_curved_boundary_bezier_face(1.0, 2.0));
    sewing.Perform();

    PlaneFixture fixture;
    fixture.document = spo::ShapeDocument(sewing.SewedShape(), {});
    assert(fixture.document.topology().faceCount() == 2);
    for (spo::FaceId face = 0; face < fixture.document.topology().faceCount(); ++face) {
        fixture.candidate.faces.push_back(face);
    }
    for (spo::EdgeId edge = 0; edge < fixture.document.topology().edgeCount(); ++edge) {
        const auto* adjacency = fixture.document.topology().adjacencyForEdge(edge);
        assert(adjacency != nullptr);
        if (adjacency->faces.size() == 2) {
            fixture.candidate.internal_edges.push_back(edge);
        } else if (adjacency->faces.size() == 1) {
            fixture.candidate.boundary_edges.push_back(edge);
        }
    }

    fixture.candidate.candidate_id = 13;
    fixture.candidate.candidate_type = spo::MergeCandidateType::PlaneLike;
    fixture.candidate.status = spo::MergeCandidateStatus::Accepted;
    fixture.candidate.face_count = static_cast<int>(fixture.candidate.faces.size());
    fixture.candidate.boundary_edge_count = static_cast<int>(fixture.candidate.boundary_edges.size());
    fixture.candidate.internal_edge_count = static_cast<int>(fixture.candidate.internal_edges.size());
    assert(fixture.candidate.boundary_edges.size() == 6);
    assert(fixture.candidate.internal_edges.size() == 1);
    return fixture;
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

void test_plane_region_merge_options_default_strict_mode() {
    const spo::PlaneRegionMergeOptions options;
    assert(!options.allow_approximate_planar_surfaces);
    assert(options.approximate_plane_max_deviation == 0.01);
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
    assert(result.brep_check_valid);
    assert(result.message == "Plane region merge completed with export roundtrip validation passed.");
    assert(result.document.hasShape());
    assert(result.face_count_after < result.face_count_before);
    assert(result.document.stats().faces > 0);
    assert(result.document.stats().edges > 0);
}

void test_plane_region_merger_roundtrip_failure_does_not_change_result_document() {
    const auto fixture = make_two_face_plane_fixture();
    const auto before = fixture.document.stats();
    const spo::PlaneRegionMerger merger;
    spo::PlaneRegionMergeOptions options;
    const auto tempFile = make_temp_file_path("step-patch-optimizer-roundtrip-not-a-directory.tmp");
    {
        std::ofstream stream(tempFile.string());
        stream << "not a directory";
    }
    options.roundtrip_temp_directory = tempFile;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);

    std::error_code ignored;
    std::filesystem::remove(tempFile, ignored);
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::ExportRoundtripFailed);
    assert(result.document.hasShape());
    assert(same_stats(result.document.stats(), before));
    assert(same_stats(fixture.document.stats(), before));
}

void test_plane_region_merger_rejects_nurbs_backed_planar_region() {
    const auto fixture = make_two_face_plane_fixture(true);
    const auto before = fixture.document.stats();
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::ApproximateSurfaceNotSupported);
    assert(result.message == "B-spline backed planar-like candidate is preview-only and not supported by strict PlaneRegionMerge.");
    assert(result.document.hasShape());
    assert(same_stats(result.document.stats(), before));
    assert(same_stats(fixture.document.stats(), before));
}

void test_plane_region_merger_approx_mode_merges_low_deviation_nurbs_planar_region() {
    const auto fixture = make_two_face_plane_fixture(true);
    const auto before = fixture.document.stats();
    const spo::PlaneRegionMerger merger;
    spo::PlaneRegionMergeOptions options;
    options.allow_approximate_planar_surfaces = true;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    assert(result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::None);
    assert(result.message == "Approximate planar B-spline candidate rebuilt as planar trimmed face with export roundtrip validation passed.");
    assert(result.brep_check_valid);
    assert(result.document.stats().faces < before.faces);
    assert(same_stats(fixture.document.stats(), before));
}

void test_plane_region_merger_approx_mode_rejects_high_deviation_nurbs_planar_region() {
    const auto fixture = make_three_face_wavy_nurbs_fixture();
    const auto before = fixture.document.stats();
    const spo::PlaneRegionMerger merger;
    spo::PlaneRegionMergeOptions options;
    options.allow_approximate_planar_surfaces = true;
    options.normal_angle_tolerance_degrees = 45.0;
    options.approximate_plane_max_deviation = 1.0e-6;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::DeviationTooLarge);
    assert(result.document.hasShape());
    assert(same_stats(result.document.stats(), before));
    assert(same_stats(fixture.document.stats(), before));
}

void test_plane_region_merger_approx_mode_rejects_invalid_boundary_from_analyzer() {
    auto fixture = make_two_face_plane_fixture(true);
    const auto before = fixture.document.stats();
    fixture.candidate.boundary_edges.pop_back();
    const spo::PlaneRegionMerger merger;
    spo::PlaneRegionMergeOptions options;
    options.allow_approximate_planar_surfaces = true;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::BoundaryLoopInvalid);
    assert(result.message == "Candidate boundary edges do not form closed loops.");
    assert(result.document.hasShape());
    assert(same_stats(result.document.stats(), before));
    assert(same_stats(fixture.document.stats(), before));
}

void test_plane_region_merger_approx_mode_rejects_boundary_curves_outside_fitted_plane() {
    const auto fixture = make_two_face_curved_boundary_nurbs_fixture();
    const auto before = fixture.document.stats();
    const spo::PlaneRegionMerger merger;
    spo::PlaneRegionMergeOptions options;
    options.allow_approximate_planar_surfaces = true;
    options.normal_angle_tolerance_degrees = 45.0;
    options.approximate_plane_max_deviation = 0.01;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);

    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::DeviationTooLarge);
    assert(result.message == "Plane candidate boundary edges exceed fitted plane tolerance.");
    assert(result.document.hasShape());
    assert(same_stats(result.document.stats(), before));
    assert(same_stats(fixture.document.stats(), before));
}

void test_plane_region_merger_approx_batch_skips_invalid_candidate_and_merges_valid_one() {
    auto fixture = make_two_face_plane_fixture(true);
    const auto before = fixture.document.stats();
    auto invalid = fixture.candidate;
    invalid.candidate_id = 12;
    invalid.boundary_edges.pop_back();
    const spo::PlaneRegionMerger merger;
    spo::PlaneRegionMergeOptions options;
    options.allow_approximate_planar_surfaces = true;

    const auto result = merger.mergeBatch(fixture.document, {fixture.candidate, invalid}, options);

    assert(result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::None);
    assert(result.message.find("merged 1 candidates, skipped 1") != std::string::npos);
    assert(result.message.find("export roundtrip validation passed") != std::string::npos);
    assert(result.brep_check_valid);
    assert(result.document.hasShape());
    assert(result.document.stats().faces < before.faces);
    assert(same_stats(fixture.document.stats(), before));
}

void test_plane_region_merger_approx_batch_fails_when_all_candidates_invalid() {
    auto fixture = make_two_face_plane_fixture(true);
    const auto before = fixture.document.stats();
    fixture.candidate.boundary_edges.pop_back();
    const spo::PlaneRegionMerger merger;
    spo::PlaneRegionMergeOptions options;
    options.allow_approximate_planar_surfaces = true;

    const auto result = merger.mergeBatch(fixture.document, {fixture.candidate}, options);

    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::CandidateNotFound);
    assert(result.document.hasShape());
    assert(same_stats(result.document.stats(), before));
    assert(same_stats(fixture.document.stats(), before));
}

void test_plane_region_merger_approx_batch_roundtrip_failure_does_not_change_result_document() {
    const auto fixture = make_two_face_plane_fixture(true);
    const auto before = fixture.document.stats();
    const spo::PlaneRegionMerger merger;
    spo::PlaneRegionMergeOptions options;
    options.allow_approximate_planar_surfaces = true;
    const auto tempFile = make_temp_file_path("step-patch-optimizer-approx-batch-roundtrip-not-a-directory.tmp");
    {
        std::ofstream stream(tempFile.string());
        stream << "not a directory";
    }
    options.roundtrip_temp_directory = tempFile;

    const auto result = merger.mergeBatch(fixture.document, {fixture.candidate}, options);

    std::error_code ignored;
    std::filesystem::remove(tempFile, ignored);
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::ExportRoundtripFailed);
    assert(result.document.hasShape());
    assert(same_stats(result.document.stats(), before));
    assert(same_stats(fixture.document.stats(), before));
}

void test_plane_region_merger_preserves_solid_container() {
    const auto fixture = make_split_solid_top_fixture();
    const auto before = fixture.document.stats();
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);

    assert(result.success);
    assert(result.brep_check_valid);
    assert(result.document.stats().solids == before.solids);
    assert(result.document.stats().faces < before.faces);
    assert(result.document.stats().edges < before.edges - 1);
}

void test_region_merge_result_default_primitive_fields() {
    const spo::RegionMergeResult result;
    assert(result.primitive_center_x == 0.0);
    assert(result.primitive_center_y == 0.0);
    assert(result.primitive_center_z == 0.0);
    assert(result.primitive_axis_x == 0.0);
    assert(result.primitive_axis_y == 0.0);
    assert(result.primitive_axis_z == 0.0);
    assert(result.primitive_radius == 0.0);
    assert(result.primitive_secondary_radius == 0.0);
    assert(result.primitive_angle_degrees == 0.0);
    assert(result.primitive_fit_error == 0.0);
}

void test_region_merge_failure_reason_strings_cover_all_values() {
    assert_failure_reason_string(spo::RegionMergeFailureReason::None, "None");
    assert_failure_reason_string(spo::RegionMergeFailureReason::NotImplemented, "NotImplemented");
    assert_failure_reason_string(spo::RegionMergeFailureReason::NotSupported, "NotSupported");
    assert_failure_reason_string(spo::RegionMergeFailureReason::CandidateNotFound, "CandidateNotFound");
    assert_failure_reason_string(spo::RegionMergeFailureReason::InvalidCandidate, "InvalidCandidate");
    assert_failure_reason_string(spo::RegionMergeFailureReason::UnsupportedCandidateType, "UnsupportedCandidateType");
    assert_failure_reason_string(spo::RegionMergeFailureReason::RejectedCandidate, "RejectedCandidate");
    assert_failure_reason_string(spo::RegionMergeFailureReason::HiddenCandidate, "HiddenCandidate");
    assert_failure_reason_string(spo::RegionMergeFailureReason::InsufficientFaces, "InsufficientFaces");
    assert_failure_reason_string(spo::RegionMergeFailureReason::ProtectedEdgeConflict, "ProtectedEdgeConflict");
    assert_failure_reason_string(spo::RegionMergeFailureReason::LockedEdgeConflict, "LockedEdgeConflict");
    assert_failure_reason_string(spo::RegionMergeFailureReason::BoundaryLoopInvalid, "BoundaryLoopInvalid");
    assert_failure_reason_string(spo::RegionMergeFailureReason::MultipleOuterLoopsNotSupported, "MultipleOuterLoopsNotSupported");
    assert_failure_reason_string(spo::RegionMergeFailureReason::InnerLoopsNotSupported, "InnerLoopsNotSupported");
    assert_failure_reason_string(spo::RegionMergeFailureReason::PrimitiveFitFailed, "PrimitiveFitFailed");
    assert_failure_reason_string(spo::RegionMergeFailureReason::DeviationTooLarge, "DeviationTooLarge");
    assert_failure_reason_string(spo::RegionMergeFailureReason::SurfaceConstructionFailed, "SurfaceConstructionFailed");
    assert_failure_reason_string(spo::RegionMergeFailureReason::TopologyReplacementFailed, "TopologyReplacementFailed");
    assert_failure_reason_string(spo::RegionMergeFailureReason::SewingFailed, "SewingFailed");
    assert_failure_reason_string(spo::RegionMergeFailureReason::ValidationFailed, "ValidationFailed");
    assert_failure_reason_string(spo::RegionMergeFailureReason::ExportRoundtripFailed, "ExportRoundtripFailed");
    assert_failure_reason_string(spo::RegionMergeFailureReason::ApproximateSurfaceNotSupported, "ApproximateSurfaceNotSupported");
}

void test_plane_region_merger_fills_primitive_fields_on_success() {
    const auto fixture = make_two_face_plane_fixture();
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    assert(result.success);
    assert(result.plane_normal_x != 0.0 || result.plane_normal_y != 0.0 || result.plane_normal_z != 0.0);

    assert(result.primitive_axis_x == result.plane_normal_x);
    assert(result.primitive_axis_y == result.plane_normal_y);
    assert(result.primitive_axis_z == result.plane_normal_z);

    assert(result.primitive_fit_error > 0.0 || result.primitive_fit_error == result.max_deviation);
    assert(result.primitive_fit_error <= options.max_deviation);

    assert(result.primitive_center_x == 0.0);
    assert(result.primitive_center_y == 0.0);
    assert(result.primitive_center_z == 0.0);
    assert(result.primitive_radius == 0.0);
    assert(result.primitive_secondary_radius == 0.0);
    assert(result.primitive_angle_degrees == 0.0);
}

void test_plane_region_merger_failure_on_nurbs_does_not_fill_primitive_fields() {
    const auto fixture = make_two_face_plane_fixture(true);
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::ApproximateSurfaceNotSupported);
    assert(result.plane_normal_x == 0.0);
    assert(result.plane_normal_y == 0.0);
    assert(result.plane_normal_z == 0.0);
    assert(result.primitive_axis_x == 0.0);
    assert(result.primitive_axis_y == 0.0);
    assert(result.primitive_axis_z == 0.0);
    assert(result.primitive_fit_error == 0.0);
}

void test_plane_region_merger_failure_does_not_fill_primitive_fields() {
    const auto fixture = make_two_face_plane_fixture();
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;

    auto wrong_type = fixture.candidate;
    wrong_type.candidate_type = spo::MergeCandidateType::CylinderLike;
    const auto result = merger.merge(fixture.document, wrong_type, options);
    assert(!result.success);
    assert(result.primitive_axis_x == 0.0);
    assert(result.primitive_axis_y == 0.0);
    assert(result.primitive_axis_z == 0.0);
    assert(result.primitive_fit_error == 0.0);
}

}

void run_plane_region_merger_tests() {
    test_region_merge_failure_reason_strings_cover_all_values();
    test_region_merge_result_default_primitive_fields();
    test_plane_region_merge_options_default_strict_mode();
    test_plane_region_merger_rejects_invalid_candidate_states();
    test_plane_region_merger_rejects_small_or_protected_regions();
    test_plane_region_merger_rejects_invalid_boundary_without_changing_stats();
    test_plane_region_merger_merges_simple_coplanar_region();
    test_plane_region_merger_roundtrip_failure_does_not_change_result_document();
    test_plane_region_merger_rejects_nurbs_backed_planar_region();
    test_plane_region_merger_approx_mode_merges_low_deviation_nurbs_planar_region();
    test_plane_region_merger_approx_mode_rejects_high_deviation_nurbs_planar_region();
    test_plane_region_merger_approx_mode_rejects_invalid_boundary_from_analyzer();
    test_plane_region_merger_approx_mode_rejects_boundary_curves_outside_fitted_plane();
    test_plane_region_merger_approx_batch_skips_invalid_candidate_and_merges_valid_one();
    test_plane_region_merger_approx_batch_fails_when_all_candidates_invalid();
    test_plane_region_merger_approx_batch_roundtrip_failure_does_not_change_result_document();
    test_plane_region_merger_preserves_solid_container();
    test_plane_region_merger_fills_primitive_fields_on_success();
    test_plane_region_merger_failure_on_nurbs_does_not_fill_primitive_fields();
    test_plane_region_merger_failure_does_not_fill_primitive_fields();
}
