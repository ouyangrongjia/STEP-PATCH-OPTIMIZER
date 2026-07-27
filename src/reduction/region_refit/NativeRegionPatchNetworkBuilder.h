#pragma once

#include "patch/BoundaryConstrainedPatchBuilder.h"
#include "reduction/region_refit/JointBSplineNetworkFitter.h"

#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace spo {

struct NativeRegionPatchNetworkBuildResult {
    RegionRefitStatus status = RegionRefitStatus::Failed;
    std::string message;
    TopoDS_Shape replacementShape;
    std::vector<TopoDS_Face> replacementFaces;
    std::vector<TopoDS_Edge> internalSeamEdges;
    std::vector<BoundaryConstrainedPatchSplitBoundarySegment> splitBoundarySegments;
    int reusedOriginalBoundaryEdgeCount = 0;
    int splitBoundarySegmentCount = 0;
    int internalSeamCount = 0;
    int pcurveProjectionCount = 0;
    int diagnosticPcurveFallbackCount = 0;
    int sameParameterCheckCount = 0;
    int sameParameterFailureCount = 0;
    int brepValidationFailureCount = 0;
    double maximumSameParameterDeviation = 0.0;
    std::vector<RegionEdgeConsistencyReport> edgeConsistency;

    bool success() const;
    bool constructible() const;
};

class NativeRegionPatchNetworkBuilder {
public:
    NativeRegionPatchNetworkBuildResult build(
        const RegionReferenceMesh& mesh,
        const RegionTopologyConstraints& constraints,
        const RegionFittedPatchNetwork& network,
        const RegionCurveNetwork& curveNetwork,
        double curveOnSurfaceTolerance,
        bool allowUnvalidatedFaces = false) const;
};

}
