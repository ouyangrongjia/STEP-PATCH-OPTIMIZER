#pragma once

#include "common/GeometryTypes.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/BoundaryConstrainedSurfaceRetrim.h"
#include "patch/MultiFacePatchAnalyzer.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace spo {

class ShapeDocument;

struct BoundaryConstrainedMultiSurfaceShellOptions {
    int samplesPerEdge = 5;
    double projectionTolerance = 0.2;
    double wireConnectTolerance = 0.2;
};

struct BoundaryConstrainedSplitBoundarySegment {
    EdgeId edgeId = 0;
    double firstParameter = 0.0;
    double lastParameter = 0.0;
    TopoDS_Edge edge;
    int patchFaceIndex = -1;
};

struct BoundaryConstrainedMultiSurfaceShellResult {
    bool success = false;
    bool attempted = false;
    TopoDS_Shape replacementShape;
    std::vector<TopoDS_Face> replacementFaces;

    int boundarySampleCount = 0;
    int projectedSampleCount = 0;
    int failedProjectionCount = 0;
    double maxProjectionDistance = 0.0;
    double averageProjectionDistance = 0.0;

    int assignedBoundarySegmentCount = 0;
    int splitBoundaryEdgeCount = 0;
    int builtFaceCount = 0;
    int closedWireCount = 0;
    int openWireCount = 0;
    int multipleClosedWireFaceCount = 0;
    int failedPatchFaceIndex = -1;
    int failedFaceEdgeCount = 0;
    int boundaryEdgePcurveRebuildAttemptCount = 0;
    int boundaryEdgePcurveRebuildSuccessCount = 0;
    int boundaryEdgePcurveRebuildFailureCount = 0;
    int boundaryEdgeSameParameterCheckCount = 0;
    int boundaryEdgeSameParameterFailureCount = 0;
    double boundaryEdgeMaxSameParameterDeviation = 0.0;
    std::vector<BoundaryConstrainedSplitBoundarySegment> splitBoundarySegments;
    std::vector<EdgeId> failedEdgeIds;
    std::vector<EdgeId> boundaryEdgePcurveRebuildFailedEdgeIds;
    std::vector<EdgeId> boundaryEdgeSameParameterFailedEdgeIds;

    std::string message;
    std::string warningMessage;
};

class BoundaryConstrainedMultiSurfaceShellBuilder {
public:
    BoundaryConstrainedMultiSurfaceShellResult build(
        const ShapeDocument& document,
        const RegionBoundaryAnalysis& boundary,
        const MultiFacePatchAnalysis& analysis,
        const BoundaryConstrainedMultiSurfaceShellOptions& options = {}) const;
};

}
