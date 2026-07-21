#pragma once

#include "brep/ShapeDocument.h"
#include "reduction/region_refit/RegionReferenceMesh.h"
#include "reduction/region_refit/RegionalSurfaceRefitTypes.h"

#include <cstddef>
#include <string>

namespace spo {

struct StepAdaptiveRegionSamplingReport {
    RegionRefitStatus status = RegionRefitStatus::Failed;
    std::string message;
    std::size_t sourceFaceCount = 0;
    std::size_t vertexCount = 0;
    std::size_t triangleCount = 0;
    std::size_t boundarySampleCount = 0;
    std::size_t boundaryEndpointCount = 0;
    std::size_t internalSeamVertexCount = 0;
    std::size_t weldedInternalSeamVertexCount = 0;
    std::size_t normalRefinementCount = 0;
    std::size_t discardedDegenerateTriangleCount = 0;
    std::size_t normalDiscontinuityVertexCount = 0;
    double linearDeflection = 0.0;
    double angularDeflectionDegrees = 0.0;
    double boundaryChordTolerance = 0.0;
    double maximumInteriorChordError = 0.0;
    double maximumInteriorNormalErrorDegrees = 0.0;
    FaceId maximumInteriorNormalFaceId = 0;
    std::size_t maximumInteriorNormalTriangleIndex = 0;
    double maximumBoundaryChordError = 0.0;
    std::string meshFingerprint;

    bool success() const;
};

struct StepAdaptiveRegionSamplingResult {
    StepAdaptiveRegionSamplingReport report;
    RegionReferenceMesh mesh;
};

class StepAdaptiveRegionSampler {
public:
    StepAdaptiveRegionSamplingResult sample(
        const ShapeDocument& document,
        const RegionTopologyConstraints& constraints,
        const RegionRefitOptions& options) const;
};

}
