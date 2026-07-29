#include "reduction/region_refit/RecoveredFittingSpecification.h"

#include <array>

namespace spo {

const char* toString(const RecoveredEvidenceLevel level) {
    switch (level) {
    case RecoveredEvidenceLevel::ConfirmedStatic: return "ConfirmedStatic";
    case RecoveredEvidenceLevel::ConfirmedDynamic: return "ConfirmedDynamic";
    case RecoveredEvidenceLevel::ConfirmedStaticAndDynamic:
        return "ConfirmedStaticAndDynamic";
    case RecoveredEvidenceLevel::BehavioralSubstitute:
        return "BehavioralSubstitute";
    case RecoveredEvidenceLevel::Inferred: return "Inferred";
    case RecoveredEvidenceLevel::Rejected: return "Rejected";
    }
    return "Inferred";
}

bool RecoveredFittingTerm::recovered() const {
    return evidence == RecoveredEvidenceLevel::ConfirmedStaticAndDynamic ||
        evidence == RecoveredEvidenceLevel::BehavioralSubstitute;
}

bool RecoveredFittingSpecification::behaviorEquivalentReady() const {
    const std::array<const RecoveredFittingTerm*, 10> required {
        &parameterization,
        &dataTerm,
        &normalTerm,
        &fairnessTerm,
        &g0Constraint,
        &g1Constraint,
        &g2Constraint,
        &legalityPredicates,
        &refinementPolicy,
        &stoppingConditions};
    for (const auto* term : required) {
        if (!term->recovered()) return false;
    }
    return degreeU > 0 && degreeV > 0 &&
        nonAdaptiveMode >= 0 && adaptiveMode >= 0;
}

RecoveredFittingSpecification geomagicWrap2021MechanicalSpecification() {
    RecoveredFittingSpecification result;
    result.specificationId =
        "geomagic-wrap-2021-mechanical-fitting-partial-v1";
    result.pinnedModule = "sqcomponent.dll";
    result.pinnedSha256 =
        "912A506EE0426CA99F61D1C63BC9C70B8E9098D34682AF71226B004EECBB84E9";
    result.degreeU = 3;
    result.degreeV = 3;
    result.nonAdaptiveMode = 1;
    result.adaptiveMode = 19;
    result.parameterization.evidence = RecoveredEvidenceLevel::Inferred;
    result.dataTerm.evidence = RecoveredEvidenceLevel::Inferred;
    result.normalTerm.evidence = RecoveredEvidenceLevel::Inferred;
    result.storedFacetNormalHandling.evidence =
        RecoveredEvidenceLevel::ConfirmedDynamic;
    result.storedFacetNormalHandling.equation =
        "Stored binary STL facet normals do not affect the observed output; "
        "recomputation and omission remain indistinguishable.";
    result.storedFacetNormalHandling.evidenceReference =
        "20260729_sinusoid_f4_stored_normal_experiment_v1;"
        "20260729_sinusoid_f4_stored_normal_zero_experiment_v1";
    result.fairnessTerm.evidence = RecoveredEvidenceLevel::Inferred;
    result.g0Constraint.evidence = RecoveredEvidenceLevel::Inferred;
    result.g1Constraint.evidence = RecoveredEvidenceLevel::Inferred;
    result.g2Constraint.evidence = RecoveredEvidenceLevel::Inferred;
    result.legalityPredicates.evidence = RecoveredEvidenceLevel::Inferred;
    result.refinementPolicy.evidence = RecoveredEvidenceLevel::Inferred;
    result.stoppingConditions.evidence = RecoveredEvidenceLevel::Inferred;
    return result;
}

}
