#pragma once

#include "common/GeometryTypes.h"

#include <TopoDS_Shape.hxx>

namespace spo {

class ShapeDocument;
struct MergeCandidate;
struct RegionBoundaryAnalysis;

struct PatchTrimDiagnosticsOptions {
    int boundarySamplesPerEdge = 7;
    int surfaceGridDivisions = 4;
    int maxBoundarySamples = 256;
    int maxSurfaceSamples = 512;
    double distanceTolerance = 0.03;
};

struct PatchTrimDiagnosticsReport {
    bool captured = false;
    int replacementFaceCount = 0;
    int trimWireInvalidCount = 0;
    int trimUvLoopSelfIntersectionCount = 0;

    int overCoverSampleCount = 0;
    int overCoverTotalSampleCount = 0;
    double overCoverRatio = 0.0;
    double overCoverMaxDistance = 0.0;

    int underCoverSampleCount = 0;
    int underCoverTotalSampleCount = 0;
    double underCoverMaxDistance = 0.0;

    double boundaryGapMax = 0.0;
    double boundaryGapP95 = 0.0;
    double boundaryGapRms = 0.0;
    int worstBoundaryEdgeId = -1;

    double internalSeamGapMax = 0.0;
    double internalSeamGapP95 = 0.0;
    double internalSeamGapRms = 0.0;
    int worstInternalEdgeId = -1;

    bool roundtripCompared = false;
    bool roundtripChanged = false;
};

struct PatchTrimDiagnosticsInput {
    const ShapeDocument* beforeDocument = nullptr;
    const MergeCandidate* candidate = nullptr;
    const RegionBoundaryAnalysis* boundary = nullptr;
    const TopoDS_Shape* replacementShape = nullptr;
    const TopoDS_Shape* roundtripShape = nullptr;
};

class PatchTrimDiagnostics {
public:
    PatchTrimDiagnosticsReport analyze(
        const PatchTrimDiagnosticsInput& input,
        const PatchTrimDiagnosticsOptions& options = {}) const;
};

}
