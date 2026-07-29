#pragma once

#include "reduction/region_refit/RegionPatchLayout.h"
#include "reduction/region_refit/RegionReferenceMesh.h"

#include <Geom_BSplineSurface.hxx>

#include <string>
#include <vector>

namespace spo {

struct SurfaceLegalityReport {
    bool finiteControlNet = true;
    bool positiveWeights = true;
    bool validDegreesAndPoles = true;
    bool strictlyIncreasingKnots = true;
    int uvFlippedTriangleCount = 0;
    int uvDegenerateTriangleCount = 0;
    int jacobianDegenerateSampleCount = 0;
    int jacobianOrientationFlipCount = 0;
    double minimumNormalizedJacobian = 1.0;
    bool passed = true;
    std::vector<std::string> rejectionReasons;
};

struct SurfaceDifferentialQualityReport {
    double physicalArea = 0.0;
    double areaWeightedBendingEnergy = 0.0;
    double areaWeightedNormalGradientEnergy = 0.0;
    double areaWeightedMeanCurvatureVariation = 0.0;
    double legacyControlNetSecondDifferenceEnergy = 0.0;
    double maximumNormalVariationDegrees = 0.0;
    double conditionNumberP50 = 0.0;
    double conditionNumberP95 = 0.0;
    double conditionNumberP99 = 0.0;
    SurfaceLegalityReport legality;
};

std::string classifySurfaceUvTopology(
    const SurfaceLegalityReport& report);

std::string classifySurfaceJacobianLegality(
    const SurfaceLegalityReport& report);

bool orientPatchTriangles(
    const RegionReferenceMesh& mesh,
    const RegionPatchParameterization& patch,
    std::vector<int>& orientations,
    std::string& message);

SurfaceDifferentialQualityReport evaluateSurfaceDifferentialQuality(
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    const std::vector<Handle(Geom_BSplineSurface)>& surfaces,
    int samplesPerDirection = 25);

}
