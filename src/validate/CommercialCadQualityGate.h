#pragma once

#include "common/GeometryTypes.h"

#include <TopoDS_Shape.hxx>

#include <string>

namespace spo {

class ShapeDocument;
struct FeatureEdgeDetectionResult;
struct MergeCandidate;
struct RegionBoundaryAnalysis;

struct CommercialCadDistanceStats {
    bool evaluated = false;
    int samples = 0;
    int overTolerance = 0;
    double tolerance = 0.0;
    double maxDistance = 0.0;
    double meanDistance = 0.0;
    double rmsDistance = 0.0;
    double p95Distance = 0.0;
};

struct CommercialCadSeamContinuityStats {
    bool evaluated = false;
    int samples = 0;
    int overTolerance = 0;
    double tolerance = 0.0;
    double maxSignedNormalOffset = 0.0;
    double maxAbsSignedNormalOffset = 0.0;
    double meanAbsSignedNormalOffset = 0.0;
    double rmsAbsSignedNormalOffset = 0.0;
    double p95AbsSignedNormalOffset = 0.0;
};

struct CommercialCadSamplingReport {
    int boundarySamplesPerEdge = 0;
    int featureEdgeSamplesPerEdge = 0;
    double anchorDedupTolerance = 0.0;
    std::string cornerAnchorSource;

    int boundaryEdgesSampled = 0;
    int boundarySampleCount = 0;
    int cornerAnchorCount = 0;
    bool featureEdgeResultAvailable = false;
    int featureBoundaryEdgesSampled = 0;
    int featureEdgeSampleCount = 0;
};

struct CommercialCadQualityGateOptions {
    int boundarySamplesPerEdge = 64;
    int featureEdgeSamplesPerEdge = 64;
    double maxBoundaryDistance = 0.03;
    double maxCornerAnchorDistance = 0.02;
    double maxFeatureEdgeDistance = 0.03;
    double maxSeamNormalOffset = 0.03;
    double anchorDedupTolerance = 1.0e-7;
};

struct CommercialCadQualityGateInput {
    const ShapeDocument* document = nullptr;
    const MergeCandidate* candidate = nullptr;
    const RegionBoundaryAnalysis* boundary = nullptr;
    const FeatureEdgeDetectionResult* featureEdges = nullptr;
    const TopoDS_Shape* patchShape = nullptr;
    CommercialCadQualityGateOptions options;
};

struct CommercialCadQualityGateReport {
    bool evaluated = false;
    bool passed = false;
    bool sharpCornerPreservationPassed = false;

    int candidateId = -1;
    int boundaryEdgeCount = 0;
    int featureBoundaryEdgeCount = 0;

    CommercialCadSamplingReport sampling;
    CommercialCadDistanceStats boundary;
    CommercialCadSeamContinuityStats seamContinuity;
    CommercialCadDistanceStats cornerAnchors;
    CommercialCadDistanceStats featureEdges;

    std::string message;
    std::string warningMessage;
};

class CommercialCadQualityGate {
public:
    CommercialCadQualityGateReport evaluate(const CommercialCadQualityGateInput& input) const;
};

std::string toJson(const CommercialCadQualityGateReport& report);

}
