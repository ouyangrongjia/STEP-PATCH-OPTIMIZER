#include "merge/FeatureBoundedCandidateSelector.h"

#include <cassert>
#include <vector>

namespace {

spo::MergeCandidate make_candidate(
    const int id,
    const int faceCount,
    const int boundaryEdgeCount) {
    spo::MergeCandidate candidate;
    candidate.candidate_id = id;
    candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    candidate.status = spo::MergeCandidateStatus::Pending;
    candidate.valid = true;
    candidate.face_count = faceCount;
    candidate.boundary_edge_count = boundaryEdgeCount;
    return candidate;
}

void test_find_candidate_by_id_preserves_vector_identity() {
    const std::vector candidates{
        make_candidate(7, 2, 4),
        make_candidate(11, 3, 6),
    };

    assert(spo::findCandidateById(candidates, 11) == &candidates[1]);
    assert(spo::findCandidateById(candidates, 99) == nullptr);
}

void test_rank_candidates_is_deterministic_by_faces_boundary_then_id() {
    const std::vector candidates{
        make_candidate(9, 5, 8),
        make_candidate(4, 5, 8),
        make_candidate(6, 5, 10),
        make_candidate(3, 6, 4),
    };

    const auto ranked = spo::rankSelectableFeatureBoundedCandidates(candidates);

    assert(ranked.size() == 4);
    assert(ranked[0]->candidate_id == 3);
    assert(ranked[1]->candidate_id == 6);
    assert(ranked[2]->candidate_id == 4);
    assert(ranked[3]->candidate_id == 9);
}

void test_rank_candidates_filters_non_selectable_states() {
    auto invalid = make_candidate(1, 9, 9);
    invalid.valid = false;
    auto rejected = make_candidate(2, 8, 8);
    rejected.status = spo::MergeCandidateStatus::Rejected;
    auto hidden = make_candidate(3, 7, 7);
    hidden.status = spo::MergeCandidateStatus::Hidden;
    auto wrongType = make_candidate(4, 6, 6);
    wrongType.candidate_type = spo::MergeCandidateType::SameDomain;
    const auto selectable = make_candidate(5, 2, 4);

    const std::vector candidates{invalid, rejected, hidden, wrongType, selectable};
    const auto ranked = spo::rankSelectableFeatureBoundedCandidates(candidates);

    assert(ranked.size() == 1);
    assert(ranked.front()->candidate_id == selectable.candidate_id);
}

void test_rank_candidates_uses_geometry_vectors_when_counts_are_unset() {
    auto larger = make_candidate(1, 0, 0);
    larger.faces = {1, 2, 3};
    larger.boundary_edges = {1, 2, 3, 4};
    auto smaller = make_candidate(2, 0, 0);
    smaller.faces = {1, 2};
    smaller.boundary_edges = {1, 2, 3, 4, 5};

    const std::vector candidates{smaller, larger};
    const auto ranked = spo::rankSelectableFeatureBoundedCandidates(candidates);

    assert(ranked.size() == 2);
    assert(ranked.front()->candidate_id == larger.candidate_id);
}

}

void run_feature_bounded_candidate_selector_tests() {
    test_find_candidate_by_id_preserves_vector_identity();
    test_rank_candidates_is_deterministic_by_faces_boundary_then_id();
    test_rank_candidates_filters_non_selectable_states();
    test_rank_candidates_uses_geometry_vectors_when_counts_are_unset();
}
