#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StlReader.h"
#include "io/StlWriter.h"
#include "merge/MergePlanner.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "stl/StlRegionExtractor.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>

namespace {

constexpr double kTolerance = 1.0e-9;

struct ExtractFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
};

spo::StlTriangle make_triangle(
    spo::StlVec3 normal,
    spo::StlVec3 v0,
    spo::StlVec3 v1,
    spo::StlVec3 v2) {
    spo::StlTriangle triangle;
    triangle.normal = normal;
    triangle.v0 = v0;
    triangle.v1 = v1;
    triangle.v2 = v2;
    return triangle;
}

spo::StlMesh make_mesh(std::initializer_list<spo::StlTriangle> triangles) {
    spo::StlMesh mesh;
    for (const auto& triangle : triangles) {
        mesh.addTriangle(triangle);
    }
    return mesh;
}

std::filesystem::path find_sample_path(const std::filesystem::path& relativePath) {
    auto current = std::filesystem::current_path();
    for (int depth = 0; depth < 6; ++depth) {
        const auto candidate = current / relativePath;
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
        if (!current.has_parent_path() || current == current.parent_path()) {
            break;
        }
        current = current.parent_path();
    }
    return {};
}

ExtractFixture make_two_face_fixture() {
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

    ExtractFixture fixture;
    fixture.document = spo::ShapeDocument(sewing.SewedShape(), {});
    assert(fixture.document.topology().faceCount() == 2);

    fixture.candidate.candidate_id = 17;
    fixture.candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    fixture.candidate.faces = {0, 1};
    fixture.candidate.face_count = 2;
    return fixture;
}

TopoDS_Face make_square_face(double x0, double y0, double x1, double y1) {
    const gp_Pnt p00(x0, y0, 0.0);
    const gp_Pnt p10(x1, y0, 0.0);
    const gp_Pnt p11(x1, y1, 0.0);
    const gp_Pnt p01(x0, y1, 0.0);

    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p00, p10).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p10, p11).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p11, p01).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p01, p00).Edge());
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}

ExtractFixture make_l_shape_fixture() {
    BRepBuilderAPI_Sewing sewing;
    sewing.Add(make_square_face(0.0, 0.0, 1.0, 1.0));
    sewing.Add(make_square_face(1.0, 0.0, 2.0, 1.0));
    sewing.Add(make_square_face(0.0, 1.0, 1.0, 2.0));
    sewing.Perform();

    ExtractFixture fixture;
    fixture.document = spo::ShapeDocument(sewing.SewedShape(), {});
    assert(fixture.document.topology().faceCount() == 3);

    fixture.candidate.candidate_id = 23;
    fixture.candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    fixture.candidate.face_count = static_cast<int>(fixture.document.topology().faceCount());
    for (spo::FaceId faceId = 0; faceId < fixture.document.topology().faceCount(); ++faceId) {
        fixture.candidate.faces.push_back(faceId);
    }
    return fixture;
}

