#pragma once

#include "common/GeometryTypes.h"
#include "merge/MergeCandidate.h"

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
    double boundaryBandOffset = 0.2;
    double boundaryBandCoverageTolerance = 0.1;
    double boundaryBandTriangleTolerance = 0.2;
    double cropBboxMarginRatio = 0.01;
    double cropMinMargin = 0.1;
    int maxTriangleDecisionRecords = 512;
    int maxRejectedTriangleOverlayCount = 200;
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

struct StlTriangleCropDecision {
    int triangleIndex = -1;
    EdgeId nearestBoundaryEdgeId = -1;
    bool bboxIntersectsExpandedCandidate = false;
    bool centroidInsideCandidate = false;
    bool anyVertexInsideCandidate = false;
    bool anyEdgeMidpointInsideCandidate = false;
    bool nearOriginalBoundaryBand = false;
    bool keptByCurrentExtractor = false;
    bool shouldKeepConservative = false;
    std::string rejectReason;
    CropBoundarySamplePoint v0;
    CropBoundarySamplePoint v1;
    CropBoundarySamplePoint v2;
    CropBoundarySamplePoint centroid;
};

struct CropBoundaryDiagnosticsReport {
    bool success = false;
    bool originalBoundarySampled = false;
    bool stlCoverageEvaluated = false;
    bool patchBoundaryEvaluated = false;
    bool boundaryBandEvaluated = false;
    bool sourceTriangleAuditEvaluated = false;
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
    int boundaryBandSampleCount = 0;
    int boundaryBandMissingPointCount = 0;
    double boundaryBandMinDistance = 0.0;
    double boundaryBandMaxDistance = 0.0;
    double boundaryBandAverageDistance = 0.0;
    int sourceTriangleAuditCount = 0;
    int rejectedNearBoundaryTriangleCount = 0;
    int conservativeKeepCandidateCount = 0;
    int suspectedGapCount = 0;
    std::vector<EdgeId> suspectedGapEdgeIds;
    std::vector<EdgeId> boundaryBandMissingEdgeIds;
    std::vector<EdgeId> suspectedCropHoleEdgeIds;

    int patchOuterEdgeCount = 0;
    double stlCoverageTolerance = 0.0;
    double patchBoundaryTolerance = 0.0;
    double boundaryBandCoverageTolerance = 0.0;
    double boundaryBandOffset = 0.0;
    double boundaryBandTriangleTolerance = 0.0;

    std::vector<CropBoundaryEdgeSample> originalBoundaryEdges;
    std::vector<CropBoundaryEdgeSample> patchOuterEdges;
    std::vector<CropBoundaryEdgeSample> boundaryBandEdges;
    std::vector<CropBoundaryGapSegment> suspectedGapSegments;
    std::vector<CropBoundaryGapSegment> boundaryBandMissingSegments;
    std::vector<StlTriangleCropDecision> triangleDecisions;
    std::vector<StlTriangleCropDecision> rejectedNearBoundaryTriangles;

    std::string message;
    std::string warningMessage;
};

struct CropBoundaryDiagnosticsInput {
    const ShapeDocument* document = nullptr;
    const MergeCandidate* candidate = nullptr;
    const RegionBoundaryAnalysis* boundary = nullptr;
    const StlMesh* localStlMesh = nullptr;
    const StlMesh* sourceStlMesh = nullptr;
    const TopoDS_Shape* importedPatchShape = nullptr;
};

class CropBoundaryDiagnostics {
public:
    CropBoundaryDiagnosticsReport analyze(
        const CropBoundaryDiagnosticsInput& input,
        const CropBoundaryDiagnosticsOptions& options = {}) const;
};

}
