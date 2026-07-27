#pragma once

#include "reduction/region_refit/RegionReferenceMesh.h"
#include "reduction/region_refit/RegionalSurfaceRefitTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spo {

struct RegionCurveNetworkGenerationResult;

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

struct RegionPatchParameterization {
    std::vector<std::size_t> triangleIndices;
    std::vector<std::size_t> vertexIndices;
    std::vector<std::array<double, 2>> parameters;
    std::vector<std::size_t> boundaryLoop;
    std::array<std::vector<std::size_t>, 4> boundarySides;
    std::vector<RegionBoundaryChain> boundaryChains;
    std::vector<std::size_t> trimCorners;
    std::string domainType = "TrimmedDiskConvexBoundary";
    bool diskTopologyPassed = false;
    std::size_t boundaryChainCount = 0;
    int trimCornerCount = 0;
    std::size_t matrixRows = 0;
    std::size_t matrixNonZeros = 0;
    int flippedTriangleCount = 0;
    double minimumSignedUvArea = 0.0;
    double maximumDistortion = 0.0;
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

}
