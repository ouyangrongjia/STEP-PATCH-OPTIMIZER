#pragma once

#include "brep/ShapeDocument.h"
#include "common/GeometryTypes.h"

#include <Geom_BSplineSurface.hxx>
#include <TopoDS_Face.hxx>

#include <string>
#include <vector>

namespace spo {

class StlHybridParameterization;

struct RestrictedBSplinePatchFitOptions {
    double tolerance3d = 0.03;
    double normalToleranceDegrees = 15.0;
    double minimumNormalizedJacobian = 1.0e-5;
    double fairnessWeight = 0.01;
    int maximumControlPoints = 1024;
    int maximumIterations = 5;
    const StlHybridParameterization* hybridParameterization = nullptr;
    double stlReferenceWeight = 0.25;
    double maximumStlReferenceDistance = 0.2;
};

struct RestrictedBSplinePatchFitReport {
    bool success = false;
    std::string message;
    int trainingSampleCount = 0;
    int selectionSampleCount = 0;
    int holdoutSampleCount = 0;
    int boundaryHoldoutSampleCount = 0;
    int controlPointCount = 0;
    int uControlPointCount = 0;
    int vControlPointCount = 0;
    int iterationCount = 0;
    int knotInsertionCount = 0;
    double maxBoundaryDistance = 0.0;
    double maxSelectionDistance = 0.0;
    double maxSelectionNormalAngleDegrees = 0.0;
    double maxHoldoutDistance = 0.0;
    double maxHoldoutNormalAngleDegrees = 0.0;
    double minimumNormalizedJacobian = 0.0;
    bool parameterizationValid = false;
    bool selectionPassed = false;
    bool holdoutPassed = false;
    bool brepCheckValid = false;
    bool hybridParameterizationUsed = false;
    int stlReferenceSampleCount = 0;
    double maxStepToStlDistance = 0.0;
};

struct RestrictedBSplinePatchFitResult {
    TopoDS_Face face;
    Handle(Geom_BSplineSurface) surface;
    RestrictedBSplinePatchFitReport report;

    bool success() const {
        return report.success && !face.IsNull() && !surface.IsNull();
    }
};

class RestrictedBSplinePatchFitter {
public:
    RestrictedBSplinePatchFitResult fit(
        const ShapeDocument& document,
        const std::vector<FaceId>& sourceFaces,
        const RestrictedBSplinePatchFitOptions& options = {}) const;
};

}
