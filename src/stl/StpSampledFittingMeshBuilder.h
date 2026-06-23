#pragma once

#include "brep/ShapeDocument.h"
#include "common/GeometryTypes.h"
#include "merge/MergeCandidate.h"
#include "stl/StlMesh.h"

#include <filesystem>
#include <string>
#include <vector>

namespace spo {

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
