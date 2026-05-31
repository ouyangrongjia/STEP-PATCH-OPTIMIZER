#include "merge/CandidateFilters.h"

namespace spo {

CandidateTypeCounts countCandidateTypes(const std::vector<MergeCandidate>& candidates) {
    CandidateTypeCounts counts;
    for (const auto& candidate : candidates) {
        switch (candidate.candidate_type) {
        case MergeCandidateType::PlaneLike:
            ++counts.plane_like;
            break;
        case MergeCandidateType::CylinderLike:
            ++counts.cylinder_like;
            break;
        case MergeCandidateType::SphereLike:
            ++counts.sphere_like;
            break;
        case MergeCandidateType::ConeLike:
            ++counts.cone_like;
            break;
        case MergeCandidateType::TorusLike:
            ++counts.torus_like;
            break;
        case MergeCandidateType::FeatureBoundedRefit:
            ++counts.feature_bounded_refit;
            break;
        case MergeCandidateType::FreeformG1:
            ++counts.freeform_g1;
            break;
        case MergeCandidateType::FreeformG2:
            ++counts.freeform_g2;
            break;
        case MergeCandidateType::SameDomain:
        case MergeCandidateType::Unknown:
            ++counts.unknown;
            break;
        }
    }
    return counts;
}

std::vector<MergeCandidate> filterCandidatesByType(
    const std::vector<MergeCandidate>& candidates,
    MergeCandidateType type,
    bool includeHidden) {
    std::vector<MergeCandidate> result;
    for (const auto& candidate : candidates) {
        if (candidate.candidate_type == type &&
            (includeHidden || candidate.status != MergeCandidateStatus::Hidden)) {
            result.push_back(candidate);
        }
    }
    return result;
}

std::vector<MergeCandidate> filterNonHiddenCandidates(const std::vector<MergeCandidate>& candidates) {
    std::vector<MergeCandidate> result;
    for (const auto& candidate : candidates) {
        if (candidate.status != MergeCandidateStatus::Hidden) {
            result.push_back(candidate);
        }
    }
    return result;
}

std::vector<MergeCandidate> filterMergeablePlaneCandidates(const std::vector<MergeCandidate>& candidates) {
    std::vector<MergeCandidate> result;
    for (const auto& candidate : candidates) {
        if (candidate.valid &&
            candidate.candidate_type == MergeCandidateType::PlaneLike &&
            candidate.status != MergeCandidateStatus::Rejected &&
            candidate.status != MergeCandidateStatus::Hidden) {
            result.push_back(candidate);
        }
    }
    return result;
}

std::vector<MergeCandidate> filterMergeableSphereCandidates(const std::vector<MergeCandidate>& candidates) {
    std::vector<MergeCandidate> result;
    for (const auto& candidate : candidates) {
        if (candidate.valid &&
            candidate.candidate_type == MergeCandidateType::SphereLike &&
            candidate.status != MergeCandidateStatus::Rejected &&
            candidate.status != MergeCandidateStatus::Hidden &&
            candidate.risk_level != MergeRiskLevel::High &&
            candidate.face_count >= 2) {
            result.push_back(candidate);
        }
    }
    return result;
}

}
