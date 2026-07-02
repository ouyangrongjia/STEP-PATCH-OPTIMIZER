#pragma once

#include "brep/ShapeDocument.h"
#include "common/GeometryTypes.h"
#include "merge/MergeCandidate.h"
#include "stl/StlMesh.h"

#include <filesystem>
#include <string>
#include <vector>

namespace spo {

enum class CandidateSurfaceOverCoverBodyBridgeMode {
    Normal,
    None,
    Smooth
};

struct StpSampledFittingOptions {
    int boundarySamplesPerEdge = 16;
    int minBoundarySamplesPerEdge = 8;
    double chordError = 0.01;
    int maxInteriorDivisions = 20;
    int maxTotalSamples = 50000;
    int bandRingCount = 2;
    double bandRingSpacing = 0.3;
    bool preserveCornerPoints = true;
    bool includeBoundaryBand = true;
    bool enableCornerFeatureDenseSampling = false;
    int cornerFeatureSamplesPerEdge = 64;
    bool enableAdjacentFaceSupportCollar = false;
    int adjacentFaceSupportCollarSamplesPerEdge = 16;
    double adjacentFaceSupportCollarWidth = 0.05;
    int adjacentFaceSupportCollarRingCount = 1;
    bool enableAdaptiveAdjacentFaceSupportCollarWidth = false;
    double adjacentFaceSupportCollarUnderCover = 0.0;
    bool enableAdjacentFaceSupportCollarCornerClamp = false;
    int adjacentFaceSupportCollarCornerSmoothingIterations = 2;
    double adjacentFaceSupportCollarMaxOffsetScale = 1.25;
    bool enableCandidateSurfaceOverCover = false;
    int candidateSurfaceOverCoverSamplesPerEdge = 64;
    double candidateSurfaceOverCoverWidth = 0.25;
    int candidateSurfaceOverCoverRingCount = 3;
    double candidateSurfaceOverCoverCornerMiterMaxScale = 1.25;
    int candidateSurfaceOverCoverDisableOuterRings = 0;
    bool candidateSurfaceOverCoverSmoothStrip = false;
    CandidateSurfaceOverCoverBodyBridgeMode candidateSurfaceOverCoverBodyBridgeMode =
        CandidateSurfaceOverCoverBodyBridgeMode::Normal;
};

struct CandidateSurfaceOverCoverRejectionRecord {
    std::string category;
    int edgeId = -1;
    int sourceFaceId = -1;
    int sampleIndex = -1;
    int ringIndexA = -1;
    int ringIndexB = -1;
    StlTriangle triangle;
    StlVec3 centroid;
    double maxEdgeLength = 0.0;
    double minEdgeLength = 0.0;
    double localSpacing = 0.0;
    double edgeLengthRatio = 0.0;
    double normalLeakage = 0.0;
    bool longTriangle = false;
    bool degenerate = false;
};

struct CandidateSurfaceOverCoverProblemEdgeReport {
    int edgeId = -1;
    int sourceFaceId = -1;
    int acceptedSampleCount = 0;
    int sampleRejectedCount = 0;
    int bodyBridgeRejectedCount = 0;
    int boundaryToRingRejectedCount = 0;
    int ringToRingRejectedCount = 0;
    int longTriangleCount = 0;
    int directionFlipCount = 0;
    int indexMismatchCount = 0;
    double maxNormalLeakage = 0.0;
    double maxTriangleEdgeLength = 0.0;
    double maxRingClosureGap = 0.0;
    double maxRingClosureRatio = 0.0;
};

struct CandidateSurfaceOverCoverEdgeSourceReport {
    int edgeId = -1;
    int sourceFaceId = -1;
    int sampleCount = 0;
    int firstSampleIndex = -1;
    int lastSampleIndex = -1;
};

struct CandidateSurfaceOverCoverLeakagePoint {
    int edgeId = -1;
    int sourceFaceId = -1;
    int sampleIndex = -1;
    int ringIndex = -1;
    StlVec3 point;
    double normalLeakage = 0.0;
};

struct StpSampledFittingReport {
    bool success = false;
    std::string message;
    std::string warningMessage;

