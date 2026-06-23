#pragma once

#include "common/GeometryTypes.h"
#include "merge/RegionBoundaryAnalyzer.h"

#include <TopoDS_Face.hxx>
#include <Geom_Surface.hxx>
#include <gp_Pnt.hxx>

#include <string>
#include <vector>

namespace spo {

class ShapeDocument;

struct BoundaryConstrainedSurfaceRetrimOptions {
    int samplesPerEdge = 5;
    double projectionTolerance = 0.2;
    bool rebuildBoundaryPcurves = false;
};

struct BoundarySurfaceSample {
    EdgeId edgeId = 0;
    double parameter = 0.0;
    gp_Pnt point;
};

struct BoundarySurfaceCoverageReport {
    int boundarySampleCount = 0;
    int projectedSampleCount = 0;
    int failedProjectionCount = 0;
    double maxProjectionDistance = 0.0;
    double averageProjectionDistance = 0.0;
    std::vector<EdgeId> uncoveredEdgeIds;
};

struct BoundaryConstrainedSurfaceCandidateReport {
    int patchFaceIndex = -1;
    int boundarySampleCount = 0;
    int projectedSampleCount = 0;
    int failedProjectionCount = 0;
    double maxProjectionDistance = 0.0;
    double averageProjectionDistance = 0.0;
    bool accepted = false;
};

struct BoundaryConstrainedSurfaceRetrimResult {
    bool success = false;
    TopoDS_Face replacementFace;
    int selectedPatchFaceIndex = -1;
    int boundarySampleCount = 0;
    int projectedSampleCount = 0;
    int failedProjectionCount = 0;
    double maxProjectionDistance = 0.0;
    double averageProjectionDistance = 0.0;
    std::vector<BoundaryConstrainedSurfaceCandidateReport> candidates;
    int surfaceCoverageProjectedSampleCount = 0;
    int surfaceCoverageFailedProjectionCount = 0;
    double surfaceCoverageMaxProjectionDistance = 0.0;
    double surfaceCoverageAverageProjectionDistance = 0.0;
    std::vector<EdgeId> surfaceCoverageUncoveredEdgeIds;
    int boundaryEdgePcurveRebuildAttemptCount = 0;
    int boundaryEdgePcurveRebuildSuccessCount = 0;
    int boundaryEdgePcurveRebuildFailureCount = 0;
    int boundaryEdgeSameParameterCheckCount = 0;
    int boundaryEdgeSameParameterFailureCount = 0;
    double boundaryEdgeMaxSameParameterDeviation = 0.0;
    std::vector<EdgeId> boundaryEdgePcurveRebuildFailedEdgeIds;
    std::vector<EdgeId> boundaryEdgeSameParameterFailedEdgeIds;
    std::string message;
    std::string warningMessage;
};

class BoundaryConstrainedSurfaceRetrim {
public:
    BoundaryConstrainedSurfaceRetrimResult retrim(
        const ShapeDocument& document,
        const RegionBoundaryAnalysis& boundary,
        const std::vector<TopoDS_Face>& patchFaces,
        const TopoDS_Face& sourceOrientationFace,
        const BoundaryConstrainedSurfaceRetrimOptions& options = {}) const;
};

std::vector<BoundarySurfaceSample> sampleBoundaryForSurfaceProjection(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& boundary,
    int samplesPerEdge);

double projectionDistanceToSurface(
    const gp_Pnt& point,
    const Handle(Geom_Surface)& surface);

BoundarySurfaceCoverageReport evaluateBoundarySurfaceCoverage(
    const std::vector<TopoDS_Face>& faces,
    const std::vector<BoundarySurfaceSample>& samples,
    double projectionTolerance);

}
