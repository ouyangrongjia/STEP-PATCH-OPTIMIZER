#include "reduction/region_refit/RegionalSurfaceRefitTypes.h"

#include <algorithm>
#include <cmath>

namespace spo {

std::string classifyRegionEdgeConsistency(
    const RegionEdgeConsistencyReport& report,
    const double tolerance) {
    const auto triangulationMismatch = std::any_of(
        report.triangulationPolylineSeparations.begin(),
        report.triangulationPolylineSeparations.end(),
        [&](const auto separation) { return separation > tolerance; });
    if (report.freeEdge || report.multipleEdge || !report.sharedSameEdge) {
        return "TopologicalOpening";
    }
    if (!std::isfinite(report.curveOnSurfaceMaximumDeviation) ||
        report.curveOnSurfaceMaximumDeviation > tolerance ||
        !report.sameRange || !report.sameParameter) {
        return "CurveOnSurfaceMismatch";
    }
    if (report.adjacentSurfaceMaximumSeparation > tolerance) {
        return "AdjacentSurfaceSeparation";
    }
    return triangulationMismatch ? "TriangulationArtifact" : "Passed";
}

const char* toString(const RegionRefitLayoutMode mode) {
    switch (mode) {
    case RegionRefitLayoutMode::FixedPatchCount: return "FixedPatchCount";
    case RegionRefitLayoutMode::TargetPatchCount: return "TargetPatchCount";
    case RegionRefitLayoutMode::Adaptive: return "Adaptive";
    }
    return "Adaptive";
}

const char* toString(const RegionRefitAdaptiveObjective objective) {
    switch (objective) {
    case RegionRefitAdaptiveObjective::MinimumQualified: return "MinimumQualified";
    }
    return "MinimumQualified";
}

const char* toString(const RegionRefitSamplingMode mode) {
    switch (mode) {
    case RegionRefitSamplingMode::AdaptiveOcct: return "AdaptiveOcct";
    case RegionRefitSamplingMode::QualityBalanced: return "QualityBalanced";
    case RegionRefitSamplingMode::UniformUv: return "UniformUv";
    }
    return "AdaptiveOcct";
}

const char* toString(const RegionRefitExecutionMode mode) {
    switch (mode) {
    case RegionRefitExecutionMode::ValidatedOnly: return "ValidatedOnly";
    case RegionRefitExecutionMode::BestEffortThroughSewing: return "BestEffortThroughSewing";
    }
    return "ValidatedOnly";
}

const char* toString(const RegionRefitStatus status) {
    switch (status) {
    case RegionRefitStatus::NotStarted: return "NotStarted";
    case RegionRefitStatus::Success: return "Success";
    case RegionRefitStatus::InvalidRequest: return "InvalidRequest";
    case RegionRefitStatus::UnsupportedTopology: return "UnsupportedTopology";
    case RegionRefitStatus::TimeBudgetExceeded: return "TimeBudgetExceeded";
    case RegionRefitStatus::MemoryBudgetExceeded: return "MemoryBudgetExceeded";
    case RegionRefitStatus::SamplingBudgetExceeded: return "SamplingBudgetExceeded";
    case RegionRefitStatus::NoValidatedLayoutWithinBudget: return "NoValidatedLayoutWithinBudget";
    case RegionRefitStatus::NoSewableGeometry: return "NoSewableGeometry";
    case RegionRefitStatus::Failed: return "Failed";
    }
    return "Failed";
}

const char* toString(const RegionRefitStage stage) {
    switch (stage) {
    case RegionRefitStage::None: return "None";
    case RegionRefitStage::Constraints: return "Constraints";
    case RegionRefitStage::Sampling: return "Sampling";
    case RegionRefitStage::CurveNetwork: return "CurveNetwork";
    case RegionRefitStage::Layout: return "Layout";
    case RegionRefitStage::Fitting: return "Fitting";
    case RegionRefitStage::Retrim: return "Retrim";
    case RegionRefitStage::Sewing: return "Sewing";
    case RegionRefitStage::StrictTopology: return "StrictTopology";
    case RegionRefitStage::StepRoundtrip: return "StepRoundtrip";
    case RegionRefitStage::ExternalCad: return "ExternalCad";
    }
    return "None";
}

const char* toString(const RegionBoundaryContinuity continuity) {
    switch (continuity) {
    case RegionBoundaryContinuity::Free: return "Free";
    case RegionBoundaryContinuity::HardCrease: return "HardCrease";
    case RegionBoundaryContinuity::Smooth: return "Smooth";
    case RegionBoundaryContinuity::Locked: return "Locked";
    }
    return "HardCrease";
}

const char* toString(const RegionRefitCachePolicy policy) {
    switch (policy) {
    case RegionRefitCachePolicy::Disabled: return "Disabled";
    case RegionRefitCachePolicy::ReadWrite: return "ReadWrite";
    case RegionRefitCachePolicy::ReadOnly: return "ReadOnly";
    case RegionRefitCachePolicy::Refresh: return "Refresh";
    }
    return "Disabled";
}

bool RegionRefitResult::canApply() const {
    return report.status == RegionRefitStatus::Success &&
        report.productValidated &&
        !finalShape.IsNull();
}

}