double bbox_diagonal(const spo::StlBoundingBox& bbox) {
    const auto dx = bbox.max.x - bbox.min.x;
    const auto dy = bbox.max.y - bbox.min.y;
    const auto dz = bbox.max.z - bbox.min.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool bbox_intersects(const spo::StlBoundingBox& lhs, const spo::StlBoundingBox& rhs) {
    return lhs.valid && rhs.valid &&
        lhs.min.x <= rhs.max.x && lhs.max.x >= rhs.min.x &&
        lhs.min.y <= rhs.max.y && lhs.max.y >= rhs.min.y &&
        lhs.min.z <= rhs.max.z && lhs.max.z >= rhs.min.z;
}

void assert_failed_with_message(const spo::StlRegionExtractResult& result) {
    assert(!result.success);
    assert(!result.report.success);
    assert(!result.report.message.empty());
}

void test_extract_keeps_intersecting_triangles_and_excludes_outside() {
    const auto fixture = make_two_face_fixture();
    const auto inside = make_triangle(
        {0.0, 0.0, 1.0},
        {0.25, 0.25, 0.0},
        {0.75, 0.25, 0.0},
        {0.25, 0.75, 0.0});
    const auto outside = make_triangle(
        {0.0, 0.0, 1.0},
        {10.0, 10.0, 0.0},
        {11.0, 10.0, 0.0},
        {10.0, 11.0, 0.0});
    const auto source = make_mesh({inside, outside});
    spo::StlRegionExtractorOptions options;
    options.bboxMarginRatio = 0.0;
    options.minMargin = 0.0;

    const auto result = spo::StlRegionExtractor().extract(fixture.document, fixture.candidate, source, options);

    assert(result.success);
    assert(result.localMesh.triangleCount() == 1);
    assert(result.report.output_triangle_count == 1);
    assert(result.localMesh.triangles().front().v0.x == inside.v0.x);
}

void test_extract_excludes_triangles_inside_bbox_but_outside_candidate_boundary() {
    const auto fixture = make_l_shape_fixture();
    const auto insideBoundary = make_triangle(
        {0.0, 0.0, 1.0},
        {0.20, 0.20, 0.0},
        {0.80, 0.20, 0.0},
        {0.20, 0.80, 0.0});
    const auto insideBBoxOnly = make_triangle(
        {0.0, 0.0, 1.0},
        {1.20, 1.20, 0.0},
        {1.80, 1.20, 0.0},
        {1.20, 1.80, 0.0});
    const auto source = make_mesh({insideBoundary, insideBBoxOnly});
    spo::StlRegionExtractorOptions options;
    options.bboxMarginRatio = 0.0;
    options.minMargin = 0.0;

    const auto result = spo::StlRegionExtractor().extract(fixture.document, fixture.candidate, source, options);

    assert(result.success);
    assert(result.localMesh.triangleCount() == 1);
    assert(result.report.output_triangle_count == 1);
    assert(result.localMesh.triangles().front().v0.x == insideBoundary.v0.x);
}

void test_margin_growth_does_not_reduce_triangle_count() {
    const auto fixture = make_two_face_fixture();
    const auto inside = make_triangle(
        {0.0, 0.0, 1.0},
        {0.25, 0.25, 0.0},
        {0.75, 0.25, 0.0},
        {0.25, 0.75, 0.0});
    const auto nearOutside = make_triangle(
        {0.0, 0.0, 1.0},
        {2.05, 0.25, 0.0},
        {2.10, 0.25, 0.0},
        {2.05, 0.75, 0.0});
    const auto source = make_mesh({inside, nearOutside});

    spo::StlRegionExtractorOptions smallMargin;
    smallMargin.bboxMarginRatio = 0.0;
    smallMargin.minMargin = 0.0;
    spo::StlRegionExtractorOptions largeMargin;
    largeMargin.bboxMarginRatio = 0.0;
    largeMargin.minMargin = 0.2;

    const spo::StlRegionExtractor extractor;
    const auto small = extractor.extract(fixture.document, fixture.candidate, source, smallMargin);
    const auto large = extractor.extract(fixture.document, fixture.candidate, source, largeMargin);

    assert(small.success);
    assert(large.success);
    assert(large.report.output_triangle_count >= small.report.output_triangle_count);
}

void test_empty_source_mesh_fails() {
    const auto fixture = make_two_face_fixture();
    const spo::StlMesh source;

    const auto result = spo::StlRegionExtractor().extract(fixture.document, fixture.candidate, source);

    assert_failed_with_message(result);
    assert(result.report.source_triangle_count == 0);
}

void test_empty_candidate_faces_fail() {
    auto fixture = make_two_face_fixture();
    fixture.candidate.faces.clear();
    fixture.candidate.face_count = 0;
    const auto source = make_mesh({make_triangle(
        {0.0, 0.0, 1.0},
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0})});

    const auto result = spo::StlRegionExtractor().extract(fixture.document, fixture.candidate, source);

    assert_failed_with_message(result);
}

void test_candidate_face_id_out_of_range_fails() {
    auto fixture = make_two_face_fixture();
    fixture.candidate.faces = {fixture.document.topology().faceCount()};
    const auto source = make_mesh({make_triangle(
        {0.0, 0.0, 1.0},
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0})});

    const auto result = spo::StlRegionExtractor().extract(fixture.document, fixture.candidate, source);

    assert_failed_with_message(result);
}

void test_negative_bbox_margin_ratio_fails() {
    const auto fixture = make_two_face_fixture();
    const auto source = make_mesh({make_triangle(
        {0.0, 0.0, 1.0},
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0})});
    spo::StlRegionExtractorOptions options;
    options.bboxMarginRatio = -0.01;

    const auto result = spo::StlRegionExtractor().extract(fixture.document, fixture.candidate, source, options);

    assert_failed_with_message(result);
}

void test_negative_min_margin_fails() {
    const auto fixture = make_two_face_fixture();
    const auto source = make_mesh({make_triangle(
        {0.0, 0.0, 1.0},
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0})});
    spo::StlRegionExtractorOptions options;
    options.minMargin = -0.1;

    const auto result = spo::StlRegionExtractor().extract(fixture.document, fixture.candidate, source, options);

    assert_failed_with_message(result);
}

void test_empty_crop_result_fails() {
    const auto fixture = make_two_face_fixture();
    const auto source = make_mesh({make_triangle(
        {0.0, 0.0, 1.0},
        {10.0, 10.0, 0.0},
        {11.0, 10.0, 0.0},
        {10.0, 11.0, 0.0})});
    spo::StlRegionExtractorOptions options;
    options.bboxMarginRatio = 0.0;
    options.minMargin = 0.0;

    const auto result = spo::StlRegionExtractor().extract(fixture.document, fixture.candidate, source, options);

    assert_failed_with_message(result);
    assert(result.report.output_triangle_count == 0);
}