    int candidateId = -1;
    int sourceFaceCount = 0;
    int boundaryEdgeCount = 0;
    int boundarySampleCount = 0;
    int boundaryBandSampleCount = 0;
    bool cornerFeatureDenseSamplingEnabled = false;
    int featureEdgeDenseSampleCount = 0;
    int cornerAnchorSampleCount = 0;
    int cornerFeatureSurfaceDivisionCount = 0;
    bool adjacentFaceSupportCollarEnabled = false;
    double adjacentFaceSupportCollarWidth = 0.0;
    int adjacentFaceSupportCollarRingCount = 0;
    int adjacentFaceSupportCollarEdgeCount = 0;
    int adjacentFaceSupportCollarSampleCount = 0;
    int adjacentFaceSupportCollarTriangleCount = 0;
    int adjacentFaceSupportCollarAdjacentFaceSampleCount = 0;
    int adjacentFaceSupportCollarFallbackCount = 0;
    int adjacentFaceSupportCollarRejectedCount = 0;
    double adjacentFaceSupportCollarBoundaryCoverage = 0.0;
    bool adjacentFaceSupportCollarAdaptiveWidthEnabled = false;
    double adjacentFaceSupportCollarUnderCover = 0.0;
    double adjacentFaceSupportCollarBoundaryH95 = 0.0;
    double adjacentFaceSupportCollarEffectiveWidthMin = 0.0;
    double adjacentFaceSupportCollarEffectiveWidthMean = 0.0;
    double adjacentFaceSupportCollarEffectiveWidthMax = 0.0;
    int adjacentFaceSupportCollarAnchorCount = 0;
    int adjacentFaceSupportCollarBodyBridgeSampleCount = 0;
    int adjacentFaceSupportCollarBodyBridgeTriangleCount = 0;
    int adjacentFaceSupportCollarBodyBridgeRejectedCount = 0;
    int adjacentFaceSupportCollarBodyBridgeComponentCount = 0;
    double adjacentFaceSupportCollarBodyBridgeMaxGap = 0.0;
    bool adjacentFaceSupportCollarCornerClampEnabled = false;
    int adjacentFaceSupportCollarCornerClampCount = 0;
    double adjacentFaceSupportCollarMaxOffset = 0.0;
    bool candidateSurfaceOverCoverEnabled = false;
    double candidateSurfaceOverCoverWidth = 0.0;
    int candidateSurfaceOverCoverRingCount = 0;
    int candidateSurfaceOverCoverEdgeCount = 0;
    int candidateSurfaceOverCoverSampleCount = 0;
    int candidateSurfaceOverCoverTriangleCount = 0;
    int candidateSurfaceOverCoverFallbackCount = 0;
    int candidateSurfaceOverCoverRejectedCount = 0;
    double candidateSurfaceOverCoverBoundaryCoverage = 0.0;
    int candidateSurfaceOverCoverCornerMiterCount = 0;
    double candidateSurfaceOverCoverMaxOffset = 0.0;
    double candidateSurfaceOverCoverNormalLeakageMax = 0.0;
    int candidateSurfaceOverCoverDirectionFallbackCount = 0;
    int candidateSurfaceOverCoverLongTriangleCount = 0;
    double candidateSurfaceOverCoverMaxTriangleEdgeLength = 0.0;
    int candidateSurfaceOverCoverSourceFaceCount = 0;
    int candidateSurfaceOverCoverDirectionFlipCount = 0;
    int candidateSurfaceOverCoverGeneratedRingCount = 0;
    int candidateSurfaceOverCoverEmittedRingCount = 0;
    int candidateSurfaceOverCoverDisabledOuterRingCount = 0;
    int candidateSurfaceOverCoverEmittedSampleCount = 0;
    std::string candidateSurfaceOverCoverBodyBridgeMode = "normal";
    bool candidateSurfaceOverCoverSmoothStripEnabled = false;
    bool candidateSurfaceOverCoverSmoothTransitionEnabled = false;
    int candidateSurfaceOverCoverSmoothingPointCount = 0;
    double candidateSurfaceOverCoverSmoothingMaxDisplacement = 0.0;
    int candidateSurfaceOverCoverBodyBridgeTriangleCount = 0;
    int candidateSurfaceOverCoverBodyBridgeRejectedCount = 0;
    int candidateSurfaceOverCoverBoundaryToRingRejectedCount = 0;
    int candidateSurfaceOverCoverRingToRingRejectedCount = 0;
    int candidateSurfaceOverCoverEdgeBuildRejectedCount = 0;
    int candidateSurfaceOverCoverProblemEdgeCount = 0;
    int candidateSurfaceOverCoverRingPointCount = 0;
    double candidateSurfaceOverCoverRingClosureMaxGap = 0.0;
    double candidateSurfaceOverCoverRingClosureMaxRatio = 0.0;
    int candidateSurfaceOverCoverIndexMismatchCount = 0;
    int candidateSurfaceOverCoverSourceFaceTransitionCount = 0;
    int candidateSurfaceOverCoverEdgeSourceDistributionCount = 0;
    int candidateSurfaceOverCoverMaxNormalLeakageEdgeId = -1;
    int candidateSurfaceOverCoverMaxNormalLeakageSourceFaceId = -1;
    std::vector<CandidateSurfaceOverCoverRejectionRecord> candidateSurfaceOverCoverRejections;
    std::vector<CandidateSurfaceOverCoverProblemEdgeReport> candidateSurfaceOverCoverProblemEdges;
    std::vector<CandidateSurfaceOverCoverEdgeSourceReport> candidateSurfaceOverCoverEdgeSourceDistribution;
    std::vector<CandidateSurfaceOverCoverLeakagePoint> candidateSurfaceOverCoverLeakagePoints;
    int interiorSampleCount = 0;
    int outputTriangleCount = 0;
    std::filesystem::path outputPath;
    double samplingSpacing = 0.0;
    double boundarySpacing = 0.0;
    int bandRingCount = 0;
    StlBoundingBox bbox;
    StlBoundingBox output_bbox;
};

class StpSampledFittingMeshBuilder {
public:
    StpSampledFittingReport build(
        const ShapeDocument& document,
        const MergeCandidate& candidate,
        const StpSampledFittingOptions& options,
        StlMesh& outMesh) const;
};

}
