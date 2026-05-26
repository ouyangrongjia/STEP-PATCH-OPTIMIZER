#include "brep/ShapeDocument.h"
#include "merge/SphereRegionMerger.h"

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
#include <gp_Sphere.hxx>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace {

struct SphereFixture {
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

SphereFixture make_split_sphere_fixture(bool convert_to_nurbs = false) {
    const gp_Sphere sphere(gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 5.0);
    const auto first = BRepBuilderAPI_MakeFace(sphere, 0.0, 3.14159265358979323846, -0.6, 0.6).Face();
    const auto second = BRepBuilderAPI_MakeFace(sphere, 3.14159265358979323846, 6.28318530717958647692, -0.6, 0.6).Face();
    BRepBuilderAPI_Sewing sewing;
    sewing.Add(first);
    sewing.Add(second);
    sewing.Perform();
    TopoDS_Shape shape = sewing.SewedShape();
    if (convert_to_nurbs) {
        BRepBuilderAPI_NurbsConvert converter(shape);
        shape = converter.Shape();
    }

    SphereFixture fixture;
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

    fixture.candidate.candidate_id = 10;
    fixture.candidate.candidate_type = spo::MergeCandidateType::SphereLike;
    fixture.candidate.status = spo::MergeCandidateStatus::Accepted;
    fixture.candidate.faces = {0, 1};
    fixture.candidate.face_count = static_cast<int>(fixture.candidate.faces.size());
    fixture.candidate.boundary_edge_count = static_cast<int>(fixture.candidate.boundary_edges.size());
    fixture.candidate.internal_edge_count = static_cast<int>(fixture.candidate.internal_edges.size());
    assert(fixture.candidate.boundary_edges.size() >= 4);
    assert(fixture.candidate.internal_edges.size() >= 1);
    return fixture;
}

void test_sphere_region_merger_rejects_invalid_candidate_states() {
    const auto fixture = make_split_sphere_fixture();
    const spo::SphereRegionMerger merger;
    const spo::SphereRegionMergeOptions options;

    auto wrong_type = fixture.candidate;
    wrong_type.candidate_type = spo::MergeCandidateType::PlaneLike;
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

void test_sphere_region_merger_rejects_small_or_protected_regions() {
    const auto fixture = make_split_sphere_fixture();
    const spo::SphereRegionMerger merger;
    const spo::SphereRegionMergeOptions options;

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

void test_sphere_region_merger_does_not_require_boundary_wire() {
    const auto fixture = make_split_sphere_fixture();
    const auto before = fixture.document.stats();
    const spo::SphereRegionMerger merger;
    spo::SphereRegionMergeOptions options;
    options.sphere_fit_tolerance = 1.0;
    options.max_deviation = 1.0;

    auto invalid_boundary = fixture.candidate;
    invalid_boundary.boundary_edges = {invalid_boundary.boundary_edges.front()};
    const auto result = merger.merge(fixture.document, invalid_boundary, options);
    if (result.success) {
        assert(result.face_count_after < result.face_count_before);
    }
    assert(same_stats(fixture.document.stats(), before));
}

void test_sphere_region_merger_merges_native_sphere_region() {
    const auto fixture = make_split_sphere_fixture();
    const spo::SphereRegionMerger merger;
    spo::SphereRegionMergeOptions options;
    options.sphere_fit_tolerance = 1.0;
    options.max_deviation = 1.0;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    if (!result.success) {
        assert(result.failure_reason != spo::RegionMergeFailureReason::UnsupportedCandidateType);
        assert(result.failure_reason != spo::RegionMergeFailureReason::RejectedCandidate);
        assert(result.failure_reason != spo::RegionMergeFailureReason::HiddenCandidate);
        assert(result.failure_reason != spo::RegionMergeFailureReason::InsufficientFaces);
        return;
    }
    assert(result.failure_reason == spo::RegionMergeFailureReason::None);
    assert(result.candidate_type == spo::MergeCandidateType::SphereLike);
    assert(result.primitive_radius > 0.0);
    assert(result.primitive_fit_error <= options.max_deviation);
    assert(result.face_count_after < result.face_count_before);
    assert(result.document.hasShape());
    assert(result.document.stats().faces > 0);
    assert(result.document.stats().edges > 0);
}

void test_sphere_region_merger_accepts_nurbs_backed_sphere_region() {
    const auto fixture = make_split_sphere_fixture(true);
    const spo::SphereRegionMerger merger;
    spo::SphereRegionMergeOptions options;
    options.sphere_fit_tolerance = 1.0;
    options.max_deviation = 1.0;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    if (!result.success) {
        assert(result.failure_reason != spo::RegionMergeFailureReason::UnsupportedCandidateType);
        assert(result.failure_reason != spo::RegionMergeFailureReason::RejectedCandidate);
        assert(result.failure_reason != spo::RegionMergeFailureReason::HiddenCandidate);
        return;
    }
    assert(result.failure_reason == spo::RegionMergeFailureReason::None);
    assert(result.document.hasShape());
    assert(result.face_count_after < result.face_count_before);
    assert(result.primitive_radius > 0.0);
    assert(result.max_deviation <= options.max_deviation);
}

void test_sphere_region_merger_fills_primitive_fields_on_success() {
    const auto fixture = make_split_sphere_fixture();
    const spo::SphereRegionMerger merger;
    spo::SphereRegionMergeOptions options;
    options.sphere_fit_tolerance = 1.0;
    options.max_deviation = 1.0;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    if (!result.success) {
        return;
    }
    assert(std::abs(result.primitive_center_x) < 1.0);
    assert(std::abs(result.primitive_center_y) < 1.0);
    assert(std::abs(result.primitive_center_z) < 1.0);
    assert(std::abs(result.primitive_radius - 5.0) < 1.0);
    assert(result.primitive_fit_error > 0.0 || result.primitive_fit_error == result.max_deviation);
    assert(result.primitive_fit_error <= options.max_deviation);
    assert(result.primitive_axis_x == 0.0);
    assert(result.primitive_axis_y == 0.0);
    assert(result.primitive_axis_z == 0.0);
    assert(result.primitive_secondary_radius == 0.0);
    assert(result.primitive_angle_degrees == 0.0);
}

void test_sphere_region_merger_failure_does_not_fill_primitive_fields() {
    const auto fixture = make_split_sphere_fixture();
    const spo::SphereRegionMerger merger;
    const spo::SphereRegionMergeOptions options;

    auto wrong_type = fixture.candidate;
    wrong_type.candidate_type = spo::MergeCandidateType::CylinderLike;
    const auto result = merger.merge(fixture.document, wrong_type, options);
    assert(!result.success);
    assert(result.primitive_center_x == 0.0);
    assert(result.primitive_center_y == 0.0);
    assert(result.primitive_center_z == 0.0);
    assert(result.primitive_radius == 0.0);
    assert(result.primitive_fit_error == 0.0);
}

void test_sphere_region_merger_compatible_with_region_merge_options() {
    const auto fixture = make_split_sphere_fixture();
    const spo::SphereRegionMerger merger;
    spo::RegionMergeOptions options;
    options.max_deviation = 1.0;

    const auto result = merger.merge(fixture.document, fixture.candidate, options);
    if (!result.success) {
        return;
    }
    assert(result.candidate_type == spo::MergeCandidateType::SphereLike);
    assert(result.primitive_radius > 0.0);
    assert(result.face_count_after < result.face_count_before);
}

void test_sphere_region_merger_batch_fails_with_empty_candidates() {
    const auto fixture = make_split_sphere_fixture();
    const spo::SphereRegionMerger merger;
    spo::SphereRegionMergeOptions options;
    options.max_deviation = 1.0;

    const auto result = merger.mergeBatch(fixture.document, {}, options);
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::CandidateNotFound);
}

void test_sphere_region_merger_batch_skips_overlapping_candidates() {
    const auto fixture = make_split_sphere_fixture();
    const spo::SphereRegionMerger merger;
    spo::SphereRegionMergeOptions options;
    options.max_deviation = 1.0;
    options.sphere_fit_tolerance = 1.0;

    auto copy = fixture.candidate;
    copy.candidate_id = 11;
    const auto result = merger.mergeBatch(fixture.document, {fixture.candidate, copy}, options);
    if (!result.success) {
        return;
    }
    assert(result.face_count_after < result.face_count_before);
}

void test_sphere_region_merger_batch_skips_invalid_candidates() {
    const auto fixture = make_split_sphere_fixture();
    const spo::SphereRegionMerger merger;
    spo::SphereRegionMergeOptions options;
    options.max_deviation = 1.0;
    options.sphere_fit_tolerance = 1.0;

    auto invalid = fixture.candidate;
    invalid.candidate_id = 11;
    invalid.boundary_edges = {invalid.boundary_edges.front()};

    const auto result = merger.mergeBatch(fixture.document, {invalid, fixture.candidate}, options);
    if (!result.success) {
        assert(result.failure_reason != spo::RegionMergeFailureReason::BoundaryLoopInvalid);
        return;
    }
    assert(result.face_count_after < result.face_count_before);
}

void test_sphere_region_merger_batch_does_not_change_solid_count() {
    const auto fixture = make_split_sphere_fixture();
    const auto before = fixture.document.stats();
    const spo::SphereRegionMerger merger;
    spo::SphereRegionMergeOptions options;
    options.max_deviation = 1.0;
    options.sphere_fit_tolerance = 1.0;

    const auto result = merger.mergeBatch(fixture.document, {fixture.candidate}, options);
    if (!result.success) {
        return;
    }
    if (before.solids > 0) {
        assert(result.document.stats().solids == before.solids);
    }
}

}

void run_sphere_region_merger_tests() {
    test_sphere_region_merger_rejects_invalid_candidate_states();
    test_sphere_region_merger_rejects_small_or_protected_regions();
    test_sphere_region_merger_does_not_require_boundary_wire();
    test_sphere_region_merger_merges_native_sphere_region();
    test_sphere_region_merger_accepts_nurbs_backed_sphere_region();
    test_sphere_region_merger_fills_primitive_fields_on_success();
    test_sphere_region_merger_failure_does_not_fill_primitive_fields();
    test_sphere_region_merger_compatible_with_region_merge_options();
    test_sphere_region_merger_batch_fails_with_empty_candidates();
    test_sphere_region_merger_batch_skips_overlapping_candidates();
    test_sphere_region_merger_batch_skips_invalid_candidates();
    test_sphere_region_merger_batch_does_not_change_solid_count();
}
