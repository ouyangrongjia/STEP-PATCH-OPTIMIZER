#include "merge/FeatureBoundedCandidateSelector.h"

#include <algorithm>
#include <utility>

namespace spo {

namespace {

int candidate_face_count(const MergeCandidate& candidate) {
    if (candidate.face_count > 0) {
        return candidate.face_count;
    }
    return static_cast<int>(candidate.faces.size());
}

int candidate_boundary_edge_count(const MergeCandidate& candidate) {
    if (candidate.boundary_edge_count > 0) {
        return candidate.boundary_edge_count;
    }
    return static_cast<int>(candidate.boundary_edges.size());
}

}

const MergeCandidate* findCandidateById(
    const std::vector<MergeCandidate>& candidates,
    const int candidateId) {
    const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
        return candidate.candidate_id == candidateId;
    });
    return found == candidates.end() ? nullptr : &*found;
}

std::vector<const MergeCandidate*> rankSelectableFeatureBoundedCandidates(
    const std::vector<MergeCandidate>& candidates) {
    std::vector<const MergeCandidate*> ranked;
    ranked.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (candidate.valid &&
            candidate.candidate_type == MergeCandidateType::FeatureBoundedRefit &&
            candidate.status != MergeCandidateStatus::Rejected &&
            candidate.status != MergeCandidateStatus::Hidden) {
            ranked.push_back(&candidate);
        }
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](const auto* lhs, const auto* rhs) {
        const auto lhsFaces = candidate_face_count(*lhs);
        const auto rhsFaces = candidate_face_count(*rhs);
        if (lhsFaces != rhsFaces) {
            return lhsFaces > rhsFaces;
        }
        const auto lhsBoundary = candidate_boundary_edge_count(*lhs);
        const auto rhsBoundary = candidate_boundary_edge_count(*rhs);
        if (lhsBoundary != rhsBoundary) {
            return lhsBoundary > rhsBoundary;
        }
        return lhs->candidate_id < rhs->candidate_id;
    });
    return ranked;
}

FeatureBoundedCandidateSelection selectLargestValidFeatureBoundedCandidate(
    const ShapeDocument& document,
    const std::vector<MergeCandidate>& candidates) {
    FeatureBoundedCandidateSelection selection;
    for (const auto* candidate : rankSelectableFeatureBoundedCandidates(candidates)) {
        auto boundary = RegionBoundaryAnalyzer().analyze(document, *candidate);
        if (boundary.valid) {
            selection.candidate = candidate;
            selection.boundary = std::move(boundary);
            return selection;
        }
        if (selection.firstBoundaryFailure.empty()) {
            selection.firstBoundaryFailure = boundary.message;
        }
    }
    return selection;
}

}
