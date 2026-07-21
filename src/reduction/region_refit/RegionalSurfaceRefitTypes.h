#pragma once

#include "common/GeometryTypes.h"

#include <Geom2d_Curve.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace spo {

enum class RegionRefitLayoutMode {
    FixedPatchCount,
    Adaptive
};

enum class RegionRefitStatus {
    NotStarted,
    Success,
    InvalidRequest,
    UnsupportedTopology,
    TimeBudgetExceeded,
    MemoryBudgetExceeded,
    SamplingBudgetExceeded,
    NoValidatedLayoutWithinBudget,
    Failed
};

enum class RegionRefitStage {
    None,
    Constraints,
    Sampling,
    Layout,
    Fitting,
    Retrim,
    Sewing,
    StrictTopology,
    StepRoundtrip,
    ExternalCad
};

enum class RegionBoundaryContinuity {
    Free,
    HardCrease,
    Smooth,
    Locked
};

enum class RegionRefitCachePolicy {
    Disabled,
    ReadWrite,
    ReadOnly,
    Refresh
};

const char* toString(RegionRefitLayoutMode mode);
const char* toString(RegionRefitStatus status);
const char* toString(RegionRefitStage stage);
const char* toString(RegionBoundaryContinuity continuity);
const char* toString(RegionRefitCachePolicy policy);

struct RegionRefitOptions {
    double tolerance3d = 0.001;
    double normalToleranceDegrees = 2.0;
    double curveOnSurfaceTolerance = 0.001;
    std::size_t maximumSampleTriangles = 200000;
    int fixedPatchCount = 0;
    int maximumAdaptivePatchCount = 12;
    int maximumFullFitCount = 3;
    int maximumFitIterations = 20;
    std::int64_t maximumWallClockMilliseconds = 900000;
    std::int64_t inputReadBudgetMilliseconds = 60000;
    std::int64_t constraintsBudgetMilliseconds = 10000;
    std::int64_t samplingBudgetMilliseconds = 90000;
    std::int64_t layoutBudgetMilliseconds = 60000;
    std::int64_t singleFitBudgetMilliseconds = 180000;
    std::int64_t totalFitBudgetMilliseconds = 450000;
    std::int64_t retrimBudgetMilliseconds = 90000;
    std::int64_t finalValidationBudgetMilliseconds = 180000;
    std::size_t maximumMemoryMiB = 8192;
    std::size_t solverMemoryPreflightMiB = 7168;
    RegionRefitCachePolicy cachePolicy = RegionRefitCachePolicy::Disabled;
    std::filesystem::path cacheDirectory;
    std::string algorithmFingerprint;
};

struct RegionRefitRequest {
    std::vector<FaceId> selectedFaces;
    std::vector<EdgeId> lockedEdges;
    RegionRefitLayoutMode layoutMode = RegionRefitLayoutMode::Adaptive;
    RegionRefitOptions options;
};

struct RegionBoundaryEdgeConstraint {
    EdgeId edgeId = 0;
    FaceId selectedFaceId = 0;
    std::optional<FaceId> adjacentFaceId;
    TopoDS_Edge originalEdge;
    TopAbs_Orientation loopOrientation = TopAbs_FORWARD;
    TopAbs_Orientation selectedFaceOrientation = TopAbs_FORWARD;
    TopAbs_Orientation adjacentFaceOrientation = TopAbs_FORWARD;
    Handle(Geom2d_Curve) selectedPcurve;
    Handle(Geom2d_Curve) adjacentPcurve;
    double selectedFirstParameter = 0.0;
    double selectedLastParameter = 0.0;
    double adjacentFirstParameter = 0.0;
    double adjacentLastParameter = 0.0;
    RegionBoundaryContinuity continuity = RegionBoundaryContinuity::HardCrease;
    bool hardPositionConstraint = true;
    bool softAdjacentNormalConstraint = false;
};

struct RegionTopologyConstraints {
    std::vector<FaceId> selectedFaces;
    std::vector<EdgeId> internalEdges;
    std::vector<EdgeId> lockedInternalEdges;
    std::vector<EdgeId> orderedBoundaryEdges;
    std::vector<RegionBoundaryEdgeConstraint> boundaryEdges;
    TopoDS_Wire outerWire;
};

struct RegionRefitStageReport {
    RegionRefitStage stage = RegionRefitStage::None;
    bool success = false;
    double wallMilliseconds = 0.0;
    double cpuMilliseconds = 0.0;
    std::uint64_t peakWorkingSetBytes = 0;
    std::uint64_t peakPrivateBytes = 0;
    std::string message;
};

struct RegionRefitReport {
    RegionRefitStatus status = RegionRefitStatus::NotStarted;
    RegionRefitStage failureStage = RegionRefitStage::None;
    std::string message;
    int inputFaceCount = 0;
    int outputFaceCount = 0;
    int selectedPatchCount = 0;
    int fullFitCount = 0;
    std::string selectedLayoutHash;
    double totalWallMilliseconds = 0.0;
    std::uint64_t peakWorkingSetBytes = 0;
    std::uint64_t peakPrivateBytes = 0;
    std::vector<RegionRefitStageReport> stages;
};

struct RegionRefitResult {
    RegionRefitReport report;
    RegionTopologyConstraints constraints;
    TopoDS_Shape fittedPatchNetwork;
    TopoDS_Shape finalShape;

    bool canApply() const;
};

}
