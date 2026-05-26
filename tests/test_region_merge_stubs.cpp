#include "brep/ShapeDocument.h"
#include "merge/ConeRegionMerger.h"
#include "merge/CylinderRegionMerger.h"
#include "merge/PlaneRegionMerger.h"
#include "merge/TorusRegionMerger.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <cassert>

namespace {

spo::ShapeDocument create_box_document() {
    return spo::ShapeDocument(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(), {});
}

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
}

spo::MergeCandidate make_candidate(spo::MergeCandidateType type) {
    spo::MergeCandidate candidate;
    candidate.candidate_id = 42;
    candidate.candidate_type = type;
    candidate.status = spo::MergeCandidateStatus::Accepted;
    candidate.faces = {0, 1};
    candidate.face_count = 2;
    return candidate;
}

void assert_not_implemented_without_document_change(
    const spo::ShapeDocument& document,
    const spo::RegionMergeResult& result,
    spo::MergeCandidateType expectedType) {
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::NotImplemented);
    assert(result.candidate_id == 42);
    assert(result.candidate_type == expectedType);
    assert(result.face_count_before == document.stats().faces);
    assert(result.face_count_after == document.stats().faces);
    assert(result.edge_count_before == document.stats().edges);
    assert(result.edge_count_after == document.stats().edges);
}

void test_region_merger_stubs_return_not_implemented_for_matching_types() {
    const auto document = create_box_document();
    const auto before = document.stats();
    const spo::RegionMergeOptions options;

    const spo::CylinderRegionMerger cylinder;
    assert_not_implemented_without_document_change(
        document,
        cylinder.merge(document, make_candidate(spo::MergeCandidateType::CylinderLike), options),
        spo::MergeCandidateType::CylinderLike);

    const spo::ConeRegionMerger cone;
    assert_not_implemented_without_document_change(
        document,
        cone.merge(document, make_candidate(spo::MergeCandidateType::ConeLike), options),
        spo::MergeCandidateType::ConeLike);

    const spo::TorusRegionMerger torus;
    assert_not_implemented_without_document_change(
        document,
        torus.merge(document, make_candidate(spo::MergeCandidateType::TorusLike), options),
        spo::MergeCandidateType::TorusLike);

    assert(same_stats(document.stats(), before));
}

void test_region_merger_stub_rejects_wrong_candidate_type() {
    const auto document = create_box_document();
    const auto before = document.stats();
    const spo::RegionMergeOptions options;
    const spo::CylinderRegionMerger cylinder;

    const auto result = cylinder.merge(document, make_candidate(spo::MergeCandidateType::PlaneLike), options);
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::UnsupportedCandidateType);
    assert(result.candidate_type == spo::MergeCandidateType::PlaneLike);
    assert(same_stats(document.stats(), before));
}

void test_region_merger_stub_rejects_hidden_and_rejected_candidates() {
    const auto document = create_box_document();
    const spo::RegionMergeOptions options;
    const spo::CylinderRegionMerger cylinder;

    auto rejected = make_candidate(spo::MergeCandidateType::CylinderLike);
    rejected.status = spo::MergeCandidateStatus::Rejected;
    const auto rejectedResult = cylinder.merge(document, rejected, options);
    assert(!rejectedResult.success);
    assert(rejectedResult.failure_reason == spo::RegionMergeFailureReason::RejectedCandidate);

    auto hidden = make_candidate(spo::MergeCandidateType::CylinderLike);
    hidden.status = spo::MergeCandidateStatus::Hidden;
    const auto hiddenResult = cylinder.merge(document, hidden, options);
    assert(!hiddenResult.success);
    assert(hiddenResult.failure_reason == spo::RegionMergeFailureReason::HiddenCandidate);
}

}

void run_region_merge_stub_tests() {
    test_region_merger_stubs_return_not_implemented_for_matching_types();
    test_region_merger_stub_rejects_wrong_candidate_type();
    test_region_merger_stub_rejects_hidden_and_rejected_candidates();
}
