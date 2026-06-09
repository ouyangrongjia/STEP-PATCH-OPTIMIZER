#pragma once

#include "common/GeometryTypes.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/BoundaryConstrainedSurfaceRetrim.h"
#include "patch/MultiFacePatchAnalyzer.h"

#include <TopoDS_Face.hxx>
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

struct BoundaryConstrainedMultiSurfaceShellResult {
    bool success = false;
    TopoDS_Shape replacementShape;
    std::vector<TopoDS_Face> replacementFaces;

    int boundarySampleCount = 0;
    int projectedSampleCount = 0;
    int failedProjectionCount = 0;
    double maxProjectionDistance = 0.0;
    double averageProjectionDistance = 0.0;

    int assignedBoundarySegmentCount = 0;
    int builtFaceCount = 0;
    int openWireCount = 0;
    std::vector<EdgeId> failedEdgeIds;

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
