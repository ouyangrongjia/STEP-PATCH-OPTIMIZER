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
