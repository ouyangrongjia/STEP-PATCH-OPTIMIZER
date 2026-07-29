#pragma once

#include "reduction/region_refit/RegionReferenceMesh.h"
#include "reduction/region_refit/RegionalSurfaceRefitTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spo {

struct RegionCurveNetworkGenerationResult;
struct RegionCurveNetwork;

struct RegionBoundaryChain {
    int adjacentPatch = -1;
    bool originalBoundary = false;
    std::vector<std::size_t> vertices;
    std::vector<EdgeId> originalEdgeIds;
};

struct RegionLayoutSeam {
    int firstPatch = -1;
    int secondPatch = -1;
    std::vector<std::array<std::size_t, 2>> meshEdges;
};

enum class RegionPatchIsoSide {
    VMin = 0,
    UMax = 1,
    VMax = 2,
    UMin = 3
};

struct RegionPatchLogicalSide {
    RegionPatchIsoSide isoSide = RegionPatchIsoSide::VMin;
    std::vector<std::size_t> edgeUseOrders;
    double curveLength = 0.0;
};

struct RegionPatchEdgePcurve {
    std::size_t edgeIndex = 0;
    bool forward = true;
    double firstParameter = 0.0;
    double lastParameter = 0.0;
    std::size_t startNode = 0;
    std::size_t endNode = 0;
    std::size_t loopOrder = 0;
    RegionPatchIsoSide isoSide = RegionPatchIsoSide::VMin;
    double sideFirstParameter = 0.0;
    double sideLastParameter = 1.0;
    std::vector<double> parameters;
    std::vector<std::array<double, 2>> uvPoints;
    std::string hash;
};

struct RegionPatchParameterization {
    std::vector<std::size_t> triangleIndices;
    std::vector<std::size_t> vertexIndices;
    // Strictly convex carrier coordinates used only for injective triangle
    // location and layout-quality diagnostics.
    std::vector<std::array<double, 2>> parameters;
    // Rectangular surface coordinates whose boundary is exactly the four
    // logical iso sides used by fitting and B-rep pcurves.
    std::vector<std::array<double, 2>> surfaceParameters;
    std::vector<std::size_t> boundaryLoop;
    // Retained only for the legacy mesh-seam fitting path. Frozen curve-network
    // fitting and B-rep construction use edgePcurves as their single UV truth.
    std::array<std::vector<std::size_t>, 4> boundarySides;
    std::array<RegionPatchLogicalSide, 4> logicalSides;
    std::vector<RegionBoundaryChain> boundaryChains;
    std::vector<RegionPatchEdgePcurve> edgePcurves;
    std::vector<std::size_t> trimCorners;
    std::string domainType = "TrimmedDiskConvexBoundary";
    bool rectangularIsoBoundary = false;
    bool diskTopologyPassed = false;
    bool pcurveLoopClosed = false;
    bool pcurveOrientationConsistent = false;
    int pcurveSelfIntersectionCount = 0;
    int pcurveNonAdjacentIntersectionCount = 0;
    std::size_t boundaryChainCount = 0;
    int trimCornerCount = 0;
    std::size_t matrixRows = 0;
    std::size_t matrixNonZeros = 0;
    int flippedTriangleCount = 0;
    double minimumSignedUvArea = 0.0;
    double minimumToMedianUvAreaRatio = 0.0;
    double distortionP995 = 0.0;
    double boundaryRepresentabilityResidual = 0.0;
    double maximumDistortion = 0.0;
    std::size_t logicalSideCandidateCount = 0;
    std::string selectedLogicalSideHash;
};

struct RegionPatchLayout {
    int patchCount = 0;
    std::vector<int> trianglePatches;
    std::vector<RegionPatchParameterization> patches;
    std::vector<RegionLayoutSeam> seams;
    double proxyError = 0.0;
    std::uint64_t estimatedSolverBytes = 0;
    std::uint64_t estimatedOutputEntities = 0;
    std::string hash;
    std::string curveNetworkHash;
};

struct RegionPatchLayoutGenerationResult {
    RegionRefitStatus status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
    std::string message;
    std::vector<RegionLayoutCandidateReport> candidates;
    std::vector<RegionPatchLayout> constructibleCandidates;
    std::vector<RegionPatchLayout> selectedCandidates;

    bool success() const;
};

class RegionPatchLayoutGenerator {
public:
    RegionPatchLayoutGenerationResult generate(
        const RegionReferenceMesh& mesh,
        RegionRefitLayoutMode mode,
        const RegionRefitOptions& options,
        bool allowDiagnosticCandidates = false) const;

    RegionPatchLayoutGenerationResult generate(
        const RegionReferenceMesh& mesh,
        const RegionCurveNetworkGenerationResult& curveNetworks,
        const RegionRefitOptions& options) const;
};

std::optional<std::array<double, 2>> evaluateRegionPatchEdgePcurve(
    const RegionPatchEdgePcurve& pcurve,
    double parameter);

bool buildCurveConsistentParameterDomain(
    const RegionReferenceMesh& mesh,
    const RegionCurveNetwork& network,
    const RegionRefitOptions& options,
    RegionPatchLayout& layout,
    std::string& message);

}
