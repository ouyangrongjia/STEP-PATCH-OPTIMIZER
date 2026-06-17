#pragma once

#include "common/GeometryTypes.h"
#include "patch/MultiFacePatchAnalyzer.h"
#include "patch/BoundaryConstrainedSurfaceRetrim.h"
#include "patch/PatchReplacementInput.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace spo {

enum class BoundaryConstrainedBuildFailureReason {
    None,
    InvalidInput,
    InvalidBoundary,
    NoPatchFaces,
    ReplacementBuildFailed,
    BoundaryMismatch,
    ShapeFixFailed
};

const char* toString(BoundaryConstrainedBuildFailureReason reason);

struct BoundaryConstrainedPatchSplitBoundarySegment {
    EdgeId edgeId = 0;
    double firstParameter = 0.0;
    double lastParameter = 0.0;
    TopoDS_Edge edge;
};

struct BoundaryConstrainedPatchBuildOptions {
    bool allowMultiFaceFragment = true;
    bool allowOneFaceSpecialPath = true;
    bool keepInternalPatchEdges = true;
    bool preferOriginalBoundarySurfaceRetrim = true;
    bool enableMultiSurfaceBoundaryShell = true;
    bool allowPatchOuterBoundaryFallback = false;
    double bboxToleranceRatio = 4.0;
    BoundaryConstrainedSurfaceRetrimOptions surfaceRetrimOptions;
};

struct BoundaryConstrainedPatchBuildResult {
    bool success = false;
    BoundaryConstrainedBuildFailureReason failureReason = BoundaryConstrainedBuildFailureReason::None;

    TopoDS_Shape replacementShape;
    std::vector<TopoDS_Face> replacementFaces;
    std::vector<TopoDS_Edge> internalPatchEdges;
    std::vector<FaceId> sourceFaceIds;

    bool usedMultiFaceFragment = false;
    bool usedOneFaceSpecialPath = false;
    bool usedOriginalBoundarySurfaceRetrim = false;
    bool attemptedMultiSurfaceBoundaryShell = false;
    bool usedMultiSurfaceBoundaryShell = false;
    bool boundaryMismatch = false;

    int sourceFaceCount = 0;
    int patchFaceCount = 0;
    int replacementFaceCount = 0;
    int internalPatchEdgeCount = 0;

    int retrimSelectedPatchFaceIndex = -1;
    int retrimBoundarySampleCount = 0;
    int retrimProjectedSampleCount = 0;
    int retrimFailedProjectionCount = 0;
    double retrimMaxProjectionDistance = 0.0;
    double retrimAverageProjectionDistance = 0.0;
    int retrimSurfaceCoverageProjectedSampleCount = 0;
    int retrimSurfaceCoverageFailedProjectionCount = 0;
    double retrimSurfaceCoverageMaxProjectionDistance = 0.0;
    double retrimSurfaceCoverageAverageProjectionDistance = 0.0;
    std::vector<EdgeId> retrimSurfaceCoverageUncoveredEdgeIds;

    int multiSurfaceBoundarySampleCount = 0;
    int multiSurfaceProjectedSampleCount = 0;
    int multiSurfaceFailedProjectionCount = 0;
    double multiSurfaceMaxProjectionDistance = 0.0;
    double multiSurfaceAverageProjectionDistance = 0.0;
    int multiSurfaceAssignedBoundarySegmentCount = 0;
    int multiSurfaceSplitBoundaryEdgeCount = 0;
    int multiSurfaceBuiltFaceCount = 0;
    int multiSurfaceClosedWireCount = 0;
    int multiSurfaceOpenWireCount = 0;
    int multiSurfaceMultipleClosedWireFaceCount = 0;
    int multiSurfaceFailedPatchFaceIndex = -1;
    int multiSurfaceFailedFaceEdgeCount = 0;
    int multiSurfaceBoundaryEdgePcurveRebuildAttemptCount = 0;
    int multiSurfaceBoundaryEdgePcurveRebuildSuccessCount = 0;
    int multiSurfaceBoundaryEdgePcurveRebuildFailureCount = 0;
    int multiSurfaceBoundaryEdgeSameParameterCheckCount = 0;
    int multiSurfaceBoundaryEdgeSameParameterFailureCount = 0;
    double multiSurfaceBoundaryEdgeMaxSameParameterDeviation = 0.0;
    std::vector<BoundaryConstrainedPatchSplitBoundarySegment> multiSurfaceSplitBoundarySegments;
    std::vector<EdgeId> multiSurfaceFailedEdgeIds;
    std::vector<EdgeId> multiSurfaceBoundaryEdgePcurveRebuildFailedEdgeIds;
    std::vector<EdgeId> multiSurfaceBoundaryEdgeSameParameterFailedEdgeIds;

    std::string message;
    std::string warningMessage;
};

class BoundaryConstrainedPatchBuilder {
public:
    BoundaryConstrainedPatchBuildResult build(
        const PatchReplacementInput& input,
        const MultiFacePatchAnalysis& analysis,
        const BoundaryConstrainedPatchBuildOptions& options = {}) const;
};

}
