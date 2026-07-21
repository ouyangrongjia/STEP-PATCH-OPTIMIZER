#pragma once

#include "reduction/region_refit/RegionReferenceMesh.h"
#include "reduction/region_refit/RegionalSurfaceRefitTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spo {

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
    std::size_t matrixRows = 0;
    std::size_t matrixNonZeros = 0;
    int flippedTriangleCount = 0;
    double maximumDistortion = 0.0;
};

struct RegionPatchLayout {
    int patchCount = 0;
    std::vector<int> trianglePatches;
    std::vector<RegionPatchParameterization> patches;
    std::vector<RegionLayoutSeam> seams;
    double proxyError = 0.0;
    std::uint64_t estimatedSolverBytes = 0;
    std::string hash;
};

struct RegionPatchLayoutGenerationResult {
    RegionRefitStatus status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
    std::string message;
    std::vector<RegionLayoutCandidateReport> candidates;
    std::vector<RegionPatchLayout> selectedCandidates;

    bool success() const;
};

class RegionPatchLayoutGenerator {
public:
    RegionPatchLayoutGenerationResult generate(
        const RegionReferenceMesh& mesh,
        RegionRefitLayoutMode mode,
        const RegionRefitOptions& options) const;
};

}
