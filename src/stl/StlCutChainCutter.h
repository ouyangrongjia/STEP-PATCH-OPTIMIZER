#pragma once

#include "stl/StlMesh.h"

#include <gp_Pnt.hxx>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace spo {

// ============================================================================
// Options
// ============================================================================

struct StlCutChainOptions {
    int    samplesPerEdge         = 120;
    double joinTolerance          = 1e-5;
    double weldTolerance          = 1e-6;
    double maxProjectDist         = 0.5;
    double greenResampleFactor    = 0.35;   // × avg STL edge length
    int    maxFaceWalkVisits      = 6000;
    bool   snapBoundaryToRed      = true;
    double snapMaxDist            = 0.2;
    int    minComponentFaceCount  = 10;
    double minComponentAreaRatio  = 1e-8;
    int    noSeedMinComponentFace = 50;
    double noSeedMinAreaRatio     = 1e-5;
    double maxPatchFaceRatioNoSeed = 0.80;
    double maxPatchAreaRatioNoSeed = 0.80;
    double maxPatchFaceRatioWithSeed = 0.95;
    double maxPatchAreaRatioWithSeed = 0.95;
    bool   keepSmallerIfNoSeed   = true;
    double vertexMergeTolerance   = 1e-8;
    double local2DTolerance       = 1e-8;
    double edgeParameterTolerance = 1e-7;
};

// ============================================================================
// Result
// ============================================================================

struct StlCutChainResult {
    bool   success = false;
    StlMesh patchMesh;
    std::string message;

    // Statistics
    int originalFaces         = 0;
    int cutVertexCount        = 0;
    int chainEdgeCount        = 0;
    int facesWithConstraints  = 0;
    int triangulationFailed   = 0;
    int missingConstraintEdges= 0;
    int componentCount        = 0;
    int patchFaceCount        = 0;
    int splitMeshVertexCount  = 0;
    int splitMeshFaceCount    = 0;
    std::vector<int> cutEdgeDegrees;
};

// ============================================================================
// Core cutter class
// ============================================================================

class StlCutChainCutter {
public:
    // ---- types (public for template parameters in method signatures) ----
    using FaceTriple = std::array<std::int64_t, 3>;
    using EdgePair   = std::pair<std::int64_t, std::int64_t>;
    struct EdgeHash {
        std::size_t operator()(const EdgePair& e) const noexcept {
            return static_cast<std::size_t>(e.first * 1315423911ULL) ^
                   static_cast<std::size_t>(e.second * 2654435761ULL);
        }
    };

    /// Main entry point.
    /// @param sourceMesh      source STL mesh (will be copied internally)
    /// @param boundaryLoop    ordered 3D boundary points (closed loop, from
    ///                        STEP candidate outer-boundary wire)
    /// @param seedPoints      optional seed points inside the desired patch
    StlCutChainResult cut(
        const StlMesh&            sourceMesh,
        const std::vector<gp_Pnt>& boundaryLoop,
        const std::optional<std::vector<gp_Pnt>>& seedPoints = {},
        const StlCutChainOptions& options = {}) const;

    struct MeshTopology {
        std::vector<std::array<double, 3>> vertices;     // shared vertices
        std::vector<FaceTriple>            faces;         // vertex indices
        std::vector<std::array<double, 3>> centers;       // face centroids
        std::vector<std::vector<std::int64_t>> neighbors; // face adjacency
        std::unordered_map<EdgePair, std::vector<std::int64_t>, EdgeHash> edgeToFaces;
        double avgEdgeLength = 0.0;
    };

private:

    struct CutPoint {
        std::array<double, 3> xyz;
        std::int64_t          stableKey = -1;
    };

    struct FaceConstraint {
        std::int64_t cutIdA = -1;
        std::int64_t cutIdB = -1;
    };

public:
    // ---- helpers (public for testing) ----

    static MeshTopology buildTopology(const StlMesh& mesh);

    // Geometry utilities
    static std::array<double, 3> triangleCentroid(
        const std::array<double, 3>& a,
        const std::array<double, 3>& b,
        const std::array<double, 3>& c);
    static double triangleArea(
        const std::array<double, 3>& a,
        const std::array<double, 3>& b,
        const std::array<double, 3>& c);
    static double pointSegmentDistance(
        const std::array<double, 3>& p,
        const std::array<double, 3>& a,
        const std::array<double, 3>& b);
    static double polylineLength(const std::vector<std::array<double, 3>>& pts);

    // Sampling & projection
    static std::vector<std::array<double, 3>> resamplePolyline(
        const std::vector<std::array<double, 3>>& points,
        double maxStep);
    static std::vector<std::array<double, 3>> sampleBoundaryLoop(
        const std::vector<gp_Pnt>& loop,
        int samplesPerEdge);
    static void projectToStl(
        const MeshTopology& topo,
        const std::vector<std::array<double, 3>>& redPoints,
        std::vector<std::array<double, 3>>& greenPoints,
        std::vector<std::int64_t>& triIds,
        std::vector<double>& distances);

    // Face-path tracing (A*)
    std::vector<std::int64_t> facePath(
        const MeshTopology& topo,
        std::int64_t startFace,
        std::int64_t endFace,
        const std::array<double, 3>& p0,
        const std::array<double, 3>& p1) const;

    // Build global cut-chain by tracing green boundary through STL faces
    void traceBoundary(
        const MeshTopology& topo,
        const std::vector<std::array<double, 3>>& greenPoints,
        const std::vector<std::int64_t>& triIds,
        std::vector<CutPoint>& cutVertices,
        std::unordered_map<std::int64_t, std::vector<FaceConstraint>>& faceConstraints,
        std::vector<EdgePair>& chainEdges) const;

    // Per-face 2D constrained re-triangulation + global face output
    StlMesh buildSplitMesh(
        const MeshTopology& topo,
        const std::vector<CutPoint>& cutVertices,
        const std::unordered_map<std::int64_t, std::vector<FaceConstraint>>& faceConstraints,
        std::unordered_set<EdgePair, EdgeHash>& cutEdgesOut,
        int& triangulationFailed,
        int& missingConstraints) const;

    // Flood-fill connected components, select the patch
    StlMesh floodFillPatch(
        const StlMesh& splitMesh,
        const std::unordered_set<EdgePair, EdgeHash>& cutEdges,
        const std::optional<std::vector<gp_Pnt>>& seedPoints,
        const StlCutChainOptions& options) const;

    // Snap patch outer boundary back to the original red loop
    static StlMesh snapBoundary(
        const StlMesh& patch,
        const std::vector<std::array<double, 3>>& green,
        const std::vector<std::array<double, 3>>& red,
        double maxDist);

    // Diagnostics
    static std::vector<int> computeCutEdgeDegrees(
        const std::unordered_set<EdgePair, EdgeHash>& cutEdges);
};

}
