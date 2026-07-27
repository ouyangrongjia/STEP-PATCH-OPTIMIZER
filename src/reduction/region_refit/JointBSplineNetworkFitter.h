#pragma once

#include "reduction/region_refit/RegionCurveNetwork.h"
#include "reduction/region_refit/RegionPatchLayout.h"

#include <Geom_BSplineSurface.hxx>
#include <TopoDS_Shape.hxx>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace spo {

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
