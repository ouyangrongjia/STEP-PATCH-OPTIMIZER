#include "brep/ShapeDocument.h"
#include "merge/SurfaceRefitDiagnostics.h"
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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

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

bool has_failed_gate(
    const spo::SurfaceRefitReport& report,
    const spo::SurfaceRefitFailureReason reason) {
    return std::find(report.failed_gates.begin(), report.failed_gates.end(), reason) !=
        report.failed_gates.end();
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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
    assert(result.report.filling_face_brep.performed);
    assert(result.report.oriented_face_brep.performed);
    assert(result.report.filling_face_brep_check_valid == result.report.filling_face_brep.valid);
    assert(result.report.oriented_face_brep_check_valid == result.report.oriented_face_brep.valid);
    assert(result.report.brep_check_valid == result.report.oriented_face_brep_check_valid);
    assert(result.report.filling_face_brep.face_count == 1);
    assert(result.report.oriented_face_brep.face_count == 1);
    assert(result.report.oriented_face_brep.edge_count == result.report.output_edge_count);
    assert(result.report.oriented_face_brep.edges.size() ==
        static_cast<std::size_t>(result.report.output_edge_count));
    bool saw_face_status = false;
    bool saw_wire_status = false;
    bool saw_edge_status = false;
    bool saw_vertex_status = false;
    for (const auto& status : result.report.oriented_face_brep.subshape_statuses) {
        assert(!status.statuses.empty());
        for (const auto& name : status.statuses) {
            assert(!name.empty());
        }
        saw_face_status = saw_face_status || status.shape_type == "face";
        saw_wire_status = saw_wire_status || status.shape_type == "wire";
        saw_edge_status = saw_edge_status || status.shape_type == "edge";
        saw_vertex_status = saw_vertex_status || status.shape_type == "vertex";
    }
    assert(saw_face_status && saw_wire_status && saw_edge_status && saw_vertex_status);
    for (const auto& edge : result.report.oriented_face_brep.edges) {
        assert(edge.edge_index >= 0);
        assert(edge.has_pcurve);
        assert(edge.same_range);
        assert(edge.same_parameter);
        assert(edge.curve_on_surface_deviation_computed || edge.degenerated);
    }
    assert(result.report.training_sample_count == options.training_sample_count);
    assert(result.report.validation_sample_count == options.validation_sample_count);
    assert(result.report.training_validation_overlap_count == 0);
    assert(result.report.failed_gates.empty());
    assert(result.report.interior_error.sample_count == options.validation_sample_count);
    assert(result.report.normal_error.sample_count == options.validation_sample_count);
    assert(result.report.normal_error_samples.size() ==
        static_cast<std::size_t>(options.validation_sample_count));
    assert(result.report.face_sampling.size() == fixture.candidate.faces.size());
    int per_face_training_total = 0;
    int per_face_validation_total = 0;
    for (const auto& face : result.report.face_sampling) {
        per_face_training_total += face.training_count;
        per_face_validation_total += face.validation_count;
    }
    assert(per_face_training_total == options.training_sample_count);
    assert(per_face_validation_total == options.validation_sample_count);
    assert(result.report.surface_error_samples.size() ==
        static_cast<std::size_t>(
            result.report.boundary_error.sample_count +
            result.report.corner_error.sample_count +
            result.report.interior_error.sample_count));
    assert(result.report.jacobian.sample_count > 0);
    assert(result.report.jacobian.near_singular_count == 0);
    assert(result.report.jacobian.flipped_pair_count == 0);
    assert(!result.report.complexity.surface_type.empty());
    for (const auto& sample : result.report.surface_error_samples) {
        if (sample.category == spo::SurfaceRefitSampleCategory::Interior) {
            assert(sample.nearest_training_distance_defined);
            assert(sample.nearest_training_distance > 0.0);
        }
    }
    for (const auto& sample : result.report.normal_error_samples) {
        assert(sample.source_normal.SquareMagnitude() > 0.0);
        assert(sample.projection_success);
        assert(sample.fitted_normal_defined);
        assert(std::isfinite(sample.angle_degrees));
        assert(std::isfinite(sample.u));
        assert(std::isfinite(sample.v));
    }
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
    assert(has_failed_gate(result.report, spo::SurfaceRefitFailureReason::BoundaryInvalid));
    assert(!result.report.boundary_valid);
}

