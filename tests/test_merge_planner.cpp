#include "app/AppController.h"
#include "brep/ShapeDocument.h"
#include "io/StepWriter.h"
#include "merge/MergePlanner.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Sphere.hxx>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

spo::ShapeDocument create_box_document() {
    return spo::ShapeDocument(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(), {});
}

struct TwoFaceFixture {
    spo::ShapeDocument document;
    spo::EdgeId shared_edge = 0;
};

TwoFaceFixture create_two_face_document() {
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
    for (spo::EdgeId edge = 0; edge < fixture.document.topology().edgeCount(); ++edge) {
        const auto* adjacency = fixture.document.topology().adjacencyForEdge(edge);
        assert(adjacency != nullptr);
        if (adjacency->faces.size() == 2) {
            fixture.shared_edge = edge;
            break;
        }
    }
    return fixture;
}

spo::ShapeDocument create_split_cylinder_document() {
    const gp_Cylinder cylinder(gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 5.0);
    const auto first = BRepBuilderAPI_MakeFace(cylinder, 0.0, 3.14159265358979323846, 0.0, 10.0).Face();
    const auto second = BRepBuilderAPI_MakeFace(cylinder, 3.14159265358979323846, 6.28318530717958647692, 0.0, 10.0).Face();
    BRepBuilderAPI_Sewing sewing;
    sewing.Add(first);
    sewing.Add(second);
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

spo::ShapeDocument create_split_sphere_document() {
    const gp_Sphere sphere(gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 5.0);
    const auto first = BRepBuilderAPI_MakeFace(sphere, 0.0, 3.14159265358979323846, -0.6, 0.6).Face();
    const auto second = BRepBuilderAPI_MakeFace(sphere, 3.14159265358979323846, 6.28318530717958647692, -0.6, 0.6).Face();
    BRepBuilderAPI_Sewing sewing;
    sewing.Add(first);
    sewing.Add(second);
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

std::filesystem::path temp_step_path() {
    return std::filesystem::temp_directory_path() /
        ("step-patch-optimizer-merge-planner-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".stp");
}

std::filesystem::path create_test_step() {
    const auto path = temp_step_path();
    const spo::StepWriter writer;
    assert(writer.write(create_box_document(), path).success());
    return path;
}

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
}

std::vector<spo::MergeCandidate> candidates_of_type(
    const std::vector<spo::MergeCandidate>& candidates,
    spo::MergeCandidateType type) {
    std::vector<spo::MergeCandidate> filtered;
    for (const auto& candidate : candidates) {
        if (candidate.candidate_type == type) {
            filtered.push_back(candidate);
        }
    }
    return filtered;
}

bool contains_edge(const std::vector<spo::EdgeId>& edges, spo::EdgeId edge) {
    return std::find(edges.begin(), edges.end(), edge) != edges.end();
}

spo::MergeCandidate* find_candidate(std::vector<spo::MergeCandidate>& candidates, int candidateId) {
    for (auto& candidate : candidates) {
        if (candidate.candidate_id == candidateId) {
            return &candidate;
        }
    }
    return nullptr;
}

std::vector<spo::MergeCandidate> candidates_with_status(
    const std::vector<spo::MergeCandidate>& candidates,
    spo::MergeCandidateStatus status) {
    std::vector<spo::MergeCandidate> filtered;
    for (const auto& candidate : candidates) {
        if (candidate.status == status) {
            filtered.push_back(candidate);
        }
    }
    return filtered;
}

std::vector<spo::MergeCandidate> non_hidden_candidates(const std::vector<spo::MergeCandidate>& candidates) {
    std::vector<spo::MergeCandidate> filtered;
    for (const auto& candidate : candidates) {
        if (candidate.status != spo::MergeCandidateStatus::Hidden) {
            filtered.push_back(candidate);
        }
    }
    return filtered;
}

void test_merge_planner_creates_plane_candidates_without_modifying_document() {
    const auto document = create_box_document();
    const auto before = document.stats();

    spo::MergePlannerOptions options;
    options.min_region_faces = 1;
    options.enable_feature_bounded_refit_candidates = false;

    const spo::MergePlanner planner;
    const spo::FeatureEdgeDetectionResult featureEdges;
    const auto result = planner.plan(document, featureEdges, {}, options);

    assert(same_stats(document.stats(), before));
    assert(result.visited_faces == before.faces);
    assert(!result.candidates.empty());
    for (const auto& candidate : result.candidates) {
        assert(candidate.candidate_type == spo::MergeCandidateType::PlaneLike);
        assert(candidate.face_count == static_cast<int>(candidate.faces.size()));
        assert(candidate.total_area >= 0.0);
        assert(candidate.valid);
    }
}

void test_protected_edges_are_reported_for_region_growth_barriers() {
    const auto document = create_box_document();
    assert(document.topology().edgeCount() > 0);

    spo::MergePlannerOptions options;
    options.min_region_faces = 1;
    options.enable_feature_bounded_refit_candidates = false;

    const spo::MergePlanner planner;
    const spo::FeatureEdgeDetectionResult featureEdges;
    const auto result = planner.plan(document, featureEdges, std::set<spo::EdgeId>{0}, options);

    assert(result.protected_edge_count == 1);
    assert(same_stats(document.stats(), create_box_document().stats()));
}

void test_min_region_faces_filters_candidates() {
    const auto document = create_box_document();

    spo::MergePlannerOptions permissive;
    permissive.min_region_faces = 1;
    permissive.enable_feature_bounded_refit_candidates = false;
    spo::MergePlannerOptions strict;
    strict.min_region_faces = 1000;
    strict.enable_feature_bounded_refit_candidates = false;

    const spo::MergePlanner planner;
    const spo::FeatureEdgeDetectionResult featureEdges;
    const auto permissiveResult = planner.plan(document, featureEdges, {}, permissive);
    const auto strictResult = planner.plan(document, featureEdges, {}, strict);

    assert(permissiveResult.candidates.size() >= strictResult.candidates.size());
    assert(strictResult.candidates.empty());
    assert(strictResult.rejected_regions > 0);
}

void test_app_controller_preview_keeps_document_and_counts_locked_edges() {
    const auto path = create_test_step();
    spo::AppController controller;
    assert(controller.openStepFile(path).success());
    assert(controller.lockEdges({0}).success());

    const auto before = controller.document().stats();
    spo::MergePlannerOptions options;
    options.min_region_faces = 1;
    options.enable_feature_bounded_refit_candidates = false;

    const auto result = controller.previewMergeCandidates(180.0, 0.0, options);
    assert(result.protected_edge_count >= 1);
    assert(!result.candidates.empty());
    assert(same_stats(controller.document().stats(), before));

    std::filesystem::remove(path);
}

void test_merge_candidate_status_defaults_and_filters() {
    spo::MergeCandidate pending;
    pending.candidate_id = 1;
    assert(pending.status == spo::MergeCandidateStatus::Pending);
    assert(std::string(spo::toString(pending.status)) == "Pending");

    spo::MergeCandidate accepted;
    accepted.candidate_id = 2;
    accepted.status = spo::MergeCandidateStatus::Accepted;
    spo::MergeCandidate rejected;
    rejected.candidate_id = 3;
    rejected.status = spo::MergeCandidateStatus::Rejected;
    spo::MergeCandidate hidden;
    hidden.candidate_id = 4;
    hidden.status = spo::MergeCandidateStatus::Hidden;

    std::vector<spo::MergeCandidate> candidates{pending, accepted, rejected, hidden};
    assert(candidates_with_status(candidates, spo::MergeCandidateStatus::Accepted).size() == 1);
    assert(candidates_with_status(candidates, spo::MergeCandidateStatus::Pending).size() == 1);
    assert(non_hidden_candidates(candidates).size() == 3);
    assert(find_candidate(candidates, 999) == nullptr);
}

void test_candidate_status_changes_do_not_modify_document_stats() {
    const auto document = create_box_document();
    const auto before = document.stats();

    spo::MergePlannerOptions options;
    options.min_region_faces = 1;
    options.enable_feature_bounded_refit_candidates = false;

    const spo::MergePlanner planner;
    const spo::FeatureEdgeDetectionResult featureEdges;
    auto result = planner.plan(document, featureEdges, {}, options);
    assert(!result.candidates.empty());

    auto* candidate = find_candidate(result.candidates, result.candidates.front().candidate_id);
    assert(candidate != nullptr);
    candidate->status = spo::MergeCandidateStatus::Accepted;
    assert(candidate->status == spo::MergeCandidateStatus::Accepted);
    candidate->status = spo::MergeCandidateStatus::Rejected;
    assert(candidate->status == spo::MergeCandidateStatus::Rejected);
    candidate->status = spo::MergeCandidateStatus::Hidden;
    assert(candidate->status == spo::MergeCandidateStatus::Hidden);

    assert(same_stats(document.stats(), before));
}

void test_feature_bounded_candidate_status_changes_do_not_modify_document_stats() {
    const auto fixture = create_two_face_document();
    const auto before = fixture.document.stats();

    spo::MergePlannerOptions options;
    options.enable_plane_candidates = false;
    options.min_feature_bounded_region_faces = 2;

    const spo::MergePlanner planner;
    auto result = planner.plan(fixture.document, {}, {}, options);
    auto* candidate = find_candidate(result.candidates, 0);
    assert(candidate != nullptr);
    assert(candidate->candidate_type == spo::MergeCandidateType::FeatureBoundedRefit);

    candidate->status = spo::MergeCandidateStatus::Accepted;
    assert(same_stats(fixture.document.stats(), before));
    candidate->status = spo::MergeCandidateStatus::Rejected;
    assert(same_stats(fixture.document.stats(), before));
    candidate->status = spo::MergeCandidateStatus::Hidden;
    assert(same_stats(fixture.document.stats(), before));
}

void test_feature_bounded_refit_enable_flag_controls_generation() {
    const auto fixture = create_two_face_document();
    const spo::MergePlanner planner;
    const spo::FeatureEdgeDetectionResult featureEdges;

    spo::MergePlannerOptions disabled;
    disabled.enable_plane_candidates = false;
    disabled.enable_feature_bounded_refit_candidates = false;
    const auto disabledResult = planner.plan(fixture.document, featureEdges, {}, disabled);
    assert(candidates_of_type(disabledResult.candidates, spo::MergeCandidateType::FeatureBoundedRefit).empty());

    spo::MergePlannerOptions enabled;
    enabled.enable_plane_candidates = false;
    enabled.enable_feature_bounded_refit_candidates = true;
    enabled.min_feature_bounded_region_faces = 2;
    const auto enabledResult = planner.plan(fixture.document, featureEdges, {}, enabled);
    const auto featureBounded = candidates_of_type(enabledResult.candidates, spo::MergeCandidateType::FeatureBoundedRefit);
    assert(featureBounded.size() == 1);
    assert(featureBounded.front().face_count == 2);
}

void test_min_feature_bounded_region_faces_filters_candidates() {
    const auto fixture = create_two_face_document();
    const spo::MergePlanner planner;
    const spo::FeatureEdgeDetectionResult featureEdges;

    spo::MergePlannerOptions permissive;
    permissive.enable_plane_candidates = false;
    permissive.min_feature_bounded_region_faces = 2;
    const auto permissiveResult = planner.plan(fixture.document, featureEdges, {}, permissive);
    assert(candidates_of_type(permissiveResult.candidates, spo::MergeCandidateType::FeatureBoundedRefit).size() == 1);

    spo::MergePlannerOptions strict;
    strict.enable_plane_candidates = false;
    strict.min_feature_bounded_region_faces = 3;
    const auto strictResult = planner.plan(fixture.document, featureEdges, {}, strict);
    assert(candidates_of_type(strictResult.candidates, spo::MergeCandidateType::FeatureBoundedRefit).empty());
}

void test_locked_edges_and_feature_edges_bound_feature_bounded_regions() {
    const auto fixture = create_two_face_document();
    const spo::MergePlanner planner;

    spo::MergePlannerOptions options;
    options.enable_plane_candidates = false;
    options.min_feature_bounded_region_faces = 1;

    const auto lockedResult = planner.plan(fixture.document, {}, {fixture.shared_edge}, options);
    const auto lockedCandidates = candidates_of_type(lockedResult.candidates, spo::MergeCandidateType::FeatureBoundedRefit);
    assert(lockedCandidates.size() == 2);
    for (const auto& candidate : lockedCandidates) {
        assert(candidate.face_count == 1);
        assert(candidate.internal_edges.empty());
        assert(contains_edge(candidate.boundary_edges, fixture.shared_edge));
        assert(contains_edge(candidate.protected_edges, fixture.shared_edge));
    }

    spo::FeatureEdgeDetectionResult featureEdges;
    featureEdges.edges.push_back(spo::FeatureEdge {fixture.shared_edge, spo::FeatureEdgeKind::Sharp, 90.0});
    featureEdges.sharp_edges = 1;
    const auto featureResult = planner.plan(fixture.document, featureEdges, {}, options);
    const auto featureCandidates = candidates_of_type(featureResult.candidates, spo::MergeCandidateType::FeatureBoundedRefit);
    assert(featureCandidates.size() == 2);
    for (const auto& candidate : featureCandidates) {
        assert(candidate.internal_edges.empty());
        assert(contains_edge(candidate.boundary_edges, fixture.shared_edge));
        assert(contains_edge(candidate.protected_edges, fixture.shared_edge));
    }
}

void test_feature_bounded_refit_does_not_replace_existing_candidate_entries() {
    const auto planeDocument = create_box_document();
    const auto cylinderDocument = create_split_cylinder_document();
    const auto sphereDocument = create_split_sphere_document();
    const spo::MergePlanner planner;

    spo::MergePlannerOptions planeOptions;
    planeOptions.min_region_faces = 1;
    const auto planeResult = planner.plan(planeDocument, {}, {}, planeOptions);
    assert(!candidates_of_type(planeResult.candidates, spo::MergeCandidateType::PlaneLike).empty());
    assert(!candidates_of_type(planeResult.candidates, spo::MergeCandidateType::FeatureBoundedRefit).empty());

    spo::MergePlannerOptions cylinderOptions;
    cylinderOptions.enable_plane_candidates = false;
    cylinderOptions.enable_cylinder_candidates = true;
    const auto cylinderResult = planner.plan(cylinderDocument, {}, {}, cylinderOptions);
    assert(!candidates_of_type(cylinderResult.candidates, spo::MergeCandidateType::CylinderLike).empty());
    assert(!candidates_of_type(cylinderResult.candidates, spo::MergeCandidateType::FeatureBoundedRefit).empty());

    spo::MergePlannerOptions sphereOptions;
    sphereOptions.enable_plane_candidates = false;
    sphereOptions.enable_sphere_candidates = true;
    const auto sphereResult = planner.plan(sphereDocument, {}, {}, sphereOptions);
    assert(!candidates_of_type(sphereResult.candidates, spo::MergeCandidateType::SphereLike).empty());
    assert(!candidates_of_type(sphereResult.candidates, spo::MergeCandidateType::FeatureBoundedRefit).empty());
}

}

void run_merge_planner_tests() {
    test_merge_planner_creates_plane_candidates_without_modifying_document();
    test_protected_edges_are_reported_for_region_growth_barriers();
    test_min_region_faces_filters_candidates();
    test_app_controller_preview_keeps_document_and_counts_locked_edges();
    test_merge_candidate_status_defaults_and_filters();
    test_candidate_status_changes_do_not_modify_document_stats();
    test_feature_bounded_candidate_status_changes_do_not_modify_document_stats();
    test_feature_bounded_refit_enable_flag_controls_generation();
    test_min_feature_bounded_region_faces_filters_candidates();
    test_locked_edges_and_feature_edges_bound_feature_bounded_regions();
    test_feature_bounded_refit_does_not_replace_existing_candidate_entries();
}
