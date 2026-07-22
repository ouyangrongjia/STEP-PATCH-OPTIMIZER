#pragma once

#include "brep/ShapeDocument.h"
#include "reduction/region_refit/RegionReferenceMesh.h"
#include "reduction/region_refit/RegionalSurfaceRefitTypes.h"

#include <cstddef>
#include <chrono>
#include <string>

namespace spo {

struct RegionMeshQualityReport {
    bool success = false;
    bool timeBudgetExceeded = false;
    std::string message;
    std::size_t splitCount = 0;
    std::size_t collapseCount = 0;
    std::size_t flipCount = 0;
    std::size_t relocationCount = 0;
    std::size_t zeroAreaTriangleCount = 0;
    std::size_t nonManifoldEdgeCount = 0;
    double targetEdgeLength = 0.0;
    double edgeLengthP01 = 0.0;
    double edgeLengthP50 = 0.0;
    double edgeLengthP99 = 0.0;
    double edgeLengthP99P01Ratio = 0.0;
    double minimumAngleP05Degrees = 0.0;
    double minimumAngleP50Degrees = 0.0;
    double aspectRatioP50 = 0.0;
    double aspectRatioP95 = 0.0;
    double aspectRatioP99 = 0.0;
    double normalizedQualityP05 = 0.0;
    double normalizedQualityP50 = 0.0;
};

struct QualityBalancedRegionRemeshResult {
    RegionReferenceMesh mesh;
    RegionMeshQualityReport report;
};

RegionMeshQualityReport measureRegionMeshQuality(const RegionReferenceMesh& mesh);

class QualityBalancedRegionRemesher {
public:
    QualityBalancedRegionRemeshResult remesh(
        const ShapeDocument& document,
        const RegionTopologyConstraints& constraints,
        const RegionRefitOptions& options,
        const RegionReferenceMesh& input,
        std::chrono::steady_clock::time_point deadline) const;
};

}