void test_surface_refitter_reports_all_failed_quality_gates() {
    const auto fixture = make_split_freeform_fixture();
    auto options = synthetic_options();
    options.tolerance3d = 5.0e-6;
    options.training_sample_count = 4;
    options.validation_sample_count = 48;

    const auto result = spo::SurfaceRefitter().fitSingleFace(
        fixture.document,
        fixture.candidate,
        options);

    assert(!result.success());
    assert(result.report.failure_reason == spo::SurfaceRefitFailureReason::BoundaryErrorExceeded);
    assert(has_failed_gate(result.report, spo::SurfaceRefitFailureReason::BoundaryErrorExceeded));
    assert(has_failed_gate(result.report, spo::SurfaceRefitFailureReason::InteriorErrorExceeded));
}

void test_face_stratified_sampling_is_deterministic_and_covers_every_source_face() {
    const auto fixture = make_split_freeform_fixture();
    auto options = synthetic_options();
    options.sampling_strategy = spo::SurfaceRefitSamplingStrategy::FaceStratifiedAreaWeighted;

    const auto first = spo::SurfaceRefitter().fitSingleFace(
        fixture.document,
        fixture.candidate,
        options);
    const auto second = spo::SurfaceRefitter().fitSingleFace(
        fixture.document,
        fixture.candidate,
        options);

    assert(first.report.training_sample_count == options.training_sample_count);
    assert(first.report.validation_sample_count == options.validation_sample_count);
    assert(first.report.training_validation_overlap_count == 0);
    assert(first.report.face_sampling.size() == fixture.candidate.faces.size());
    assert(first.report.face_sampling.size() == second.report.face_sampling.size());
    for (std::size_t index = 0; index < first.report.face_sampling.size(); ++index) {
        const auto& first_face = first.report.face_sampling[index];
        const auto& second_face = second.report.face_sampling[index];
        assert(first_face.training_count >= 1);
        assert(first_face.face_id == second_face.face_id);
        assert(first_face.training_count == second_face.training_count);
        assert(first_face.validation_count == second_face.validation_count);
    }
    assert(std::abs(first.report.interior_error.max - second.report.interior_error.max) < 1.0e-12);
    assert(std::abs(first.report.normal_error.max_angle_degrees -
        second.report.normal_error.max_angle_degrees) < 1.0e-12);
}

void test_face_stratified_sampling_rejects_face_count_above_fixed_budget() {
    const auto fixture = make_split_freeform_fixture();
    auto options = synthetic_options();
    options.sampling_strategy = spo::SurfaceRefitSamplingStrategy::FaceStratifiedAreaWeighted;
    options.training_sample_count = 1;

    const auto result = spo::SurfaceRefitter().fitSingleFace(
        fixture.document,
        fixture.candidate,
        options);

    assert(!result.success());
    assert(result.report.failure_reason ==
        spo::SurfaceRefitFailureReason::SamplingConfigurationUnsupported);
    assert(has_failed_gate(
        result.report,
        spo::SurfaceRefitFailureReason::SamplingConfigurationUnsupported));
    assert(result.report.training_sample_count == 0);
}

