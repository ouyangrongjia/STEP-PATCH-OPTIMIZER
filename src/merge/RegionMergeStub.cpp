#include "merge/RegionMergeStub.h"

#include <string>

namespace spo {

namespace {

RegionMergeResult baseResult(const ShapeDocument& document, const MergeCandidate& candidate) {
    RegionMergeResult result;
    result.candidate_id = candidate.candidate_id;
    result.candidate_type = candidate.candidate_type;
    const auto& stats = document.stats();
    result.face_count_before = stats.faces;
    result.face_count_after = stats.faces;
    result.edge_count_before = stats.edges;
    result.edge_count_after = stats.edges;
    return result;
}

}

RegionMergeResult makeRegionMergeStubResult(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionMergeOptions& options,
    MergeCandidateType expectedType,
    const char* mergerName) {
    auto result = baseResult(document, candidate);

    if (candidate.candidate_type != expectedType) {
        result.failure_reason = RegionMergeFailureReason::UnsupportedCandidateType;
        result.message = std::string(mergerName) + " does not support this candidate type.";
        return result;
    }
    if (!candidate.valid) {
        result.failure_reason = RegionMergeFailureReason::InvalidCandidate;
        result.message = std::string(mergerName) + " received an invalid candidate.";
        return result;
    }
    if (candidate.status == MergeCandidateStatus::Rejected) {
        result.failure_reason = RegionMergeFailureReason::RejectedCandidate;
        result.message = std::string(mergerName) + " will not merge a rejected candidate.";
        return result;
    }
    if (candidate.status == MergeCandidateStatus::Hidden) {
        result.failure_reason = RegionMergeFailureReason::HiddenCandidate;
        result.message = std::string(mergerName) + " will not merge a hidden candidate.";
        return result;
    }
    if (candidate.status == MergeCandidateStatus::Pending &&
        options.require_accepted_candidate &&
        !options.allow_pending_candidate) {
        result.failure_reason = RegionMergeFailureReason::NotSupported;
        result.message = std::string(mergerName) + " requires an accepted candidate in Stage 3.";
        return result;
    }
    if (candidate.face_count < options.min_region_faces) {
        result.failure_reason = RegionMergeFailureReason::InsufficientFaces;
        result.message = std::string(mergerName) + " requires more faces in the candidate region.";
        return result;
    }

    result.failure_reason = RegionMergeFailureReason::NotImplemented;
    result.message = std::string(mergerName) + " is a Stage 3-0 stub; real B-rep region merge is not implemented.";
    return result;
}

}
