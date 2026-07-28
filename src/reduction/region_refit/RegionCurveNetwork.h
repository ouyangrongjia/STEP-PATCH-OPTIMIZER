#pragma once

#include "reduction/region_refit/RegionPatchLayout.h"

#include <Geom_Curve.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Pnt.hxx>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spo {

enum class RegionCurveContinuity {
    G0,
    G1,
    G2
};

const char* toString(RegionCurveContinuity continuity);

enum class RegionInternalCurveConstructionMode {
    RegularizedCubic,
    DiagnosticPolyline
};

const char* toString(RegionInternalCurveConstructionMode mode);

struct RegionCurveNetworkNode {
    std::size_t id = 0;
    std::size_t meshVertex = 0;
    gp_Pnt point;
    bool originalBoundary = false;
    std::vector<std::size_t> incidentEdges;
};

struct RegionCurveNetworkEdge {
    std::size_t id = 0;
    std::size_t startNode = 0;
    std::size_t endNode = 0;
    int firstPatch = -1;
    int secondPatch = -1;
    RegionCurveContinuity requestedContinuity = RegionCurveContinuity::G0;
    RegionCurveContinuity continuity = RegionCurveContinuity::G0;
    RegionInternalCurveConstructionMode constructionMode =
        RegionInternalCurveConstructionMode::RegularizedCubic;
    bool productQualityPassed = true;
    int validationSampleCount = 0;
    int controlPointCount = 0;
    double maximumSampleTurnDegrees = 0.0;
    double bendingEnergy = 0.0;
    int selfIntersectionCount = 0;
    bool originalBoundary = false;
    bool hasOriginalEdge = false;
    EdgeId originalEdgeId = 0;
    TopoDS_Edge originalEdge;
    Handle(Geom_Curve) curve;
    double firstParameter = 0.0;
    double lastParameter = 0.0;
    std::vector<std::size_t> meshVertices;
    std::vector<double> meshParameters;
    bool reusesOriginalEdge = false;
    bool splitsOriginalEdge = false;
    bool frozen = false;
    double maximumProjectionError = 0.0;
};

struct RegionCurveNetworkEdgeUse {
    std::size_t edgeIndex = 0;
    bool forward = true;
};

struct RegionCurveNetworkPatchLoop {
    int patch = -1;
    std::vector<RegionCurveNetworkEdgeUse> edges;
};

struct RegionCurveNetworkReport {
    bool finitePassed = false;
    bool closurePassed = false;
    bool ownershipPassed = false;
    bool projectionPassed = false;
    bool orientationPassed = false;
    bool intersectionPassed = false;
    bool nonManifoldEdgePassed = false;
    bool frozen = false;
    std::size_t nodeCount = 0;
    std::size_t edgeCount = 0;
    std::size_t internalEdgeCount = 0;
    std::size_t boundaryEdgeCount = 0;
    std::size_t g0EdgeCount = 0;
    std::size_t g1EdgeCount = 0;
    std::size_t g2EdgeCount = 0;
    std::size_t regularizedCubicEdgeCount = 0;
    std::size_t diagnosticPolylineEdgeCount = 0;
    std::size_t continuityDowngradeCount = 0;
    double maximumProjectionError = 0.0;
    double maximumSampleTurnDegrees = 0.0;
    double maximumBendingEnergy = 0.0;
    int selfIntersectionCount = 0;
    int unexpectedCurveIntersectionCount = 0;
    bool productQualityPassed = false;
    std::string rejectionReason;
};

struct RegionCurveNetwork {
    int patchCount = 0;
    std::vector<int> trianglePatches;
    std::vector<RegionCurveNetworkNode> nodes;
    std::vector<RegionCurveNetworkEdge> edges;
    std::vector<RegionCurveNetworkPatchLoop> patchLoops;
    RegionCurveNetworkReport report;
    std::string partitionLayoutHash;
    std::string hash;

    bool valid() const;
};

struct RegionCurveNetworkCandidate {
    RegionCurveNetwork network;
    RegionPatchLayout partitionLayout;
};

struct RegionCurveNetworkGenerationResult {
    RegionRefitStatus status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
    std::string message;
    std::vector<RegionLayoutCandidateReport> layoutReports;
    std::vector<RegionCurveNetworkCandidate> constructibleCandidates;
    std::vector<RegionCurveNetworkCandidate> selectedCandidates;

    bool success() const;
};

bool validateRegionCurveNetwork(
    RegionCurveNetwork& network,
    double projectionTolerance,
    int validationSampleCount,
    std::string& message);

class RegionCurveNetworkGenerator {
public:
    RegionCurveNetworkGenerationResult generate(
        const RegionReferenceMesh& mesh,
        const RegionTopologyConstraints& constraints,
        RegionRefitLayoutMode mode,
        const RegionRefitOptions& options,
        bool allowDiagnosticCandidates = false) const;
};

}