void test_surface_refit_diagnostic_file_contract() {
    spo::SurfaceRefitReport report;
    spo::SurfaceRefitSampleDiagnostic boundary;
    boundary.category = spo::SurfaceRefitSampleCategory::Boundary;
    boundary.source_point = gp_Pnt(1.0, 2.0, 3.0);
    boundary.projected_point = gp_Pnt(1.1, 2.0, 3.0);
    boundary.distance = 0.1;
    boundary.projection_success = true;
    boundary.over_tolerance = true;
    boundary.edge_id = 9;
    report.surface_error_samples.push_back(boundary);

    spo::SurfaceRefitSampleDiagnostic interior;
    interior.category = spo::SurfaceRefitSampleCategory::Interior;
    interior.source_point = gp_Pnt(4.0, 5.0, 6.0);
    interior.projected_point = gp_Pnt(4.0, 5.0, 6.01);
    interior.distance = 0.01;
    interior.projection_success = true;
    interior.over_tolerance = false;
    interior.face_id = 7;
    interior.nearest_training_distance_defined = true;
    interior.nearest_training_distance = 0.25;
    report.surface_error_samples.push_back(interior);

    spo::SurfaceRefitSampleDiagnostic corner;
    corner.category = spo::SurfaceRefitSampleCategory::Corner;
    corner.source_point = gp_Pnt(7.0, 8.0, 9.0);
    corner.projection_success = false;
    corner.over_tolerance = true;
    corner.edge_id = 11;
    report.surface_error_samples.push_back(corner);

    spo::SurfaceRefitNormalSampleDiagnostic normal;
    normal.source_point = gp_Pnt(10.0, 11.0, 12.0);
    normal.projected_point = gp_Pnt(10.0, 11.0, 12.02);
    normal.source_normal = gp_Vec(0.0, 0.0, 1.0);
    normal.fitted_normal = gp_Vec(0.0, 0.5, std::sqrt(0.75));
    normal.angle_degrees = 30.0;
    normal.u = 0.2;
    normal.v = 0.7;
    normal.face_id = 13;
    normal.projection_success = true;
    normal.fitted_normal_defined = true;
    normal.over_tolerance = true;
    report.normal_error_samples.push_back(normal);

    const auto directory =
        std::filesystem::temp_directory_path() / "spo_surface_refit_diagnostic_contract";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    const auto artifacts = spo::writeSurfaceRefitDiagnosticArtifacts(report, directory);

    assert(artifacts.success());
    assert(std::filesystem::exists(artifacts.csv_path));
    assert(std::filesystem::exists(artifacts.obj_path));
    assert(std::filesystem::exists(artifacts.normal_csv_path));

    const auto csv = read_text_file(artifacts.csv_path);
    assert(csv.find(
        "category,source_x,source_y,source_z,projected_x,projected_y,projected_z,"
        "distance,projection_success,over_tolerance,face_id,edge_id,nearest_training_distance") != std::string::npos);
    assert(csv.find("boundary,1,2,3,1.1000000000000001,2,3,0.10000000000000001,true,true,,9,") !=
        std::string::npos);
    assert(csv.find("interior,4,5,6,4,5,6.0099999999999998,0.01,true,false,7,,0.25") !=
        std::string::npos);
    assert(csv.find("corner,7,8,9,,,,,false,true,,11,") != std::string::npos);

    const auto obj = read_text_file(artifacts.obj_path);
    assert(obj.find("# category=boundary") != std::string::npos);
    assert(obj.find("# category=interior") == std::string::npos);
    assert(obj.find("# category=corner") == std::string::npos);
    assert(obj.find("v 1 2 3") != std::string::npos);
    assert(obj.find("v 1.1000000000000001 2 3") != std::string::npos);
    assert(obj.find("l 1 2") != std::string::npos);

    const auto normal_csv = read_text_file(artifacts.normal_csv_path);
    assert(normal_csv.find(
        "source_x,source_y,source_z,projected_x,projected_y,projected_z,"
        "source_normal_x,source_normal_y,source_normal_z,"
        "fitted_normal_x,fitted_normal_y,fitted_normal_z,"
        "angle_degrees,u,v,face_id,projection_success,fitted_normal_defined,over_tolerance") !=
        std::string::npos);
    assert(normal_csv.find("10,11,12,10,11,12.02,0,0,1,0,0.5") != std::string::npos);
    assert(normal_csv.find(",30,0.20000000000000001,0.69999999999999996,13,true,true,true") !=
        std::string::npos);

    std::filesystem::remove_all(directory, error);
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
    test_surface_refitter_reports_all_failed_quality_gates();
    test_face_stratified_sampling_is_deterministic_and_covers_every_source_face();
    test_face_stratified_sampling_rejects_face_count_above_fixed_budget();
    test_surface_refit_diagnostic_file_contract();
}
