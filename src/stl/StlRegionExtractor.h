#pragma once

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "stl/StlCropReport.h"
#include "stl/StlMesh.h"

namespace spo {

enum class StlCropMode {
    CentroidOnly,
    ConservativeBoundaryBand
};

struct StlRegionExtractorOptions {
    StlCropMode mode = StlCropMode::CentroidOnly;
    double bboxMarginRatio = 0.01;
    double minMargin = 0.1;
    bool includeVertexInsideTriangles = true;
    bool includeEdgeMidpointInsideTriangles = true;
    bool includeBoundaryBandTriangles = true;
    double boundaryBandTolerance = 0.2;
    double surfaceToleranceMultiplier = 1.0;
    double maxConservativeLeakRatio = 3.0;
    bool repairBoundaryLoopCoverage = true;
    int boundaryLoopSamplesPerEdge = 5;
    double boundaryLoopCoverageTolerance = 0.1;
    double boundaryLoopConnectivityTolerance = 1.0e-5;
    int maxBoundaryLoopRepairTriangles = 256;
};

struct StlRegionExtractResult {
    bool success = false;
    StlMesh localMesh;
    StlCropReport report;
};

class StlRegionExtractor {
public:
    StlRegionExtractResult extract(
        const ShapeDocument& document,
        const MergeCandidate& candidate,
        const StlMesh& sourceMesh,
        const StlRegionExtractorOptions& options = {}) const;
};

}
