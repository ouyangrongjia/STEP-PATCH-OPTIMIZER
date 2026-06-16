#include "merge/CandidateFilters.h"

#include <cassert>
#include <set>
#include <string>

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
        make_candidate(0, spo::MergeCandidateType::FeatureBoundedRefit),
        make_candidate(1, spo::MergeCandidateType::Unknown),
        make_candidate(2, spo::MergeCandidateType::SameDomain),
    };

    const auto counts = spo::countCandidateTypes(candidates);
    assert(counts.feature_bounded_refit == 1);
    assert(counts.unknown == 2);
}

void test_candidate_filters_by_visibility_and_type() {
    const std::vector<spo::MergeCandidate> candidates {
        make_candidate(0, spo::MergeCandidateType::FeatureBoundedRefit),
        make_candidate(1, spo::MergeCandidateType::FeatureBoundedRefit, spo::MergeCandidateStatus::Hidden),
        make_candidate(2, spo::MergeCandidateType::Unknown),
        make_candidate(3, spo::MergeCandidateType::SameDomain),
    };

    const auto nonHidden = spo::filterNonHiddenCandidates(candidates);
    assert(nonHidden.size() == 3);
    for (const auto& candidate : nonHidden) {
        assert(candidate.status != spo::MergeCandidateStatus::Hidden);
    }

    const auto featureBoundedOnly = spo::filterCandidatesByType(candidates, spo::MergeCandidateType::FeatureBoundedRefit);
    assert(featureBoundedOnly.size() == 1);
    assert(featureBoundedOnly.front().candidate_type == spo::MergeCandidateType::FeatureBoundedRefit);

    const auto unknownOnly = spo::filterCandidatesByType(candidates, spo::MergeCandidateType::Unknown);
    assert(unknownOnly.size() == 1);
    assert(unknownOnly.front().candidate_type == spo::MergeCandidateType::Unknown);

    const auto hiddenFeatureBounded = spo::filterCandidatesByType(
        candidates,
        spo::MergeCandidateType::FeatureBoundedRefit,
        true);
    assert(hiddenFeatureBounded.size() == 2);
}

void test_candidate_type_string_includes_feature_bounded_refit() {
    assert(std::string(spo::toString(spo::MergeCandidateType::FeatureBoundedRefit)) == "FeatureBoundedRefit");
}

}

void run_candidate_type_statistics_tests() {
    test_candidate_type_counts_include_zero_types();
    test_candidate_filters_by_visibility_and_type();
    test_candidate_type_string_includes_feature_bounded_refit();
}
