#include "app/AppController.h"
#include "brep/ShapeDocument.h"
#include "io/StepWriter.h"
#include "merge/MergePlanner.h"

#include <BRepPrimAPI_MakeBox.hxx>

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
    spo::MergePlannerOptions strict;
    strict.min_region_faces = 1000;

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

}

void run_merge_planner_tests() {
    test_merge_planner_creates_plane_candidates_without_modifying_document();
    test_protected_edges_are_reported_for_region_growth_barriers();
    test_min_region_faces_filters_candidates();
    test_app_controller_preview_keeps_document_and_counts_locked_edges();
    test_merge_candidate_status_defaults_and_filters();
    test_candidate_status_changes_do_not_modify_document_stats();
}
