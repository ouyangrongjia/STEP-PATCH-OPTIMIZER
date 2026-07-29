#pragma once

#include "reduction/region_refit/RegionCurveNetwork.h"
#include "reduction/region_refit/RegionPatchLayout.h"
#include "reduction/region_refit/SurfaceDifferentialQuality.h"

#include <Geom_BSplineSurface.hxx>
#include <TopoDS_Shape.hxx>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace spo {

struct RegionSurfaceParameterMappingSample {
    std::size_t referenceVertex = 0;
    std::size_t carrierTriangle = 0;
    int patch = -1;
    std::array<double, 2> surfaceParameter {};
};

struct RegionSurfaceParameterMappingResult {
    std::vector<RegionSurfaceParameterMappingSample> samples;
    std::size_t sameFaceBarycentricCount = 0;
    std::size_t sameFaceFallbackCount = 0;
    std::size_t crossFaceFailureCount = 0;

    bool complete(std::size_t expectedCount) const {
        return samples.size() == expectedCount &&
            crossFaceFailureCount == 0;
    }
};

struct RegionSurfaceQualityMetrics {
    int totalPoleCount = 0;
    int maximumPoleCountPerSide = 0;
    int totalKnotCount = 0;
    std::vector<RegionSurfaceComplexityReport> surfaceComplexities;
    double fairnessEnergy = 0.0;
    double controlNetSecondDifferenceEnergy = 0.0;
    double physicalSurfaceArea = 0.0;
    double areaWeightedNormalGradientEnergy = 0.0;
    double areaWeightedMeanCurvatureVariation = 0.0;
    double maximumNormalVariationDegrees = 0.0;
    double curvatureOscillation = 0.0;
    double minimumNormalizedJacobian = 0.0;
    double conditionNumberP50 = 0.0;
    double conditionNumberP95 = 0.0;
    double conditionNumberP99 = 0.0;
    bool hardFailure = false;
    SurfaceLegalityReport legality;
};

struct RegionFittedPatchNetwork {
    RegionPatchLayout layout;
    std::vector<Handle(Geom_BSplineSurface)> surfaces;
    TopoDS_Shape inspectionShape;
    double holdoutMaximumDistance = 0.0;
    double holdoutMaximumNormalDegrees = 0.0;
    double boundaryMaximumDistance = 0.0;
    double seamMaximumDistance = 0.0;
    double seamMaximumCurveDeviation = 0.0;
    double seamMaximumNormalDegrees = 0.0;
    double seamMaximumCurvatureDifference = 0.0;
    bool seamContinuityGatePassed = false;
    double normalizedViolationScore = 0.0;
    bool fitGatePassed = false;
    bool boundaryLocked = false;
    bool constructibleForBRep = false;
    RegionSurfaceQualityMetrics quality;
    std::string fitOutcome = "NotEvaluated";
};

struct JointBSplineNetworkFitResult {
    RegionRefitStatus status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
    std::string message;
    std::vector<RegionFitAttemptReport> attempts;
    std::shared_ptr<RegionFittedPatchNetwork> network;
    std::shared_ptr<RegionFittedPatchNetwork> bestEffortNetwork;

    bool success() const;
};

bool regionLocalSupportDomainPassed(
    const Handle(Geom_BSplineSurface)& surface,
    const RegionReferenceMesh& carrier,
    const RegionPatchParameterization& parameterization,
    const RegionRefitOptions& options,
    std::string& reason);

RegionSurfaceParameterMappingResult mapRegionReferenceSamplesToSurfaceParameters(
    const RegionReferenceMesh& fittingReference,
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout);

class JointBSplineNetworkFitter {
public:
    JointBSplineNetworkFitResult fit(
        const RegionReferenceMesh& fittingReference,
        const RegionReferenceMesh& layoutCarrier,
        const RegionTopologyConstraints& constraints,
        const std::vector<RegionPatchLayout>& candidates,
        const std::vector<RegionCurveNetwork>& curveNetworks,
        const RegionRefitOptions& options,
        std::chrono::steady_clock::time_point deadline) const;

    JointBSplineNetworkFitResult fit(
        const RegionReferenceMesh& fittingReference,
        const RegionReferenceMesh& layoutCarrier,
        const RegionTopologyConstraints& constraints,
        const std::vector<RegionPatchLayout>& candidates,
        const RegionRefitOptions& options,
        std::chrono::steady_clock::time_point deadline) const;

    JointBSplineNetworkFitResult fit(
        const RegionReferenceMesh& mesh,
        const RegionTopologyConstraints& constraints,
        const std::vector<RegionPatchLayout>& candidates,
        const RegionRefitOptions& options,
        std::chrono::steady_clock::time_point deadline) const;
};

}
