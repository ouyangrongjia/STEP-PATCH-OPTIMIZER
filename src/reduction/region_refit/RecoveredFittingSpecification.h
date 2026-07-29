#pragma once

#include <string>

namespace spo {

enum class RecoveredEvidenceLevel {
    ConfirmedStatic,
    ConfirmedDynamic,
    ConfirmedStaticAndDynamic,
    BehavioralSubstitute,
    Inferred,
    Rejected
};

const char* toString(RecoveredEvidenceLevel level);

struct RecoveredFittingTerm {
    RecoveredEvidenceLevel evidence = RecoveredEvidenceLevel::Inferred;
    std::string equation;
    std::string evidenceReference;

    bool recovered() const;
};

struct RecoveredFittingSpecification {
    std::string specificationId;
    std::string status = "ResearchPartial";
    std::string pinnedModule;
    std::string pinnedSha256;
    int degreeU = 0;
    int degreeV = 0;
    int nonAdaptiveMode = -1;
    int adaptiveMode = -1;
    RecoveredFittingTerm parameterization;
    RecoveredFittingTerm dataTerm;
    RecoveredFittingTerm normalTerm;
    RecoveredFittingTerm storedFacetNormalHandling;
    RecoveredFittingTerm fairnessTerm;
    RecoveredFittingTerm g0Constraint;
    RecoveredFittingTerm g1Constraint;
    RecoveredFittingTerm g2Constraint;
    RecoveredFittingTerm legalityPredicates;
    RecoveredFittingTerm refinementPolicy;
    RecoveredFittingTerm stoppingConditions;

    bool behaviorEquivalentReady() const;
};

RecoveredFittingSpecification geomagicWrap2021MechanicalSpecification();

}
