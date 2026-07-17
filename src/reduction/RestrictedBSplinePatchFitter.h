#pragma once

#include "brep/ShapeDocument.h"
#include "common/GeometryTypes.h"
#include "reduction/StlHybridParameterization.h"
#include "stl/StlMesh.h"

#include <Geom_BSplineSurface.hxx>
#include <TopoDS_Face.hxx>

#include <string>
#include <vector>

namespace spo {

class StlRegionIndex;

enum class RestrictedBSplinePatchFitFailureStage {
    None,
    Parameterization,
    BoundaryConstraints,
    UvIteration,
    ModelSelection,
    Holdout,
    Jacobian,
    Retrim,
    BoundaryHoldout,
    BrepCheck
};

const char* toString(RestrictedBSplinePatchFitFailureStage stage);

struct UvIterationReport {
    int iteration = 0;
    double objectiveBefore = 0.0;
    double objectiveAfter = 0.0;
    double maximumShift = 0.0;
    int backtrackingSteps = 0;
    bool accepted = false;
    std::string reason;
};

struct RestrictedBSplinePatchFitOptions {
    double tolerance3d = 0.03;
    double normalToleranceDegrees = 15.0;
    double minimumNormalizedJacobian = 1.0e-5;
    double fairnessWeight = 0.01;
    int maximumControlPoints = 1024;
    int maximumBoundaryConstraintRefinements = 8;
    int maximumUvIterations = 5;
    int maximumUvBacktrackingSteps = 8;
    double uvConvergenceTolerance = 1.0e-8;
    const StlMesh* scanReference = nullptr;
    const StlRegionIndex* scanRegionIndex = nullptr;
    ScanReferenceCoverage scanReferenceCoverage = ScanReferenceCoverage::None;
    double scanReferenceWeight = 0.25;
    double maximumScanReferenceDistance = 0.2;
};

struct RestrictedBSplinePatchFitTiming {
    double topologyPreparationMs = 0.0;
    double scanCropMs = 0.0;
    double samplingMs = 0.0;
    double parameterizationMs = 0.0;
    double kktSolveMs = 0.0;
    double uvProjectionMs = 0.0;
    double boundaryRefinementMs = 0.0;
    double validationMs = 0.0;
    double retrimMs = 0.0;
    double brepCheckMs = 0.0;
    double totalMs = 0.0;
};

struct RestrictedBSplinePatchFitReport {
    bool success = false;
    RestrictedBSplinePatchFitFailureStage failureStage =
        RestrictedBSplinePatchFitFailureStage::None;
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
    int boundaryConstraintCount = 0;
    int boundaryConstraintRefinementCount = 0;
    int exteriorGuardSampleCount = 0;
    double maxBoundaryConstraintResidual = 0.0;
    double maxBoundaryUvShift = 0.0;
    double maxBoundaryDistance = 0.0;
    double supportSurfaceExtentRatio = 0.0;
    double supportSurfaceMaxPoleOverflowRatio = 0.0;
    double maxSelectionDistance = 0.0;
    double maxSelectionNormalAngleDegrees = 0.0;
    double maxHoldoutDistance = 0.0;
    double maxHoldoutNormalAngleDegrees = 0.0;
    double minimumNormalizedJacobian = 0.0;
    bool parameterizationValid = false;
    bool selectionPassed = false;
    bool holdoutPassed = false;
    bool brepCheckValid = false;
    SurfaceParameterizationSource parameterizationSource = SurfaceParameterizationSource::None;
    StlHybridParameterizationReport parameterization;
    ScanReferenceCoverage scanReferenceCoverage = ScanReferenceCoverage::None;
    int scanReferenceSampleCount = 0;
    double maxStepToScanDistance = 0.0;
    int kktSolveCount = 0;
    int boundaryProjectionCount = 0;
    int scanIndexCandidateTriangleCount = 0;
    RestrictedBSplinePatchFitTiming timing;
    std::vector<UvIterationReport> uvIterations;
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
