#include "brep/ShapeDocument.h"
#include "merge/CandidateFilters.h"
#include "merge/PlaneRegionMerger.h"

#include <cassert>
#include <set>

namespace {

spo::MergeCandidate make_candidate(
    int id,
    spo::MergeCandidateType type,
    spo::MergeCandidateStatus status = spo::MergeCandidateStatus::Pending) {
    spo::MergeCandidate candidate;
    candidate.candidate_id = id;
    candidate.candidate_type = type;
    candidate.status = status;
    candidate.valid = true;
    candidate.faces = {static_cast<spo::FaceId>(id)};
    candidate.face_count = 1;
    return candidate;
}

void test_candidate_type_counts_include_zero_types() {
    const std::vector<spo::MergeCandidate> candidates {
        make_candidate(0, spo::MergeCandidateType::PlaneLike),
        make_candidate(1, spo::MergeCandidateType::SphereLike),
        make_candidate(2, spo::MergeCandidateType::CylinderLike),
        make_candidate(3, spo::MergeCandidateType::FreeformG1),
        make_candidate(4, spo::MergeCandidateType::Unknown),
    };

    const auto counts = spo::countCandidateTypes(candidates);
    assert(counts.plane_like == 1);
    assert(counts.cylinder_like == 1);
    assert(counts.sphere_like == 1);
    assert(counts.cone_like == 0);
    assert(counts.torus_like == 0);
    assert(counts.freeform_g1 == 1);
    assert(counts.freeform_g2 == 0);
    assert(counts.unknown == 1);
}

void test_candidate_filters_by_visibility_and_type() {
    const std::vector<spo::MergeCandidate> candidates {
        make_candidate(0, spo::MergeCandidateType::PlaneLike),
        make_candidate(1, spo::MergeCandidateType::PlaneLike, spo::MergeCandidateStatus::Hidden),
        make_candidate(2, spo::MergeCandidateType::SphereLike),
    };

    const auto nonHidden = spo::filterNonHiddenCandidates(candidates);
    assert(nonHidden.size() == 2);
    for (const auto& candidate : nonHidden) {
        assert(candidate.status != spo::MergeCandidateStatus::Hidden);
    }

    const auto planeOnly = spo::filterCandidatesByType(candidates, spo::MergeCandidateType::PlaneLike);
    assert(planeOnly.size() == 1);
    assert(planeOnly.front().candidate_type == spo::MergeCandidateType::PlaneLike);

    const auto sphereOnly = spo::filterCandidatesByType(candidates, spo::MergeCandidateType::SphereLike);
    assert(sphereOnly.size() == 1);
    assert(sphereOnly.front().candidate_type == spo::MergeCandidateType::SphereLike);

    assert(spo::filterCandidatesByType(candidates, spo::MergeCandidateType::ConeLike).empty());
    assert(spo::filterCandidatesByType(candidates, spo::MergeCandidateType::FreeformG1).empty());
    assert(spo::filterCandidatesByType(candidates, spo::MergeCandidateType::FreeformG2).empty());
    assert(spo::filterCandidatesByType(candidates, spo::MergeCandidateType::Unknown).empty());
}

void test_mergeable_plane_filter_only_returns_plane_candidates() {
    std::vector<spo::MergeCandidate> candidates {
        make_candidate(0, spo::MergeCandidateType::PlaneLike),
        make_candidate(1, spo::MergeCandidateType::PlaneLike, spo::MergeCandidateStatus::Rejected),
        make_candidate(2, spo::MergeCandidateType::PlaneLike, spo::MergeCandidateStatus::Hidden),
        make_candidate(3, spo::MergeCandidateType::SphereLike),
    };
    candidates.front().face_count = 2;
    candidates.front().faces = {0, 1};

    const auto planes = spo::filterMergeablePlaneCandidates(candidates);
    assert(planes.size() == 1);
    assert(planes.front().candidate_id == 0);
    assert(planes.front().candidate_type == spo::MergeCandidateType::PlaneLike);
}

void test_plane_region_merger_rejects_non_plane_candidate() {
    const spo::ShapeDocument document;
    const spo::PlaneRegionMerger merger;
    const spo::PlaneRegionMergeOptions options;
    const auto result = merger.merge(document, make_candidate(7, spo::MergeCandidateType::SphereLike), options);
    assert(!result.success);
}

}

void run_candidate_type_statistics_tests() {
    test_candidate_type_counts_include_zero_types();
    test_candidate_filters_by_visibility_and_type();
    test_mergeable_plane_filter_only_returns_plane_candidates();
    test_plane_region_merger_rejects_non_plane_candidate();
}
