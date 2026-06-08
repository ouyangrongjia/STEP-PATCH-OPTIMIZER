#pragma once

#include "common/GeometryTypes.h"

#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace spo {

class ShapeDocument;
class StlMesh;
struct RegionBoundaryAnalysis;

struct CropBoundaryDiagnosticsOptions {
    int minSamplesPerEdge = 5;
    int maxSamplesPerEdge = 64;
    double targetSampleSpacing = 0.5;
    double stlCoverageTolerance = 0.1;
    double patchBoundaryTolerance = 0.1;
};

struct CropBoundarySamplePoint {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double parameter = 0.0;
    double normalizedParameter = 0.0;
};

struct CropBoundaryEdgeSample {
    EdgeId edgeId = -1;
    int sampleCount = 0;
    double edgeLength = 0.0;
    bool singleClosedOuterLoop = false;
    std::vector<CropBoundarySamplePoint> samples;
};

struct CropBoundaryGapSegment {
    std::string source;
    EdgeId edgeId = -1;
    int startSampleIndex = 0;
    int endSampleIndex = 0;
    double startParameter = 0.0;
    double endParameter = 0.0;
    double startNormalizedParameter = 0.0;
    double endNormalizedParameter = 0.0;
    double maxDistance = 0.0;
    std::vector<CropBoundarySamplePoint> samples;
};

struct CropBoundaryDiagnosticsReport {
    bool success = false;
    bool originalBoundarySampled = false;
    bool stlCoverageEvaluated = false;
    bool patchBoundaryEvaluated = false;
    bool singleClosedOuterLoop = false;

    int originalBoundarySampleCount = 0;
    int stlCoverageMissingPointCount = 0;
    double stlCoverageMinDistance = 0.0;
    double stlCoverageMaxDistance = 0.0;
    double stlCoverageAverageDistance = 0.0;
    int patchBoundaryMissingPointCount = 0;
    double patchBoundaryMinDistance = 0.0;
    double patchBoundaryMaxDistance = 0.0;
    double patchBoundaryAverageDistance = 0.0;
    int suspectedGapCount = 0;
    std::vector<EdgeId> suspectedGapEdgeIds;

    int patchOuterEdgeCount = 0;
    double stlCoverageTolerance = 0.0;
    double patchBoundaryTolerance = 0.0;

    std::vector<CropBoundaryEdgeSample> originalBoundaryEdges;
    std::vector<CropBoundaryEdgeSample> patchOuterEdges;
    std::vector<CropBoundaryGapSegment> suspectedGapSegments;

    std::string message;
    std::string warningMessage;
};

struct CropBoundaryDiagnosticsInput {
    const ShapeDocument* document = nullptr;
    const RegionBoundaryAnalysis* boundary = nullptr;
    const StlMesh* localStlMesh = nullptr;
    const TopoDS_Shape* importedPatchShape = nullptr;
};

class CropBoundaryDiagnostics {
public:
    CropBoundaryDiagnosticsReport analyze(
        const CropBoundaryDiagnosticsInput& input,
        const CropBoundaryDiagnosticsOptions& options = {}) const;
};

}