void test_crop_report_fields_are_populated() {
    const auto fixture = make_two_face_fixture();
    const auto source = make_mesh({make_triangle(
        {0.0, 0.0, 1.0},
        {0.25, 0.25, 0.0},
        {0.75, 0.25, 0.0},
        {0.25, 0.75, 0.0})});
    spo::StlRegionExtractorOptions options;
    options.bboxMarginRatio = 0.5;
    options.minMargin = 0.1;

    const auto result = spo::StlRegionExtractor().extract(fixture.document, fixture.candidate, source, options);
    const auto expectedMargin = std::max(bbox_diagonal(result.report.candidate_bbox) * options.bboxMarginRatio, options.minMargin);

    assert(result.success);
    assert(result.report.success);
    assert(result.report.message.empty());
    assert(result.report.candidate_id == fixture.candidate.candidate_id);
    assert(result.report.source_triangle_count == 1);
    assert(result.report.output_triangle_count == 1);
    assert(result.report.candidate_bbox.valid);
    assert(result.report.expanded_bbox.valid);
    assert(result.report.output_bbox.valid);
    assert(std::fabs(result.report.margin - expectedMargin) <= kTolerance);
    assert(bbox_intersects(result.report.output_bbox, result.report.expanded_bbox));
}

void test_stl_region_extractor_real_clay_stp_stl_if_present() {
    const auto realStep = find_sample_path(std::filesystem::path(L"data") / L"stp" / L"03_配件_Clay.stp");
    const auto realStl = find_sample_path(std::filesystem::path(L"data") / L"stl" / L"03_配件_Clay.stl");
    if (realStep.empty() || realStl.empty()) {
        return;
    }

    const auto stepResult = spo::StepReader().read(realStep);
    assert(stepResult.status.success());
    assert(stepResult.document.hasShape());

    const auto stlResult = spo::StlReader().read(realStl);
    assert(stlResult.success);
    assert(stlResult.mesh.triangleCount() > 0);

    const auto featureEdges = spo::FeatureEdgeDetector().detect(stepResult.document.topology(), 25.0, 0.0);
    spo::MergePlannerOptions options;
    options.enable_plane_candidates = false;
    options.enable_feature_bounded_refit_candidates = true;
    options.min_feature_bounded_region_faces = 2;
    const auto plan = spo::MergePlanner().plan(stepResult.document, featureEdges, {}, options);

    const spo::RegionBoundaryAnalyzer analyzer;
    const spo::StlRegionExtractor extractor;
    bool foundValidCandidate = false;
    for (const auto& candidate : plan.candidates) {
        if (candidate.candidate_type != spo::MergeCandidateType::FeatureBoundedRefit ||
            candidate.face_count < 2 ||
            candidate.status == spo::MergeCandidateStatus::Rejected ||
            candidate.status == spo::MergeCandidateStatus::Hidden) {
            continue;
        }

        const auto analysis = analyzer.analyze(stepResult.document, candidate);
        if (!analysis.valid) {
            continue;
        }
        foundValidCandidate = true;

        const auto extractResult = extractor.extract(stepResult.document, candidate, stlResult.mesh);
        if (!extractResult.success) {
            continue;
        }
        assert(extractResult.localMesh.triangleCount() > 0);
        assert(extractResult.report.success);
        assert(extractResult.report.output_triangle_count > 0);
        assert(extractResult.report.candidate_bbox.valid);
        assert(extractResult.report.expanded_bbox.valid);
        assert(extractResult.report.output_bbox.valid);

        const auto tempPath = std::filesystem::temp_directory_path() / "local_clay_region_test.stl";
        std::filesystem::remove(tempPath);
        const auto writeResult = spo::StlWriter().write(extractResult.localMesh, tempPath);
        assert(writeResult.success);
        const auto roundTrip = spo::StlReader().read(tempPath);
        assert(roundTrip.success);
        assert(roundTrip.mesh.triangleCount() == extractResult.localMesh.triangleCount());
        std::filesystem::remove(tempPath);
        return;
    }

    if (!foundValidCandidate) {
        return;
    }
    assert(false && "No valid FeatureBoundedRefit candidate produced a local STL crop.");
}

}

void run_stl_region_extractor_tests() {
    test_extract_keeps_intersecting_triangles_and_excludes_outside();
    test_extract_excludes_triangles_inside_bbox_but_outside_candidate_boundary();
    test_margin_growth_does_not_reduce_triangle_count();
    test_empty_source_mesh_fails();
    test_empty_candidate_faces_fail();
    test_candidate_face_id_out_of_range_fails();
    test_negative_bbox_margin_ratio_fails();
    test_negative_min_margin_fails();
    test_empty_crop_result_fails();
    test_crop_report_fields_are_populated();
    test_stl_region_extractor_real_clay_stp_stl_if_present();
}
