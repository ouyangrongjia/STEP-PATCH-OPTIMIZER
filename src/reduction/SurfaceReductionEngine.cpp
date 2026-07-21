#include "reduction/SurfaceReductionEngine.h"

#include "feature/FeatureEdgeDetector.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/PatchReplacementRepair.h"
#include "reduction/RestrictedBSplinePatchFitter.h"
#include "reduction/SurfaceReductionFitCache.h"
#include "reduction/StlHybridParameterization.h"
#include "stl/StlRegionIndex.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepTools_ReShape.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <tuple>
#include <utility>

namespace spo {

namespace {

class ScopedMilliseconds {
public:
    explicit ScopedMilliseconds(double& target)
        : target_(target), started_(std::chrono::steady_clock::now()) {}
    ~ScopedMilliseconds() {
        target_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started_).count();
    }
private:
    double& target_;
    std::chrono::steady_clock::time_point started_;
};

class SurfaceReductionReportFinalizer {
public:
    explicit SurfaceReductionReportFinalizer(SurfaceReductionReport& report)
        : report_(report), started_(std::chrono::steady_clock::now()) {}
    ~SurfaceReductionReportFinalizer() {
        report_.timing.totalMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started_).count();
        std::vector<double> durations;
        durations.reserve(report_.fitProfiles.size());
        for (const auto& profile : report_.fitProfiles) {
            durations.push_back(profile.totalMs);
        }
        if (durations.empty()) return;
        std::sort(durations.begin(), durations.end());
        const auto percentile = [&](double value) {
            const auto index = static_cast<std::size_t>(std::ceil(
                value * static_cast<double>(durations.size()))) - 1;
            return durations[std::min(index, durations.size() - 1)];
        };
        report_.fitDurationP50Ms = percentile(0.50);
        report_.fitDurationP95Ms = percentile(0.95);
        report_.fitDurationP99Ms = percentile(0.99);
    }
private:
    SurfaceReductionReport& report_;
    std::chrono::steady_clock::time_point started_;
};

void publishProgress(
    const SurfaceReductionExecutionOptions& execution,
    SurfaceReductionProgressStage stage,
    const SurfaceReductionReport& report,
    int targetFaceCount,
    int currentFaceCount,
    int maximumFitCount,
    std::string message,
    int searchIteration = 0,
    std::vector<FaceId> activeGroup = {},
    int candidateValidationIndex = 0,
    int candidateValidationCount = 0) {
    if (!execution.progressCallback) {
        return;
    }
    SurfaceReductionProgress progress;
    progress.stage = stage;
    progress.searchObjective = report.searchObjective;
    progress.inputFaceCount = report.inputFaceCount;
    progress.targetFaceCount = targetFaceCount;
    progress.theoreticalMinimumFaceCount = report.theoreticalMinimumFaceCount;
    progress.currentFaceCount = currentFaceCount;
    progress.searchIteration = searchIteration;
    progress.attemptedMergeCount = report.attemptedMergeCount;
    progress.completedFitCount = report.fitCacheMissCount;
    progress.maximumFitCount = maximumFitCount;
    progress.candidateValidationIndex = candidateValidationIndex;
    progress.candidateValidationCount = candidateValidationCount;
    progress.activeGroup = std::move(activeGroup);
    progress.message = std::move(message);
    execution.progressCallback(progress);
}

struct Cluster {
    std::vector<FaceId> faces;
    TopoDS_Face replacementFace;
    Handle(Geom_Surface) supportSurface;
    RestrictedBSplinePatchFitReport fitReport;
    SurfaceReductionMergeMethod method = SurfaceReductionMergeMethod::OriginalFace;
};

enum class PairProposalType {
    ClusterCluster,
    ClusterSingleton,
    SingletonSingleton
};

struct PairCandidate {
    int first = -1;
    int second = -1;
    double normalJump = 0.0;
    double curvatureDifference = 0.0;
    double sharedLength = 0.0;
    FaceId stableFace = 0;
    int combinedOriginalFaceCount = 0;
    PairProposalType type = PairProposalType::SingletonSingleton;
    gp_Pnt centroid;
};

struct PreparedSelectionEdge {
    EdgeId edgeId = 0;
    FaceId firstFace = 0;
    FaceId secondFace = 0;
    double normalJump = 0.0;
    double curvatureDifference = 0.0;
    double length = 0.0;
};

struct SurfaceReductionPreparedContext {
    std::vector<PreparedSelectionEdge> internalEdges;
    std::map<FaceId, gp_Pnt> faceCentroids;
    std::map<FaceId, double> faceAreas;
    mutable std::map<
        std::tuple<EdgeId, std::vector<FaceId>, std::vector<FaceId>>,
        double> seamAngleCache;
};

struct SeamQuality {
    bool valid = true;
    double maximumAngle = 0.0;
};

struct SearchState {
    std::vector<Cluster> clusters;
    std::map<EdgeId, double> seamAngles;
    double maximumSeamAngle = 0.0;
};

struct CachedFit {
    bool success = false;
    TopoDS_Face face;
    Handle(Geom_Surface) surface;
    RestrictedBSplinePatchFitReport report;
    SurfaceReductionMergeMethod method = SurfaceReductionMergeMethod::RestrictedBSpline;
};

struct FinalValidationResult {
    bool passed = false;
    TopoDS_Shape reducedShape;
    TopoDS_Shape previewShape;
    std::vector<SurfaceReductionReplacement> replacements;
    std::vector<CommercialCadQualityGateReport> commercialCadReports;
    StrictTopologyGateReport topologyReport;
    SurfaceReductionFailureStage failureStage = SurfaceReductionFailureStage::None;
    std::vector<FaceId> failedFaces;
    std::string message;
};

struct DeterministicSearchResult {
    std::vector<SearchState> finalCandidates;
    std::map<std::vector<FaceId>, SurfaceReductionRejectedGroup> rejectedGroups;
    bool lowerBoundAccepted = false;
    SearchState lowerBoundState;
    FinalValidationResult lowerBoundValidation;
};

struct FinalCandidateSelection {
    bool accepted = false;
    SearchState state;
    FinalValidationResult validation;
    std::string bestFailure;
};

std::vector<FaceId> normalizedFaces(const std::vector<FaceId>& input) {
    auto faces = input;
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    return faces;
}

double edgeLength(const TopoDS_Edge& edge) {
    GProp_GProps properties;
    BRepGProp::LinearProperties(edge, properties);
    return std::max(0.0, properties.Mass());
}

bool normalOnSurfaceAtPoint(
    const Handle(Geom_Surface)& surface,
    const gp_Pnt& point,
    gp_Vec& normal) {
    if (surface.IsNull()) {
        return false;
    }
    GeomAPI_ProjectPointOnSurf projector(point, surface);
    if (projector.NbPoints() < 1) {
        return false;
    }
    double u = 0.0;
    double v = 0.0;
    projector.LowerDistanceParameters(u, v);
    gp_Pnt projected;
    gp_Vec du;
    gp_Vec dv;
    surface->D1(u, v, projected, du, dv);
    normal = du.Crossed(dv);
    if (normal.SquareMagnitude() <= Precision::SquareConfusion()) {
        return false;
    }
    normal.Normalize();
    return true;
}

double surfaceAngleAtEdge(
    const ShapeDocument& document,
    EdgeId edgeId,
    const Handle(Geom_Surface)& first,
    const Handle(Geom_Surface)& second) {
    try {
        BRepAdaptor_Curve curve(document.topology().edge(edgeId));
        const double firstParameter = curve.FirstParameter();
        const double lastParameter = curve.LastParameter();
        double maximumAngle = 0.0;
        for (int sampleIndex = 0; sampleIndex < 9; ++sampleIndex) {
            const double fraction = (static_cast<double>(sampleIndex) + 0.5) / 9.0;
            const gp_Pnt point = curve.Value(
                firstParameter + (lastParameter - firstParameter) * fraction);
            gp_Vec firstNormal;
            gp_Vec secondNormal;
            if (!normalOnSurfaceAtPoint(first, point, firstNormal) ||
                !normalOnSurfaceAtPoint(second, point, secondNormal)) {
                return 180.0;
            }
            const double angle = std::acos(std::clamp(
                std::abs(firstNormal.Dot(secondNormal)), 0.0, 1.0)) *
                180.0 / 3.14159265358979323846;
            maximumAngle = std::max(maximumAngle, angle);
        }
        return maximumAngle;
    } catch (...) {
        return 180.0;
    }
}

double originalFaceAngleAtEdge(
    const ShapeDocument& document,
    EdgeId edgeId) {
    const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
    if (adjacency == nullptr || adjacency->faces.size() != 2) {
        return 180.0;
    }
    return surfaceAngleAtEdge(
        document,
        edgeId,
        BRep_Tool::Surface(document.topology().face(adjacency->faces[0])),
        BRep_Tool::Surface(document.topology().face(adjacency->faces[1])));
}

double curvatureIndicatorAtPoint(
    const Handle(Geom_Surface)& surface,
    const gp_Pnt& point) {
    if (surface.IsNull()) {
        return std::numeric_limits<double>::infinity();
    }
    try {
        GeomAPI_ProjectPointOnSurf projector(point, surface);
        if (projector.NbPoints() < 1) {
            return std::numeric_limits<double>::infinity();
        }
        double u = 0.0;
        double v = 0.0;
        projector.LowerDistanceParameters(u, v);
        gp_Pnt projected;
        gp_Vec du;
        gp_Vec dv;
        gp_Vec duu;
        gp_Vec dvv;
        gp_Vec duv;
        surface->D2(u, v, projected, du, dv, duu, dvv, duv);
        const double firstDerivativeScale = du.SquareMagnitude() + dv.SquareMagnitude();
        return (duu.Magnitude() + dvv.Magnitude() + 2.0 * duv.Magnitude()) /
            std::max(firstDerivativeScale, 1.0e-12);
    } catch (...) {
        return std::numeric_limits<double>::infinity();
    }
}

double originalCurvatureDifferenceAtEdge(
    const ShapeDocument& document,
    EdgeId edgeId) {
    const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
    if (adjacency == nullptr || adjacency->faces.size() != 2) {
        return std::numeric_limits<double>::infinity();
    }
    try {
        BRepAdaptor_Curve curve(document.topology().edge(edgeId));
        const gp_Pnt point = curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter()));
        const double first = curvatureIndicatorAtPoint(
            BRep_Tool::Surface(document.topology().face(adjacency->faces[0])), point);
        const double second = curvatureIndicatorAtPoint(
            BRep_Tool::Surface(document.topology().face(adjacency->faces[1])), point);
        return std::isfinite(first) && std::isfinite(second)
            ? std::abs(first - second)
            : std::numeric_limits<double>::infinity();
    } catch (...) {
        return std::numeric_limits<double>::infinity();
    }
}

bool selectionConnected(
    const ShapeDocument& document,
    const std::vector<FaceId>& faces) {
    if (faces.empty()) {
        return false;
    }
    const std::set<FaceId> selected(faces.begin(), faces.end());
    std::set<FaceId> visited {faces.front()};
    std::queue<FaceId> pending;
    pending.push(faces.front());
    while (!pending.empty()) {
        const auto current = pending.front();
        pending.pop();
        for (const auto adjacent : document.topology().adjacentFaces(current)) {
            if (selected.contains(adjacent) && visited.insert(adjacent).second) {
                pending.push(adjacent);
            }
        }
    }
    return visited.size() == selected.size();
}

bool internalizesProtectedEdge(
    const ShapeDocument& document,
    const std::vector<FaceId>& faces,
    const std::set<EdgeId>& protectedEdges) {
    const std::set<FaceId> groupedFaces(faces.begin(), faces.end());
    for (const auto edgeId : protectedEdges) {
        const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
        if (adjacency == nullptr) {
            continue;
        }
        const int groupedAdjacentFaces = static_cast<int>(std::count_if(
            adjacency->faces.begin(),
            adjacency->faces.end(),
            [&](FaceId face) { return groupedFaces.contains(face); }));
        if (groupedAdjacentFaces >= 2) {
            return true;
        }
    }
    return false;
}

std::vector<std::vector<FaceId>> mergeableComponents(
    const std::vector<FaceId>& faces,
    const SurfaceReductionPreparedContext& prepared,
    const std::set<EdgeId>& mergeBlockedEdges) {
    std::map<FaceId, std::vector<FaceId>> adjacency;
    for (const auto face : faces) {
        adjacency[face];
    }
    for (const auto& edge : prepared.internalEdges) {
        if (mergeBlockedEdges.contains(edge.edgeId)) {
            continue;
        }
        adjacency[edge.firstFace].push_back(edge.secondFace);
        adjacency[edge.secondFace].push_back(edge.firstFace);
    }
    std::set<FaceId> visited;
    std::vector<std::vector<FaceId>> components;
    for (const auto root : faces) {
        if (!visited.insert(root).second) {
            continue;
        }
        std::vector<FaceId> component;
        std::queue<FaceId> pending;
        pending.push(root);
        while (!pending.empty()) {
            const auto current = pending.front();
            pending.pop();
            component.push_back(current);
            for (const auto adjacent : adjacency[current]) {
                if (visited.insert(adjacent).second) {
                    pending.push(adjacent);
                }
            }
        }
        components.push_back(normalizedFaces(component));
    }
    std::sort(components.begin(), components.end());
    return components;
}

std::map<FaceId, int> clusterMembership(const std::vector<Cluster>& clusters) {
    std::map<FaceId, int> membership;
    for (int index = 0; index < static_cast<int>(clusters.size()); ++index) {
        for (const auto face : clusters[static_cast<std::size_t>(index)].faces) {
            membership[face] = index;
        }
    }
    return membership;
}

PairProposalType proposalType(const Cluster& first, const Cluster& second) {
    const bool firstMerged = first.faces.size() > 1;
    const bool secondMerged = second.faces.size() > 1;
    if (firstMerged && secondMerged) {
        return PairProposalType::ClusterCluster;
    }
    if (firstMerged || secondMerged) {
        return PairProposalType::ClusterSingleton;
    }
    return PairProposalType::SingletonSingleton;
}

int proposalTypePriority(PairProposalType type) {
    switch (type) {
    case PairProposalType::ClusterCluster: return 0;
    case PairProposalType::ClusterSingleton: return 1;
    case PairProposalType::SingletonSingleton: return 2;
    }
    return 2;
}

SurfaceReductionProposalStatistics& proposalStatistics(
    SurfaceReductionReport& report,
    PairProposalType type) {
    switch (type) {
    case PairProposalType::ClusterCluster: return report.clusterClusterProposals;
    case PairProposalType::ClusterSingleton: return report.clusterSingletonProposals;
    case PairProposalType::SingletonSingleton: return report.singletonSingletonProposals;
    }
    return report.singletonSingletonProposals;
}

gp_Pnt clusterCentroid(
    const SurfaceReductionPreparedContext& prepared,
    const Cluster& cluster) {
    gp_XYZ weighted(0.0, 0.0, 0.0);
    double totalArea = 0.0;
    for (const auto face : cluster.faces) {
        const auto centroid = prepared.faceCentroids.find(face);
        const auto area = prepared.faceAreas.find(face);
        if (centroid == prepared.faceCentroids.end() || area == prepared.faceAreas.end()) {
            continue;
        }
        const double weight = std::max(area->second, 1.0e-12);
        weighted += centroid->second.XYZ() * weight;
        totalArea += weight;
    }
    return totalArea > 0.0
        ? gp_Pnt(weighted / totalArea)
        : gp_Pnt();
}

std::vector<PairCandidate> buildPairQueue(
    const ShapeDocument& document,
    const SurfaceReductionPreparedContext& prepared,
    const std::vector<Cluster>& clusters,
    const std::set<EdgeId>& protectedEdges,
    int diversityBins) {
    const auto membership = clusterMembership(clusters);
    std::map<std::pair<int, int>, PairCandidate> pairs;
    for (const auto& edge : prepared.internalEdges) {
        if (protectedEdges.contains(edge.edgeId)) {
            continue;
        }
        const auto firstMember = membership.find(edge.firstFace);
        const auto secondMember = membership.find(edge.secondFace);
        if (firstMember == membership.end() || secondMember == membership.end() ||
            firstMember->second == secondMember->second) {
            continue;
        }
        const int first = std::min(firstMember->second, secondMember->second);
        const int second = std::max(firstMember->second, secondMember->second);
        auto& pair = pairs[{first, second}];
        pair.first = first;
        pair.second = second;
        pair.normalJump = std::max(pair.normalJump, edge.normalJump);
        pair.curvatureDifference = std::max(
            pair.curvatureDifference,
            edge.curvatureDifference);
        pair.sharedLength += edge.length;
        pair.stableFace = std::min(clusters[static_cast<std::size_t>(first)].faces.front(),
                                   clusters[static_cast<std::size_t>(second)].faces.front());
    }
    std::vector<PairCandidate> queue;
    for (const auto& [key, pair] : pairs) {
        auto candidate = pair;
        const auto& first = clusters[static_cast<std::size_t>(candidate.first)];
        const auto& second = clusters[static_cast<std::size_t>(candidate.second)];
        auto groupedFaces = first.faces;
        groupedFaces.insert(
            groupedFaces.end(), second.faces.begin(), second.faces.end());
        if (internalizesProtectedEdge(
                document, normalizedFaces(groupedFaces), protectedEdges)) {
            continue;
        }
        candidate.type = proposalType(first, second);
        candidate.combinedOriginalFaceCount =
            static_cast<int>(first.faces.size() + second.faces.size());
        const auto firstCentroid = clusterCentroid(prepared, first);
        const auto secondCentroid = clusterCentroid(prepared, second);
        candidate.centroid.SetXYZ((firstCentroid.XYZ() + secondCentroid.XYZ()) * 0.5);
        queue.push_back(candidate);
    }
    std::map<std::pair<int, int>, int> aggregateRanks;
    const auto addRanks = [&](const auto& less) {
        auto ranked = queue;
        std::sort(ranked.begin(), ranked.end(), less);
        for (int rank = 0; rank < static_cast<int>(ranked.size()); ++rank) {
            const auto& pair = ranked[static_cast<std::size_t>(rank)];
            aggregateRanks[{pair.first, pair.second}] += rank;
        }
    };
    addRanks([](const PairCandidate& lhs, const PairCandidate& rhs) {
        return std::make_tuple(lhs.normalJump, lhs.stableFace, lhs.first, lhs.second) <
            std::make_tuple(rhs.normalJump, rhs.stableFace, rhs.first, rhs.second);
    });
    addRanks([](const PairCandidate& lhs, const PairCandidate& rhs) {
        return std::make_tuple(lhs.curvatureDifference, lhs.stableFace, lhs.first, lhs.second) <
            std::make_tuple(rhs.curvatureDifference, rhs.stableFace, rhs.first, rhs.second);
    });
    addRanks([](const PairCandidate& lhs, const PairCandidate& rhs) {
        return std::make_tuple(-lhs.sharedLength, lhs.stableFace, lhs.first, lhs.second) <
            std::make_tuple(-rhs.sharedLength, rhs.stableFace, rhs.first, rhs.second);
    });
    std::sort(queue.begin(), queue.end(), [&](const PairCandidate& lhs, const PairCandidate& rhs) {
        return std::make_tuple(
                   aggregateRanks.at({lhs.first, lhs.second}),
                   lhs.normalJump, lhs.curvatureDifference, -lhs.sharedLength,
                   lhs.stableFace, lhs.first, lhs.second) <
            std::make_tuple(
                   aggregateRanks.at({rhs.first, rhs.second}),
                   rhs.normalJump, rhs.curvatureDifference, -rhs.sharedLength,
                   rhs.stableFace, rhs.first, rhs.second);
    });
    if (queue.size() > 1 && diversityBins > 1) {
        const auto [minimumFace, maximumFace] = std::minmax_element(
            queue.begin(), queue.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.stableFace < rhs.stableFace;
            });
        const int binCount = std::min(diversityBins, static_cast<int>(queue.size()));
        const auto faceSpan = std::max<FaceId>(1, maximumFace->stableFace - minimumFace->stableFace + 1);
        std::vector<std::vector<PairCandidate>> bins(static_cast<std::size_t>(binCount));
        for (const auto& pair : queue) {
            const int bin = std::min(
                binCount - 1,
                static_cast<int>((pair.stableFace - minimumFace->stableFace) * binCount / faceSpan));
            bins[static_cast<std::size_t>(bin)].push_back(pair);
        }
        std::vector<PairCandidate> diverseQueue;
        diverseQueue.reserve(queue.size());
        for (std::size_t rank = 0; diverseQueue.size() < queue.size(); ++rank) {
            for (const auto& bin : bins) {
                if (rank < bin.size()) {
                    diverseQueue.push_back(bin[rank]);
                }
            }
        }
        queue = std::move(diverseQueue);
    }
    return queue;
}

std::vector<std::size_t> minimumPairOrder(
    const std::vector<PairCandidate>& queue,
    const std::vector<bool>& exact,
    const std::vector<bool>& knownSuccessfulFit,
    const std::vector<bool>& eligible,
    int limit,
    bool initialState) {
    std::vector<std::size_t> ranked;
    for (std::size_t index = 0; index < queue.size(); ++index) {
        if (eligible[index]) {
            ranked.push_back(index);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [&](std::size_t lhs, std::size_t rhs) {
        const auto& first = queue[lhs];
        const auto& second = queue[rhs];
        return std::make_tuple(
                   exact[lhs] ? 0 : 1,
                   proposalTypePriority(first.type),
                   knownSuccessfulFit[lhs] ? 0 : 1,
                   first.normalJump,
                   first.curvatureDifference,
                   -first.sharedLength,
                   -first.combinedOriginalFaceCount,
                   first.stableFace,
                   first.first,
                   first.second) <
            std::make_tuple(
                   exact[rhs] ? 0 : 1,
                   proposalTypePriority(second.type),
                   knownSuccessfulFit[rhs] ? 0 : 1,
                   second.normalJump,
                   second.curvatureDifference,
                   -second.sharedLength,
                   -second.combinedOriginalFaceCount,
                   second.stableFace,
                   second.first,
                   second.second);
    });
    if (static_cast<int>(ranked.size()) <= limit) {
        return ranked;
    }

    std::vector<std::size_t> selected;
    std::set<std::size_t> selectedSet;
    if (!initialState) {
        for (const auto type : {
                 PairProposalType::ClusterCluster,
                 PairProposalType::ClusterSingleton,
                 PairProposalType::SingletonSingleton}) {
            const auto representative = std::find_if(
                ranked.begin(), ranked.end(),
                [&](std::size_t index) { return queue[index].type == type; });
            if (representative != ranked.end() && static_cast<int>(selected.size()) < limit) {
                selected.push_back(*representative);
                selectedSet.insert(*representative);
            }
        }
    }
    if (initialState && !ranked.empty()) {
        const int qualitySeedCount = std::min(
            limit,
            std::max(1, limit / 2));
        for (int rank = 0; rank < qualitySeedCount; ++rank) {
            selected.push_back(ranked[static_cast<std::size_t>(rank)]);
            selectedSet.insert(ranked[static_cast<std::size_t>(rank)]);
        }
        while (static_cast<int>(selected.size()) < limit) {
            std::size_t bestIndex = queue.size();
            double bestDistance = -1.0;
            int bestExactRank = 2;
            int bestKnownSuccessRank = 2;
            std::size_t bestStableRank = ranked.size();
            for (std::size_t stableRank = 0; stableRank < ranked.size(); ++stableRank) {
                const auto index = ranked[stableRank];
                if (selectedSet.contains(index)) {
                    continue;
                }
                double minimumDistance = std::numeric_limits<double>::infinity();
                for (const auto chosen : selected) {
                    minimumDistance = std::min(
                        minimumDistance,
                        queue[index].centroid.SquareDistance(queue[chosen].centroid));
                }
                const int exactRank = exact[index] ? 0 : 1;
                const int knownSuccessRank = knownSuccessfulFit[index] ? 0 : 1;
                if (exactRank < bestExactRank ||
                    (exactRank == bestExactRank &&
                     knownSuccessRank < bestKnownSuccessRank) ||
                    (exactRank == bestExactRank &&
                     knownSuccessRank == bestKnownSuccessRank &&
                     minimumDistance > bestDistance) ||
                    (exactRank == bestExactRank &&
                     knownSuccessRank == bestKnownSuccessRank &&
                     minimumDistance == bestDistance &&
                     stableRank < bestStableRank)) {
                    bestIndex = index;
                    bestDistance = minimumDistance;
                    bestExactRank = exactRank;
                    bestKnownSuccessRank = knownSuccessRank;
                    bestStableRank = stableRank;
                }
            }
            if (bestIndex == queue.size()) {
                break;
            }
            selected.push_back(bestIndex);
            selectedSet.insert(bestIndex);
        }
        return selected;
    }

    for (const auto index : ranked) {
        if (static_cast<int>(selected.size()) >= limit) {
            break;
        }
        if (selectedSet.insert(index).second) {
            selected.push_back(index);
        }
    }
    std::vector<std::size_t> orderedSelection;
    orderedSelection.reserve(selected.size());
    for (const auto index : ranked) {
        if (selectedSet.contains(index)) {
            orderedSelection.push_back(index);
        }
    }
    return orderedSelection;
}

std::vector<FaceId> mergedFaces(const Cluster& first, const Cluster& second) {
    auto faces = first.faces;
    faces.insert(faces.end(), second.faces.begin(), second.faces.end());
    return normalizedFaces(faces);
}

MergeCandidate makeQualityGateCandidate(
    const ShapeDocument& document,
    const std::vector<FaceId>& faces) {
    MergeCandidate candidate;
    candidate.candidate_type = MergeCandidateType::FeatureBoundedRefit;
    candidate.faces = faces;
    candidate.face_count = static_cast<int>(faces.size());
    const std::set<FaceId> faceSet(faces.begin(), faces.end());
    std::set<EdgeId> edges;
    for (const auto face : faces) {
        const auto faceEdges = document.topology().edgesForFace(face);
        edges.insert(faceEdges.begin(), faceEdges.end());
    }
    for (const auto edge : edges) {
        const auto* adjacency = document.topology().adjacencyForEdge(edge);
        const int inside = adjacency == nullptr
            ? 0
            : static_cast<int>(std::count_if(
                adjacency->faces.begin(), adjacency->faces.end(),
                [&](FaceId face) { return faceSet.contains(face); }));
        if (inside >= 2) {
            candidate.internal_edges.push_back(edge);
        } else if (inside == 1) {
            candidate.boundary_edges.push_back(edge);
        }
    }
    candidate.internal_edge_count = static_cast<int>(candidate.internal_edges.size());
    candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());
    return candidate;
}

Handle(Geom_Surface) clusterSurface(
    const ShapeDocument& document,
    const Cluster& cluster) {
    if (!cluster.supportSurface.IsNull()) {
        return cluster.supportSurface;
    }
    return BRep_Tool::Surface(document.topology().face(cluster.faces.front()));
}

SeamQuality validateClusterSeams(
    const ShapeDocument& document,
    const std::vector<Cluster>& clusters,
    int candidateIndex,
    const std::set<EdgeId>& protectedEdges,
    double toleranceDegrees) {
    SeamQuality quality;
    const auto membership = clusterMembership(clusters);
    for (EdgeId edgeId = 0; edgeId < document.topology().edgeCount(); ++edgeId) {
        if (protectedEdges.contains(edgeId)) {
            continue;
        }
        const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
        if (adjacency == nullptr || adjacency->faces.size() != 2) {
            continue;
        }
        const auto first = membership.find(adjacency->faces[0]);
        const auto second = membership.find(adjacency->faces[1]);
        if (first == membership.end() || second == membership.end() || first->second == second->second ||
            (first->second != candidateIndex && second->second != candidateIndex)) {
            continue;
        }
        const double angle = surfaceAngleAtEdge(
            document,
            edgeId,
            clusterSurface(document, clusters[static_cast<std::size_t>(first->second)]),
            clusterSurface(document, clusters[static_cast<std::size_t>(second->second)]));
        quality.maximumAngle = std::max(quality.maximumAngle, angle);
        if (angle > toleranceDegrees) {
            quality.valid = false;
        }
    }
    return quality;
}

SeamQuality validateAllClusterSeams(
    const ShapeDocument& document,
    const SurfaceReductionPreparedContext& prepared,
    const std::vector<Cluster>& clusters,
    const std::set<EdgeId>& protectedEdges,
    double toleranceDegrees) {
    SeamQuality quality;
    const auto membership = clusterMembership(clusters);
    for (const auto& edge : prepared.internalEdges) {
        if (protectedEdges.contains(edge.edgeId)) {
            continue;
        }
        const auto first = membership.find(edge.firstFace);
        const auto second = membership.find(edge.secondFace);
        if (first == membership.end() || second == membership.end() ||
            first->second == second->second) {
            continue;
        }
        if (clusters[static_cast<std::size_t>(first->second)].faces.size() < 2 &&
            clusters[static_cast<std::size_t>(second->second)].faces.size() < 2) {
            continue;
        }
        const double angle = surfaceAngleAtEdge(
            document,
            edge.edgeId,
            clusterSurface(document, clusters[static_cast<std::size_t>(first->second)]),
            clusterSurface(document, clusters[static_cast<std::size_t>(second->second)]));
        quality.maximumAngle = std::max(quality.maximumAngle, angle);
        quality.valid = quality.valid && angle <= toleranceDegrees;
    }
    return quality;
}

SeamQuality updateClusterSeams(
    const ShapeDocument& document,
    const SurfaceReductionPreparedContext& prepared,
    const std::vector<Cluster>& clusters,
    const std::vector<FaceId>& changedFaces,
    const std::set<EdgeId>& protectedEdges,
    double toleranceDegrees,
    std::map<EdgeId, double>& seamAngles) {
    const auto membership = clusterMembership(clusters);
    const std::set<FaceId> changed(changedFaces.begin(), changedFaces.end());
    for (const auto& edge : prepared.internalEdges) {
        if (protectedEdges.contains(edge.edgeId)) {
            seamAngles.erase(edge.edgeId);
            continue;
        }
        const auto first = membership.find(edge.firstFace);
        const auto second = membership.find(edge.secondFace);
        if (first == membership.end() || second == membership.end() ||
            first->second == second->second) {
            seamAngles.erase(edge.edgeId);
            continue;
        }
        if (!changed.contains(edge.firstFace) && !changed.contains(edge.secondFace)) {
            continue;
        }
        auto firstFaces = clusters[static_cast<std::size_t>(first->second)].faces;
        auto secondFaces = clusters[static_cast<std::size_t>(second->second)].faces;
        if (secondFaces < firstFaces) std::swap(firstFaces, secondFaces);
        const auto key = std::make_tuple(edge.edgeId, firstFaces, secondFaces);
        const auto cached = prepared.seamAngleCache.find(key);
        if (cached != prepared.seamAngleCache.end()) {
            seamAngles[edge.edgeId] = cached->second;
        } else {
            const auto angle = surfaceAngleAtEdge(
                document,
                edge.edgeId,
                clusterSurface(document, clusters[static_cast<std::size_t>(first->second)]),
                clusterSurface(document, clusters[static_cast<std::size_t>(second->second)]));
            prepared.seamAngleCache.emplace(key, angle);
            seamAngles[edge.edgeId] = angle;
        }
    }
    SeamQuality quality;
    for (const auto& [edgeId, angle] : seamAngles) {
        (void)edgeId;
        quality.maximumAngle = std::max(quality.maximumAngle, angle);
        quality.valid = quality.valid && angle <= toleranceDegrees;
    }
    return quality;
}

TopoDS_Shape makePreviewShape(
    const ShapeDocument& document,
    const std::vector<Cluster>& clusters) {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const auto& cluster : clusters) {
        if (!cluster.replacementFace.IsNull()) {
            builder.Add(compound, cluster.replacementFace);
        } else if (cluster.faces.size() == 1) {
            builder.Add(compound, document.topology().face(cluster.faces.front()));
        }
    }
    return compound;
}

TopoDS_Shape preserveOpenShellContainer(
    const ShapeDocument& before,
    const TopoDS_Shape& shape) {
    if (before.stats().shells <= 0 || shape.IsNull()) {
        return shape;
    }
    const ShapeDocument assembled(shape, {});
    if (assembled.stats().shells > 0 || assembled.stats().solids > 0) {
        return shape;
    }
    TopoDS_Shell shell;
    BRep_Builder builder;
    builder.MakeShell(shell);
    int faceCount = 0;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        builder.Add(shell, TopoDS::Face(explorer.Current()));
        ++faceCount;
    }
    return faceCount > 0 ? TopoDS_Shape(shell) : shape;
}

TopoDS_Shape assembleFaceCompound(
    const ShapeDocument& document,
    const std::vector<Cluster>& clusters) {
    std::set<FaceId> replacedFaces;
    for (const auto& cluster : clusters) {
        if (cluster.faces.size() < 2 || cluster.replacementFace.IsNull()) {
            continue;
        }
        replacedFaces.insert(cluster.faces.begin(), cluster.faces.end());
    }

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (FaceId faceId = 0; faceId < document.topology().faceCount(); ++faceId) {
        if (!replacedFaces.contains(faceId)) {
            builder.Add(compound, document.topology().face(faceId));
        }
    }
    for (const auto& cluster : clusters) {
        if (cluster.faces.size() >= 2 && !cluster.replacementFace.IsNull()) {
            builder.Add(compound, cluster.replacementFace);
        }
    }
    return compound;
}

TopoDS_Shape assembleWithReShape(
    const ShapeDocument& document,
    const std::vector<Cluster>& clusters) {
    BRepTools_ReShape reshaper;
    for (const auto& cluster : clusters) {
        if (cluster.faces.size() < 2 || cluster.replacementFace.IsNull()) {
            continue;
        }
        reshaper.Replace(document.topology().face(cluster.faces.front()), cluster.replacementFace);
        for (std::size_t index = 1; index < cluster.faces.size(); ++index) {
            reshaper.Remove(document.topology().face(cluster.faces[index]));
        }
    }
    return reshaper.Apply(document.shape());
}

TopoDS_Face tryExactSameDomainGroup(
    const ShapeDocument& document,
    const std::vector<FaceId>& faces) {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const auto faceId : faces) {
        builder.Add(compound, document.topology().face(faceId));
    }
    try {
        ShapeUpgrade_UnifySameDomain unifier(
            compound,
            Standard_True,
            Standard_True,
            Standard_True);
        unifier.Build();
        const auto unified = unifier.Shape();
        TopoDS_Face result;
        int faceCount = 0;
        for (TopExp_Explorer explorer(unified, TopAbs_FACE); explorer.More(); explorer.Next()) {
            result = TopoDS::Face(explorer.Current());
            ++faceCount;
        }
        if (faceCount == 1 && !result.IsNull() && BRepCheck_Analyzer(result, Standard_True).IsValid()) {
            return result;
        }
    } catch (...) {
    }
    return {};
}

std::string stateSignature(const SearchState& state) {
    std::vector<std::vector<FaceId>> groups;
    groups.reserve(state.clusters.size());
    for (const auto& cluster : state.clusters) {
        groups.push_back(cluster.faces);
    }
    std::sort(groups.begin(), groups.end());
    std::string signature;
    for (const auto& group : groups) {
        if (!signature.empty()) {
            signature.push_back('|');
        }
        for (std::size_t index = 0; index < group.size(); ++index) {
            if (index > 0) {
                signature.push_back(',');
            }
            signature += std::to_string(group[index]);
        }
    }
    return signature;
}

auto stateRank(const SearchState& state) {
    double maximumDistance = 0.0;
    double maximumNormal = 0.0;
    int controlPointCount = 0;
    for (const auto& cluster : state.clusters) {
        maximumDistance = std::max({
            maximumDistance,
            cluster.fitReport.maxBoundaryDistance,
            cluster.fitReport.maxSelectionDistance,
            cluster.fitReport.maxHoldoutDistance});
        maximumNormal = std::max({
            maximumNormal,
            cluster.fitReport.maxSelectionNormalAngleDegrees,
            cluster.fitReport.maxHoldoutNormalAngleDegrees});
        controlPointCount += cluster.fitReport.controlPointCount;
    }
    return std::make_tuple(
        state.clusters.size(),
        state.maximumSeamAngle,
        maximumDistance,
        maximumNormal,
        controlPointCount,
        stateSignature(state));
}

SurfaceReductionFailureStage mapFailureStage(
    RestrictedBSplinePatchFitFailureStage stage) {
    switch (stage) {
    case RestrictedBSplinePatchFitFailureStage::None:
        return SurfaceReductionFailureStage::None;
    case RestrictedBSplinePatchFitFailureStage::Parameterization:
        return SurfaceReductionFailureStage::Parameterization;
    case RestrictedBSplinePatchFitFailureStage::BoundaryConstraints:
        return SurfaceReductionFailureStage::BoundaryConstraints;
    case RestrictedBSplinePatchFitFailureStage::UvIteration:
        return SurfaceReductionFailureStage::UvIteration;
    case RestrictedBSplinePatchFitFailureStage::ModelSelection:
        return SurfaceReductionFailureStage::ModelSelection;
    case RestrictedBSplinePatchFitFailureStage::Holdout:
        return SurfaceReductionFailureStage::Holdout;
    case RestrictedBSplinePatchFitFailureStage::Jacobian:
        return SurfaceReductionFailureStage::Jacobian;
    case RestrictedBSplinePatchFitFailureStage::Retrim:
        return SurfaceReductionFailureStage::Retrim;
    case RestrictedBSplinePatchFitFailureStage::BoundaryHoldout:
        return SurfaceReductionFailureStage::BoundaryHoldout;
    case RestrictedBSplinePatchFitFailureStage::BrepCheck:
        return SurfaceReductionFailureStage::BrepCheck;
    }
    return SurfaceReductionFailureStage::None;
}

FinalValidationResult validateFinalState(
    const ShapeDocument& document,
    const std::vector<FaceId>& selectedFaces,
    const std::vector<Cluster>& clusters,
    const SurfaceReductionOptions& options,
    SurfaceReductionStageTiming& timing,
    const SurfaceReductionExecutionOptions& execution,
    const SurfaceReductionReport& report,
    int targetFaceCount,
    int candidateValidationIndex,
    int candidateValidationCount) {
    FinalValidationResult result;
    auto assemblyTimer = std::make_unique<ScopedMilliseconds>(
        timing.assemblyAndCommercialCadMs);
    const auto featureEdges = FeatureEdgeDetector().detect(
        document.topology(),
        options.smoothSeamToleranceDegrees,
        0.0);
    for (const auto& cluster : clusters) {
        if (cluster.faces.size() < 2 || cluster.replacementFace.IsNull()) {
            continue;
        }
        publishProgress(
            execution,
            SurfaceReductionProgressStage::ValidatingCommercialCad,
            report,
            targetFaceCount,
            static_cast<int>(clusters.size()),
            options.maximumRestrictedFitEvaluations,
            "正在检查局部替换面的边界、角点、特征边和接缝质量。",
            0,
            cluster.faces,
            candidateValidationIndex,
            candidateValidationCount);
        const auto candidate = makeQualityGateCandidate(document, cluster.faces);
        auto boundary = RegionBoundaryAnalyzer().analyze(document, candidate);
        if (!boundary.valid &&
            boundary.failure_reason == BoundaryAnalysisFailureReason::InnerLoopsNotSupported &&
            boundary.boundary_closed &&
            !boundary.ordered_boundary_edges.empty()) {
            boundary.valid = true;
            boundary.message = "曲面减面质量门保留原始外轮廓和内孔线框。";
        }
        CommercialCadQualityGateInput qualityInput;
        qualityInput.document = &document;
        qualityInput.candidate = &candidate;
        qualityInput.boundary = &boundary;
        qualityInput.featureEdges = &featureEdges;
        qualityInput.patchShape = &cluster.replacementFace;
        qualityInput.options.maxBoundaryDistance = options.tolerance3d;
        qualityInput.options.maxCornerAnchorDistance = options.tolerance3d;
        qualityInput.options.maxFeatureEdgeDistance = options.tolerance3d;
        qualityInput.options.maxSeamNormalOffset = options.tolerance3d;
        auto qualityReport = CommercialCadQualityGate().evaluate(qualityInput);
        const bool passed = qualityReport.passed;
        const auto message = qualityReport.message;
        result.commercialCadReports.push_back(std::move(qualityReport));
        if (!passed) {
            result.failureStage = SurfaceReductionFailureStage::CommercialCad;
            result.failedFaces = cluster.faces;
            result.message = "商业 CAD 类边界质量门失败：" + message;
            return result;
        }
        result.replacements.push_back({cluster.faces, cluster.replacementFace});
    }

    publishProgress(
        execution,
        SurfaceReductionProgressStage::AssemblingModel,
        report,
        targetFaceCount,
        static_cast<int>(clusters.size()),
        options.maximumRestrictedFitEvaluations,
        "正在组装整件 B-rep 并执行保守修复。",
        0,
        {},
        candidateValidationIndex,
        candidateValidationCount);
    const auto replacedShape = document.stats().solids == 1 && document.stats().shells == 1
        ? assembleFaceCompound(document, clusters)
        : assembleWithReShape(document, clusters);
    if (replacedShape.IsNull()) {
        result.failureStage = SurfaceReductionFailureStage::Topology;
        result.message = "事务组装未生成有效 B-rep。";
        return result;
    }

    const double sewingTolerance = std::max(
        1.0e-9,
        std::min(1.0e-6, options.tolerance3d * 0.01));
    TopoDS_Shape assembledShape = replacedShape;
    if (document.stats().solids == 0) {
        BRepBuilderAPI_Sewing sewing(sewingTolerance);
        sewing.SetSameParameterMode(Standard_True);
        sewing.Add(replacedShape);
        sewing.Perform();
        if (!sewing.SewedShape().IsNull()) {
            assembledShape = preserveOpenShellContainer(document, sewing.SewedShape());
        }
    }

    PatchReplacementRepairOptions repairOptions;
    repairOptions.runSewing = document.stats().solids > 0;
    repairOptions.runAdaptiveSewing = false;
    repairOptions.runUnifySameDomain = false;
    repairOptions.runShellToSolid = document.stats().solids > 0;
    repairOptions.sewingTolerance = sewingTolerance;
    repairOptions.preferredSewingTolerance = sewingTolerance;
    repairOptions.minSewingTolerance = sewingTolerance;
    repairOptions.maxSewingTolerance = sewingTolerance;
    const auto repaired = repairPatchReplacementShape(assembledShape, repairOptions);
    if (!repaired.success || repaired.shape.IsNull()) {
        result.failureStage = SurfaceReductionFailureStage::Topology;
        result.message = "B-rep 修复未生成可验证结果。";
        return result;
    }

    const ShapeDocument reducedDocument(repaired.shape, document.sourcePath());
    assemblyTimer.reset();
    ScopedMilliseconds topologyTimer(timing.strictTopologyAndStepRoundtripMs);
    publishProgress(
        execution,
        SurfaceReductionProgressStage::ValidatingTopology,
        report,
        targetFaceCount,
        static_cast<int>(clusters.size()),
        options.maximumRestrictedFitEvaluations,
        "正在执行 StrictTopologyGate、BRepCheck 和 STEP 回读。",
        0,
        {},
        candidateValidationIndex,
        candidateValidationCount);
    StrictTopologyGateInput gateInput;
    gateInput.beforeDocument = &document;
    gateInput.afterDocument = &reducedDocument;
    gateInput.requireStepRoundtrip = true;
    gateInput.allowMultiFaceReplacement = true;
    gateInput.allowFaceCountIncrease = false;
    result.topologyReport = StrictTopologyGate().evaluate(gateInput);
    const int expectedFaceCount = document.stats().faces -
        static_cast<int>(selectedFaces.size()) + static_cast<int>(clusters.size());
    if (!result.topologyReport.passed || reducedDocument.stats().faces > expectedFaceCount) {
        result.failureStage = result.topologyReport.stepRoundtripOk
            ? SurfaceReductionFailureStage::Topology
            : SurfaceReductionFailureStage::StepRoundtrip;
        result.message = "全局 B-rep 或 STEP 回读质量门失败：" +
            result.topologyReport.message;
        return result;
    }

    result.passed = true;
    result.reducedShape = repaired.shape;
    result.previewShape = makePreviewShape(document, clusters);
    return result;
}

DeterministicSearchResult runDeterministicReductionSearch(
    const ShapeDocument& document,
    const SurfaceReductionRequest& request,
    const SurfaceReductionExecutionOptions& execution,
    const SurfaceReductionPreparedContext& prepared,
    const std::vector<FaceId>& faces,
    int targetCount,
    const SearchState& initialState,
    const std::set<EdgeId>& mergeBlockedEdges,
    const std::set<EdgeId>& nonSmoothSeamEdges,
    const RestrictedBSplinePatchFitOptions& fitOptions,
    SurfaceReductionReport& report) {
    DeterministicSearchResult search;
    const bool hasScanReference = request.scanReferenceStl != nullptr;
    std::vector<SearchState> frontier {initialState};
    std::map<std::string, SearchState> archivedStates;
    std::map<std::vector<FaceId>, CachedFit> fitCache;
    std::map<std::vector<FaceId>, CachedFit> stepOnlyFitCache;
    std::map<std::vector<FaceId>, TopoDS_Face> exactCache;
    using FitVariantKey = std::pair<bool, std::vector<FaceId>>;
    std::set<FitVariantKey> prefetchedGroupSets;
    std::set<FitVariantKey> usedGroupSets;
    const auto uniqueGroupSetCount = [&] {
        std::set<std::vector<FaceId>> groups;
        for (const auto& [groupedFaces, cached] : fitCache) {
            groups.insert(groupedFaces);
        }
        for (const auto& [groupedFaces, cached] : stepOnlyFitCache) {
            groups.insert(groupedFaces);
        }
        for (const auto& [groupedFaces, face] : exactCache) {
            groups.insert(groupedFaces);
        }
        return static_cast<int>(groups.size());
    };
    int searchIteration = 0;
    SurfaceReductionFitCache persistentCache(execution);
    const bool persistentCacheEnabled = execution.cacheMode != SurfaceReductionCacheMode::Disabled;
    const bool persistentCacheReadable = execution.cacheMode == SurfaceReductionCacheMode::ReadWrite ||
        execution.cacheMode == SurfaceReductionCacheMode::ReadOnlyRequired;
    const bool persistentCacheWritable = execution.cacheMode == SurfaceReductionCacheMode::ReadWrite ||
        execution.cacheMode == SurfaceReductionCacheMode::Refresh;
    const auto recordFitProfile = [&](const std::vector<FaceId>& groupedFaces,
                                      const CachedFit& cached,
                                      bool persistentHit) {
        report.fitProfiles.push_back({
            groupedFaces,
            cached.success,
            mapFailureStage(cached.report.failureStage),
            cached.report.timing.totalMs,
            cached.report.kktSolveCount,
            cached.report.boundaryProjectionCount,
            cached.report.scanIndexCandidateTriangleCount,
            persistentHit});
        report.scanIndexCandidateTriangleCount += cached.report.scanIndexCandidateTriangleCount;
    };
    const auto loadPersistent = [&](const std::vector<FaceId>& groupedFaces,
                                    const RestrictedBSplinePatchFitOptions& options,
                                    CachedFit& cached) {
        if (!persistentCacheEnabled || !persistentCacheReadable) return false;
        const auto cacheKey = persistentCache.key(groupedFaces, options);
        const auto entry = persistentCache.load(cacheKey);
        if (!entry.has_value()) {
            ++report.persistentFitCacheMissCount;
            if (execution.cacheMode == SurfaceReductionCacheMode::ReadOnlyRequired) {
                report.persistentFitCacheRequiredMiss = true;
            }
            return false;
        }
        ++report.persistentFitCacheHitCount;
        cached.success = entry->success;
        cached.face = entry->face;
        cached.surface = entry->surface;
        cached.report = entry->report;
        cached.method = SurfaceReductionMergeMethod::RestrictedBSpline;
        recordFitProfile(groupedFaces, cached, true);
        return true;
    };
    const auto storePersistent = [&](const std::vector<FaceId>& groupedFaces,
                                     const RestrictedBSplinePatchFitOptions& options,
                                     const CachedFit& cached) {
        const bool deterministicResult =
            cached.success ||
            cached.report.failureStage != RestrictedBSplinePatchFitFailureStage::None;
        if (!persistentCacheWritable || !deterministicResult) return;
        SurfaceReductionFitCacheEntry entry;
        entry.success = cached.success;
        entry.face = cached.face;
        entry.surface = cached.surface;
        entry.report = cached.report;
        if (persistentCache.store(persistentCache.key(groupedFaces, options), entry)) {
            ++report.persistentFitCacheWriteCount;
        }
    };
    auto stepOnlyFallback = [&](const std::vector<FaceId>& groupedFaces) -> const CachedFit* {
        usedGroupSets.emplace(true, groupedFaces);
        const auto existing = stepOnlyFitCache.find(groupedFaces);
        if (existing != stepOnlyFitCache.end()) {
            ++report.fitCacheHitCount;
            return &existing->second;
        }
        auto options = fitOptions;
        options.scanReference = nullptr;
        options.scanRegionIndex = nullptr;
        options.scanReferenceCoverage = ScanReferenceCoverage::None;
        CachedFit cached;
        cached.method = SurfaceReductionMergeMethod::RestrictedBSpline;
        if (!loadPersistent(groupedFaces, options, cached)) {
            if (report.persistentFitCacheRequiredMiss) return nullptr;
            if (report.fitCacheMissCount >=
                request.options.maximumRestrictedFitEvaluations) {
                report.searchBudgetExhausted = true;
                return nullptr;
            }
            publishProgress(
                execution,
                SurfaceReductionProgressStage::Fitting,
                report,
                targetCount,
                frontier.empty() ? static_cast<int>(faces.size()) :
                    static_cast<int>(frontier.front().clusters.size()),
                request.options.maximumRestrictedFitEvaluations,
                "正在计算 STEP-only 回退拟合。",
                searchIteration,
                groupedFaces);
            const auto fit = RestrictedBSplinePatchFitter().fit(document, groupedFaces, options);
            ++report.fitCacheMissCount;
            ++report.restrictedFitEvaluationCount;
            cached.success = fit.success();
            cached.face = fit.face;
            cached.surface = fit.surface;
            cached.report = fit.report;
            report.timing.restrictedFitMs += fit.report.timing.totalMs;
            recordFitProfile(groupedFaces, cached, false);
            storePersistent(groupedFaces, options, cached);
            publishProgress(
                execution,
                SurfaceReductionProgressStage::Fitting,
                report,
                targetCount,
                frontier.empty() ? static_cast<int>(faces.size()) :
                    static_cast<int>(frontier.front().clusters.size()),
                request.options.maximumRestrictedFitEvaluations,
                "STEP-only 回退拟合完成。",
                searchIteration,
                groupedFaces);
        }
        return &stepOnlyFitCache.emplace(groupedFaces, std::move(cached)).first->second;
    };
    const auto prefetchStepOnlyFits = [&](const std::vector<std::vector<FaceId>>& groupedFacesBatch) {
        if (!hasScanReference || execution.fitWorkerCount <= 1 ||
            execution.cacheMode == SurfaceReductionCacheMode::ReadOnlyRequired) {
            return;
        }
        auto options = fitOptions;
        options.scanReference = nullptr;
        options.scanRegionIndex = nullptr;
        options.scanReferenceCoverage = ScanReferenceCoverage::None;
        std::vector<std::vector<FaceId>> computeGroups;
        for (const auto& groupedFaces : groupedFacesBatch) {
            const auto primary = fitCache.find(groupedFaces);
            if (primary == fitCache.end() || primary->second.success ||
                stepOnlyFitCache.contains(groupedFaces)) {
                continue;
            }
            CachedFit loaded;
            loaded.method = SurfaceReductionMergeMethod::RestrictedBSpline;
            if (loadPersistent(groupedFaces, options, loaded)) {
                stepOnlyFitCache.emplace(groupedFaces, std::move(loaded));
            } else if (!report.persistentFitCacheRequiredMiss &&
                       report.fitCacheMissCount + static_cast<int>(computeGroups.size()) <
                           request.options.maximumRestrictedFitEvaluations) {
                computeGroups.push_back(groupedFaces);
            }
        }
        if (computeGroups.empty() || report.persistentFitCacheRequiredMiss) return;
        std::sort(computeGroups.begin(), computeGroups.end());
        publishProgress(
            execution,
            SurfaceReductionProgressStage::Fitting,
            report,
            targetCount,
            frontier.empty() ? static_cast<int>(faces.size()) :
                static_cast<int>(frontier.front().clusters.size()),
            request.options.maximumRestrictedFitEvaluations,
            "扫描辅助结果需要回退，正在批量计算 STEP-only 对照拟合。",
            searchIteration,
            computeGroups.front());
        const auto batchStarted = std::chrono::steady_clock::now();
        std::vector<std::future<RestrictedBSplinePatchFitResult>> futures;
        futures.reserve(computeGroups.size());
        for (const auto& groupedFaces : computeGroups) {
            futures.push_back(std::async(
                std::launch::async,
                [&document, options, groupedFaces] {
                    return RestrictedBSplinePatchFitter().fit(
                        document, groupedFaces, options);
                }));
        }
        for (std::size_t index = 0; index < futures.size(); ++index) {
            const auto fit = futures[index].get();
            CachedFit cached;
            cached.success = fit.success();
            cached.face = fit.face;
            cached.surface = fit.surface;
            cached.report = fit.report;
            cached.method = SurfaceReductionMergeMethod::RestrictedBSpline;
            ++report.fitCacheMissCount;
            ++report.restrictedFitEvaluationCount;
            recordFitProfile(computeGroups[index], cached, false);
            storePersistent(computeGroups[index], options, cached);
            stepOnlyFitCache.emplace(computeGroups[index], std::move(cached));
            prefetchedGroupSets.emplace(true, computeGroups[index]);
            publishProgress(
                execution,
                SurfaceReductionProgressStage::Fitting,
                report,
                targetCount,
                frontier.empty() ? static_cast<int>(faces.size()) :
                    static_cast<int>(frontier.front().clusters.size()),
                request.options.maximumRestrictedFitEvaluations,
                "STEP-only 回退拟合已完成一个分组。",
                searchIteration,
                computeGroups[index]);
        }
        report.timing.restrictedFitMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - batchStarted).count();
    };
    const auto searchStarted = std::chrono::steady_clock::now();
    const bool minimizeFaceCount =
        request.options.searchObjective == SurfaceReductionSearchObjective::MinimizeFaceCount;
    const auto searchTimeExpired = [&] {
        return execution.maximumSearchWallTimeMilliseconds > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - searchStarted).count() >=
                execution.maximumSearchWallTimeMilliseconds;
    };
    const auto evaluateGroup = [&](const std::vector<FaceId>& groupedFaces)
        -> std::optional<CachedFit> {
        usedGroupSets.emplace(false, groupedFaces);
        const auto existing = fitCache.find(groupedFaces);
        if (existing != fitCache.end()) {
            ++report.fitCacheHitCount;
            if ((!existing->second.success || existing->second.face.IsNull() ||
                 existing->second.surface.IsNull()) &&
                hasScanReference) {
                const auto* fallback = stepOnlyFallback(groupedFaces);
                if (fallback != nullptr && fallback->success &&
                    !fallback->face.IsNull() && !fallback->surface.IsNull()) {
                    return *fallback;
                }
            }
            return existing->second;
        }

        auto exactResult = exactCache.find(groupedFaces);
        if (exactResult == exactCache.end()) {
            ++report.exactSameDomainAttemptCount;
            ScopedMilliseconds timer(report.timing.exactMergeMs);
            exactResult = exactCache.emplace(
                groupedFaces, tryExactSameDomainGroup(document, groupedFaces)).first;
            if (!exactResult->second.IsNull()) {
                ++report.exactSameDomainSuccessCount;
            }
        }
        if (!exactResult->second.IsNull()) {
            CachedFit cached;
            cached.face = exactResult->second;
            cached.surface = BRep_Tool::Surface(cached.face);
            cached.success = !cached.face.IsNull() && !cached.surface.IsNull();
            cached.method = SurfaceReductionMergeMethod::ExactSameDomain;
            cached.report.success = cached.success;
            cached.report.parameterizationValid = cached.success;
            cached.report.selectionPassed = cached.success;
            cached.report.holdoutPassed = cached.success;
            cached.report.brepCheckValid = cached.success;
            cached.report.message = "同域面已执行无损精确合并。";
            fitCache.emplace(groupedFaces, cached);
            return cached;
        }

        CachedFit cached;
        cached.method = SurfaceReductionMergeMethod::RestrictedBSpline;
        if (!loadPersistent(groupedFaces, fitOptions, cached)) {
            if (report.persistentFitCacheRequiredMiss) {
                return std::nullopt;
            }
            if (report.fitCacheMissCount >=
                request.options.maximumRestrictedFitEvaluations) {
                report.searchBudgetExhausted = true;
                return std::nullopt;
            }
            publishProgress(
                execution,
                SurfaceReductionProgressStage::Fitting,
                report,
                targetCount,
                frontier.empty() ? static_cast<int>(faces.size()) :
                    static_cast<int>(frontier.front().clusters.size()),
                request.options.maximumRestrictedFitEvaluations,
                "正在从原始 STEP FaceId 并集执行受限 B-spline 拟合。",
                searchIteration,
                groupedFaces);
            const auto fit = RestrictedBSplinePatchFitter().fit(
                document, groupedFaces, fitOptions);
            ++report.fitCacheMissCount;
            ++report.restrictedFitEvaluationCount;
            cached.success = fit.success();
            cached.face = fit.face;
            cached.surface = fit.surface;
            cached.report = fit.report;
            report.timing.restrictedFitMs += fit.report.timing.totalMs;
            recordFitProfile(groupedFaces, cached, false);
            storePersistent(groupedFaces, fitOptions, cached);
        }
        fitCache.emplace(groupedFaces, cached);
        if ((!cached.success || cached.face.IsNull() || cached.surface.IsNull()) &&
            hasScanReference) {
            const auto* fallback = stepOnlyFallback(groupedFaces);
            if (fallback != nullptr && fallback->success &&
                !fallback->face.IsNull() && !fallback->surface.IsNull()) {
                return *fallback;
            }
        }
        return cached;
    };

    if (minimizeFaceCount &&
        report.theoreticalMinimumFaceCount < static_cast<int>(faces.size())) {
        const auto components = mergeableComponents(faces, prepared, mergeBlockedEdges);
        SearchState lowerBoundState;
        bool allComponentsFitted = true;
        publishProgress(
            execution,
            SurfaceReductionProgressStage::Searching,
            report,
            targetCount,
            static_cast<int>(faces.size()),
            request.options.maximumRestrictedFitEvaluations,
            "正在直接探测受保护边决定的理论最少面数状态。");
        for (const auto& component : components) {
            if (searchTimeExpired()) {
                report.searchTimeBudgetExhausted = true;
                report.stopReason =
                    SurfaceReductionStopReason::SearchTimeBudgetExhausted;
                allComponentsFitted = false;
                break;
            }
            if (internalizesProtectedEdge(
                    document, component, mergeBlockedEdges)) {
                allComponentsFitted = false;
                search.rejectedGroups.try_emplace(
                    component,
                    SurfaceReductionRejectedGroup {
                        component,
                        SurfaceReductionFailureStage::Topology,
                        "理论下界分组会内化受保护边，已禁止直接合并。"});
                continue;
            }
            if (component.size() == 1) {
                const auto face = component.front();
                lowerBoundState.clusters.push_back(Cluster {
                    component,
                    {},
                    BRep_Tool::Surface(document.topology().face(face)),
                    {},
                    SurfaceReductionMergeMethod::OriginalFace});
                continue;
            }
            const auto evaluated = evaluateGroup(component);
            if (!evaluated.has_value() || !evaluated->success ||
                evaluated->face.IsNull() || evaluated->surface.IsNull()) {
                allComponentsFitted = false;
                if (evaluated.has_value()) {
                    search.rejectedGroups.try_emplace(
                        component,
                        SurfaceReductionRejectedGroup {
                            component,
                            mapFailureStage(evaluated->report.failureStage),
                            evaluated->report.message,
                            evaluated->report});
                }
                if (report.persistentFitCacheRequiredMiss) {
                    break;
                }
                continue;
            }
            lowerBoundState.clusters.push_back(Cluster {
                component,
                evaluated->face,
                evaluated->surface,
                evaluated->report,
                evaluated->method});
        }
        if (allComponentsFitted &&
            lowerBoundState.clusters.size() == components.size()) {
            const auto seam = validateAllClusterSeams(
                document,
                prepared,
                lowerBoundState.clusters,
                nonSmoothSeamEdges,
                request.options.smoothSeamToleranceDegrees);
            lowerBoundState.maximumSeamAngle = seam.maximumAngle;
            archivedStates.try_emplace(
                stateSignature(lowerBoundState), lowerBoundState);
            if (seam.valid &&
                report.finalValidationAttemptCount <
                    request.options.maximumFinalValidationEvaluations) {
                ++report.finalValidationAttemptCount;
                auto validation = validateFinalState(
                    document,
                    faces,
                    lowerBoundState.clusters,
                    request.options,
                    report.timing,
                    execution,
                    report,
                    targetCount,
                    1,
                    1);
                if (validation.passed) {
                    search.lowerBoundAccepted = true;
                    search.lowerBoundState = std::move(lowerBoundState);
                    search.lowerBoundValidation = std::move(validation);
                    report.stopReason = SurfaceReductionStopReason::LowerBoundAccepted;
                    report.archivedStateCount = static_cast<int>(archivedStates.size());
                    report.uniqueGroupSetCount = uniqueGroupSetCount();
                    return search;
                }
                const auto rejectedFaces = validation.failedFaces.empty()
                    ? faces
                    : validation.failedFaces;
                search.rejectedGroups.try_emplace(
                    rejectedFaces,
                    SurfaceReductionRejectedGroup {
                        rejectedFaces,
                        validation.failureStage,
                        validation.message});
                archivedStates.erase(stateSignature(lowerBoundState));
            }
        }
    }
    bool stopSearch = false;
    const auto recordSearchIteration = [&](int continuingStateCount, int bestFaceCount) {
        report.searchIterations.push_back({
            searchIteration,
            continuingStateCount,
            bestFaceCount,
            static_cast<int>(archivedStates.size()),
            report.restrictedFitEvaluationCount,
            report.clusterClusterProposals.selectedCount,
            report.clusterSingletonProposals.selectedCount,
            report.singletonSingletonProposals.selectedCount,
            report.skipTripleProposals.selectedCount});
    };
    while (!frontier.empty() &&
           (minimizeFaceCount ||
            static_cast<int>(frontier.front().clusters.size()) > targetCount) &&
           !stopSearch) {
        if (searchTimeExpired()) {
            report.searchTimeBudgetExhausted = true;
            report.stopReason = SurfaceReductionStopReason::SearchTimeBudgetExhausted;
            break;
        }
        ++searchIteration;
        publishProgress(
            execution,
            SurfaceReductionProgressStage::Searching,
            report,
            targetCount,
            static_cast<int>(frontier.front().clusters.size()),
            request.options.maximumRestrictedFitEvaluations,
            "正在展开确定性束搜索层。",
            searchIteration);
        report.expandedStateCount += static_cast<int>(frontier.size());
        std::vector<SearchState> children;
        for (const auto& state : frontier) {
            if (searchTimeExpired()) {
                report.searchTimeBudgetExhausted = true;
                report.stopReason = SurfaceReductionStopReason::SearchTimeBudgetExhausted;
                stopSearch = true;
                break;
            }
            std::vector<PairCandidate> queue;
            {
                ScopedMilliseconds timer(report.timing.pairQueueMs);
                queue = buildPairQueue(
                    document,
                    prepared,
                    state.clusters,
                    mergeBlockedEdges,
                    minimizeFaceCount
                        ? 1
                        : std::max(
                            1,
                            std::min(
                                8,
                                request.options.maximumPairExpansionsPerState / 2)));
            }
            if (minimizeFaceCount) {
                for (const auto& proposal : queue) {
                    auto& statistics = proposalStatistics(report, proposal.type);
                    ++statistics.generatedCount;
                    statistics.maximumCombinedOriginalFaceCount = std::max(
                        statistics.maximumCombinedOriginalFaceCount,
                        proposal.combinedOriginalFaceCount);
                }
            }
            std::vector<bool> exact(queue.size(), false);
            std::vector<bool> knownSuccessfulFit(queue.size(), false);
            std::vector<bool> eligible(queue.size(), true);
            const bool warmSeedFromPersistentCache =
                minimizeFaceCount &&
                state.clusters.size() == faces.size() &&
                execution.cacheMode == SurfaceReductionCacheMode::ReadWrite;
            for (std::size_t pairIndex = 0; pairIndex < queue.size(); ++pairIndex) {
                const auto& pair = queue[pairIndex];
                const auto groupedFaces = mergedFaces(
                    state.clusters[static_cast<std::size_t>(pair.first)],
                    state.clusters[static_cast<std::size_t>(pair.second)]);
                if (execution.maximumProposalGroupSize > 0 &&
                    static_cast<int>(groupedFaces.size()) > execution.maximumProposalGroupSize) {
                    eligible[pairIndex] = false;
                    continue;
                }
                const auto cachedFit = fitCache.find(groupedFaces);
                if (cachedFit != fitCache.end()) {
                    exact[pairIndex] = cachedFit->second.success &&
                        cachedFit->second.method == SurfaceReductionMergeMethod::ExactSameDomain;
                    knownSuccessfulFit[pairIndex] =
                        cachedFit->second.success &&
                        !cachedFit->second.face.IsNull() &&
                        !cachedFit->second.surface.IsNull();
                    continue;
                }
                auto exactResult = exactCache.find(groupedFaces);
                if (exactResult == exactCache.end()) {
                    ++report.exactSameDomainAttemptCount;
                    ScopedMilliseconds timer(report.timing.exactMergeMs);
                    exactResult = exactCache.emplace(
                        groupedFaces, tryExactSameDomainGroup(document, groupedFaces)).first;
                    if (!exactResult->second.IsNull()) {
                        ++report.exactSameDomainSuccessCount;
                    }
                }
                exact[pairIndex] = !exactResult->second.IsNull();
                if (warmSeedFromPersistentCache && !exact[pairIndex]) {
                    CachedFit loaded;
                    loaded.method = SurfaceReductionMergeMethod::RestrictedBSpline;
                    if (loadPersistent(groupedFaces, fitOptions, loaded)) {
                        fitCache.emplace(groupedFaces, loaded);
                        knownSuccessfulFit[pairIndex] =
                            loaded.success && !loaded.face.IsNull() &&
                            !loaded.surface.IsNull();
                    }
                    if (!knownSuccessfulFit[pairIndex] && hasScanReference) {
                        auto stepOnlyOptions = fitOptions;
                        stepOnlyOptions.scanReference = nullptr;
                        stepOnlyOptions.scanRegionIndex = nullptr;
                        stepOnlyOptions.scanReferenceCoverage =
                            ScanReferenceCoverage::None;
                        CachedFit stepOnly;
                        stepOnly.method =
                            SurfaceReductionMergeMethod::RestrictedBSpline;
                        if (loadPersistent(
                                groupedFaces, stepOnlyOptions, stepOnly)) {
                            stepOnlyFitCache.emplace(groupedFaces, stepOnly);
                            knownSuccessfulFit[pairIndex] =
                                stepOnly.success && !stepOnly.face.IsNull() &&
                                !stepOnly.surface.IsNull();
                        }
                    }
                }
            }

            std::vector<std::size_t> pairOrder;
            int initialPairCount = 0;
            if (minimizeFaceCount) {
                const int reservedTripleSlots = std::min(
                    request.options.maximumSkipTripleProposalsPerState,
                    request.options.maximumPairExpansionsPerState);
                const int binaryLimit = std::max(
                    0,
                    request.options.maximumPairExpansionsPerState -
                        reservedTripleSlots);
                pairOrder = minimumPairOrder(
                    queue,
                    exact,
                    knownSuccessfulFit,
                    eligible,
                    request.options.maximumPairExpansionsPerState,
                    state.clusters.size() == faces.size());
                initialPairCount = std::min(
                    binaryLimit, static_cast<int>(pairOrder.size()));
            } else {
                pairOrder.reserve(queue.size());
                for (std::size_t index = 0; index < queue.size(); ++index) {
                    if (eligible[index] && exact[index]) pairOrder.push_back(index);
                }
                for (std::size_t index = 0; index < queue.size(); ++index) {
                    if (eligible[index] && !exact[index]) pairOrder.push_back(index);
                }
                initialPairCount = static_cast<int>(pairOrder.size());
            }

            const int completedMergeCount =
                static_cast<int>(faces.size() - state.clusters.size());
            const int validExpansionQuota = completedMergeCount == 0
                ? std::min(3, request.options.maximumPairExpansionsPerState)
                : 1;
            const int transitionalExpansionQuota =
                request.options.maximumPairExpansionsPerState - validExpansionQuota;
            std::vector<SearchState> validStateChildren;
            std::vector<SearchState> transitionalStateChildren;
            std::vector<int> selectedProposalCount(state.clusters.size(), 0);
            std::vector<int> successfulProposalCount(state.clusters.size(), 0);
            const auto evaluatePairProposal = [&](std::size_t orderPosition) {
                if (searchTimeExpired()) {
                    report.searchTimeBudgetExhausted = true;
                    report.stopReason = SurfaceReductionStopReason::SearchTimeBudgetExhausted;
                    stopSearch = true;
                    return false;
                }
                const auto pairIndex = pairOrder[orderPosition];
                if (static_cast<int>(validStateChildren.size()) >= validExpansionQuota &&
                    static_cast<int>(validStateChildren.size() + transitionalStateChildren.size()) >=
                        request.options.maximumPairExpansionsPerState) {
                    return false;
                }
                ++report.attemptedMergeCount;
                const auto& pair = queue[pairIndex];
                if (minimizeFaceCount) {
                    ++proposalStatistics(report, pair.type).selectedCount;
                    ++selectedProposalCount[static_cast<std::size_t>(pair.first)];
                    ++selectedProposalCount[static_cast<std::size_t>(pair.second)];
                }
                const auto groupedFaces = mergedFaces(
                    state.clusters[static_cast<std::size_t>(pair.first)],
                    state.clusters[static_cast<std::size_t>(pair.second)]);
                usedGroupSets.emplace(false, groupedFaces);

                const int restrictedFitCountBefore = report.restrictedFitEvaluationCount;
                int attributedBatchFitCount = 0;
                bool proposalCacheHit = false;
                CachedFit cached;
                const auto cachedIterator = fitCache.find(groupedFaces);
                if (cachedIterator != fitCache.end()) {
                    cached = cachedIterator->second;
                    ++report.fitCacheHitCount;
                    proposalCacheHit = true;
                } else if (exact[pairIndex]) {
                    cached.face = exactCache[groupedFaces];
                    cached.surface = BRep_Tool::Surface(cached.face);
                    cached.success = !cached.face.IsNull() && !cached.surface.IsNull();
                    cached.method = SurfaceReductionMergeMethod::ExactSameDomain;
                    cached.report.success = cached.success;
                    cached.report.parameterizationValid = cached.success;
                    cached.report.selectionPassed = cached.success;
                    cached.report.holdoutPassed = cached.success;
                    cached.report.brepCheckValid = cached.success;
                    cached.report.message = "同域面已执行无损精确合并。";
                    fitCache.emplace(groupedFaces, cached);
                } else {
                    cached.method = SurfaceReductionMergeMethod::RestrictedBSpline;
                    if (execution.fitWorkerCount > 1 &&
                        execution.cacheMode != SurfaceReductionCacheMode::ReadOnlyRequired) {
                        std::vector<std::vector<FaceId>> batchGroups;
                        std::map<std::vector<FaceId>, PairProposalType> batchProposalTypes;
                        for (std::size_t futurePosition = orderPosition;
                             futurePosition < pairOrder.size() &&
                             batchGroups.size() < static_cast<std::size_t>(execution.fitWorkerCount);
                             ++futurePosition) {
                            const auto futurePairIndex = pairOrder[futurePosition];
                            if (exact[futurePairIndex]) continue;
                            const auto& futurePair = queue[futurePairIndex];
                            const auto futureFaces = mergedFaces(
                                state.clusters[static_cast<std::size_t>(futurePair.first)],
                                state.clusters[static_cast<std::size_t>(futurePair.second)]);
                            if (fitCache.contains(futureFaces) ||
                                std::find(batchGroups.begin(), batchGroups.end(), futureFaces) != batchGroups.end()) {
                                continue;
                            }
                            batchGroups.push_back(futureFaces);
                            batchProposalTypes.emplace(futureFaces, futurePair.type);
                        }
                        std::sort(batchGroups.begin(), batchGroups.end());
                        const auto profileStart = report.fitProfiles.size();
                        std::vector<std::vector<FaceId>> computeGroups;
                        for (const auto& batchFaces : batchGroups) {
                            CachedFit loaded;
                            loaded.method = SurfaceReductionMergeMethod::RestrictedBSpline;
                            if (loadPersistent(batchFaces, fitOptions, loaded)) {
                                fitCache.emplace(batchFaces, std::move(loaded));
                                if (batchFaces == groupedFaces) {
                                    proposalCacheHit = true;
                                }
                            } else if (!report.persistentFitCacheRequiredMiss &&
                                       report.fitCacheMissCount + static_cast<int>(computeGroups.size()) <
                                           request.options.maximumRestrictedFitEvaluations) {
                                computeGroups.push_back(batchFaces);
                            }
                        }
                        if (!report.persistentFitCacheRequiredMiss) {
                            if (!computeGroups.empty()) {
                                publishProgress(
                                    execution,
                                    SurfaceReductionProgressStage::Fitting,
                                    report,
                                    targetCount,
                                    static_cast<int>(state.clusters.size()),
                                    request.options.maximumRestrictedFitEvaluations,
                                    "正在并行计算受限 B-spline 拟合批次。",
                                    searchIteration,
                                    computeGroups.front());
                            }
                            const auto batchStarted = std::chrono::steady_clock::now();
                            std::vector<std::future<RestrictedBSplinePatchFitResult>> futures;
                            futures.reserve(computeGroups.size());
                            for (const auto& batchFaces : computeGroups) {
                                futures.push_back(std::async(
                                    std::launch::async,
                                    [&document, &fitOptions, batchFaces] {
                                        return RestrictedBSplinePatchFitter().fit(
                                            document, batchFaces, fitOptions);
                                    }));
                            }
                            for (std::size_t futureIndex = 0; futureIndex < futures.size(); ++futureIndex) {
                                const auto fit = futures[futureIndex].get();
                                CachedFit prefetched;
                                prefetched.success = fit.success();
                                prefetched.face = fit.face;
                                prefetched.surface = fit.surface;
                                prefetched.report = fit.report;
                                prefetched.method = SurfaceReductionMergeMethod::RestrictedBSpline;
                                ++report.fitCacheMissCount;
                                ++report.restrictedFitEvaluationCount;
                                recordFitProfile(computeGroups[futureIndex], prefetched, false);
                                storePersistent(computeGroups[futureIndex], fitOptions, prefetched);
                                fitCache.emplace(computeGroups[futureIndex], std::move(prefetched));
                                prefetchedGroupSets.emplace(
                                    false, computeGroups[futureIndex]);
                                publishProgress(
                                    execution,
                                    SurfaceReductionProgressStage::Fitting,
                                    report,
                                    targetCount,
                                    static_cast<int>(state.clusters.size()),
                                    request.options.maximumRestrictedFitEvaluations,
                                    "受限 B-spline 拟合已完成一个分组。",
                                    searchIteration,
                                    computeGroups[futureIndex]);
                            }
                            report.timing.restrictedFitMs += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - batchStarted).count();
                            prefetchStepOnlyFits(batchGroups);
                            if (minimizeFaceCount) {
                                for (std::size_t profileIndex = profileStart;
                                     profileIndex < report.fitProfiles.size();
                                     ++profileIndex) {
                                    const auto& profile = report.fitProfiles[profileIndex];
                                    if (profile.persistentCacheHit) {
                                        continue;
                                    }
                                    const auto type = batchProposalTypes.find(profile.faces);
                                    if (type == batchProposalTypes.end()) {
                                        continue;
                                    }
                                    ++proposalStatistics(
                                        report, type->second).restrictedFitCount;
                                    ++attributedBatchFitCount;
                                }
                            }
                        }
                    }
                    const auto prefetched = fitCache.find(groupedFaces);
                    if (prefetched != fitCache.end()) {
                        cached = prefetched->second;
                    } else {
                        const bool persistentLoaded =
                            loadPersistent(groupedFaces, fitOptions, cached);
                        proposalCacheHit = proposalCacheHit || persistentLoaded;
                        if (!persistentLoaded) {
                            if (report.persistentFitCacheRequiredMiss) {
                                stopSearch = true;
                                return false;
                            }
                            if (report.fitCacheMissCount >=
                                request.options.maximumRestrictedFitEvaluations) {
                                report.searchBudgetExhausted = true;
                                if (minimizeFaceCount) {
                                    return true;
                                }
                                stopSearch = true;
                                return false;
                            }
                            publishProgress(
                                execution,
                                SurfaceReductionProgressStage::Fitting,
                                report,
                                targetCount,
                                static_cast<int>(state.clusters.size()),
                                request.options.maximumRestrictedFitEvaluations,
                                "正在计算受限 B-spline 拟合。",
                                searchIteration,
                                groupedFaces);
                            const auto fit = RestrictedBSplinePatchFitter().fit(
                                document, groupedFaces, fitOptions);
                            ++report.fitCacheMissCount;
                            ++report.restrictedFitEvaluationCount;
                            cached.success = fit.success();
                            cached.face = fit.face;
                            cached.surface = fit.surface;
                            cached.report = fit.report;
                            report.timing.restrictedFitMs += fit.report.timing.totalMs;
                            recordFitProfile(groupedFaces, cached, false);
                            storePersistent(groupedFaces, fitOptions, cached);
                            publishProgress(
                                execution,
                                SurfaceReductionProgressStage::Fitting,
                                report,
                                targetCount,
                                static_cast<int>(state.clusters.size()),
                                request.options.maximumRestrictedFitEvaluations,
                                "受限 B-spline 拟合完成。",
                                searchIteration,
                                groupedFaces);
                        }
                    }
                    fitCache.try_emplace(groupedFaces, cached);
                }

                if ((!cached.success || cached.face.IsNull() || cached.surface.IsNull()) &&
                    hasScanReference &&
                    cached.method == SurfaceReductionMergeMethod::RestrictedBSpline) {
                    const int fallbackCacheHitsBefore =
                        report.fitCacheHitCount + report.persistentFitCacheHitCount;
                    const auto* fallback = stepOnlyFallback(groupedFaces);
                    proposalCacheHit = proposalCacheHit ||
                        report.fitCacheHitCount + report.persistentFitCacheHitCount >
                            fallbackCacheHitsBefore;
                    if (fallback != nullptr && fallback->success &&
                        !fallback->face.IsNull() && !fallback->surface.IsNull()) {
                        cached = *fallback;
                    }
                }
                const auto recordProposalEvaluation = [&] {
                    if (!minimizeFaceCount) {
                        return;
                    }
                    auto& statistics = proposalStatistics(report, pair.type);
                    statistics.restrictedFitCount += std::max(
                        0,
                        report.restrictedFitEvaluationCount -
                            restrictedFitCountBefore - attributedBatchFitCount);
                    if (proposalCacheHit) {
                        ++statistics.cacheHitCount;
                    }
                };
                if (!cached.success || cached.face.IsNull() || cached.surface.IsNull()) {
                    recordProposalEvaluation();
                    if (minimizeFaceCount) {
                        ++proposalStatistics(report, pair.type).rejectedCount;
                    }
                    search.rejectedGroups.try_emplace(
                        groupedFaces,
                        SurfaceReductionRejectedGroup {
                            groupedFaces,
                            mapFailureStage(cached.report.failureStage),
                            cached.report.message,
                            cached.report});
                    return true;
                }

                auto candidateClusters = state.clusters;
                candidateClusters[static_cast<std::size_t>(pair.first)] = Cluster {
                    groupedFaces,
                    cached.face,
                    cached.surface,
                    cached.report,
                    cached.method};
                candidateClusters.erase(candidateClusters.begin() + pair.second);
                auto candidateSeamAngles = state.seamAngles;
                SeamQuality seam;
                {
                    ScopedMilliseconds timer(report.timing.seamValidationMs);
                    seam = updateClusterSeams(
                        document,
                        prepared,
                        candidateClusters,
                        groupedFaces,
                        nonSmoothSeamEdges,
                        request.options.smoothSeamToleranceDegrees,
                        candidateSeamAngles);
                }
                if (!seam.valid && hasScanReference &&
                    cached.method == SurfaceReductionMergeMethod::RestrictedBSpline &&
                    cached.report.scanReferenceSampleCount > 0) {
                    const int fallbackCacheHitsBefore =
                        report.fitCacheHitCount + report.persistentFitCacheHitCount;
                    const auto* fallback = stepOnlyFallback(groupedFaces);
                    proposalCacheHit = proposalCacheHit ||
                        report.fitCacheHitCount + report.persistentFitCacheHitCount >
                            fallbackCacheHitsBefore;
                    if (fallback != nullptr && fallback->success &&
                        !fallback->face.IsNull() && !fallback->surface.IsNull()) {
                        auto fallbackClusters = state.clusters;
                        fallbackClusters[static_cast<std::size_t>(pair.first)] = Cluster {
                            groupedFaces,
                            fallback->face,
                            fallback->surface,
                            fallback->report,
                            fallback->method};
                        fallbackClusters.erase(fallbackClusters.begin() + pair.second);
                        auto fallbackSeamAngles = state.seamAngles;
                        SeamQuality fallbackSeam;
                        {
                            ScopedMilliseconds timer(report.timing.seamValidationMs);
                            fallbackSeam = updateClusterSeams(
                                document,
                                prepared,
                                fallbackClusters,
                                groupedFaces,
                                nonSmoothSeamEdges,
                                request.options.smoothSeamToleranceDegrees,
                                fallbackSeamAngles);
                        }
                        if ((fallbackSeam.valid && !seam.valid) ||
                            fallbackSeam.maximumAngle < seam.maximumAngle) {
                            cached = *fallback;
                            candidateClusters = std::move(fallbackClusters);
                            candidateSeamAngles = std::move(fallbackSeamAngles);
                            seam = fallbackSeam;
                        }
                    }
                }
                recordProposalEvaluation();
                SearchState child;
                child.clusters = std::move(candidateClusters);
                child.seamAngles = std::move(candidateSeamAngles);
                child.maximumSeamAngle = seam.maximumAngle;
                if (minimizeFaceCount) {
                    ++proposalStatistics(report, pair.type).successCount;
                    ++successfulProposalCount[static_cast<std::size_t>(pair.first)];
                    ++successfulProposalCount[static_cast<std::size_t>(pair.second)];
                }
                if (seam.valid) {
                    validStateChildren.push_back(std::move(child));
                } else {
                    transitionalStateChildren.push_back(std::move(child));
                }
                return true;
            };
            bool pairProcessingOpen = true;
            for (int orderPosition = 0;
                 orderPosition < initialPairCount && pairProcessingOpen;
                 ++orderPosition) {
                pairProcessingOpen = evaluatePairProposal(
                    static_cast<std::size_t>(orderPosition));
            }
            int selectedTripleProposalCount = 0;
            if (minimizeFaceCount && !stopSearch &&
                request.options.maximumSkipTripleProposalsPerState > 0) {
                struct TripleProposal {
                    std::vector<int> clusterIndices;
                    std::vector<FaceId> faces;
                    double normalJump = 0.0;
                    double curvatureDifference = 0.0;
                    double sharedLength = 0.0;
                };
                std::map<std::pair<int, int>, const PairCandidate*> adjacencyPairs;
                for (const auto& pair : queue) {
                    adjacencyPairs[{pair.first, pair.second}] = &pair;
                }
                const auto pairBetween = [&](int first, int second) -> const PairCandidate* {
                    const auto key = std::minmax(first, second);
                    const auto found = adjacencyPairs.find({key.first, key.second});
                    return found == adjacencyPairs.end() ? nullptr : found->second;
                };
                std::map<std::vector<FaceId>, TripleProposal> uniqueTriples;
                for (int anchor = 0; anchor < static_cast<int>(state.clusters.size()); ++anchor) {
                    if (selectedProposalCount[static_cast<std::size_t>(anchor)] == 0 ||
                        successfulProposalCount[static_cast<std::size_t>(anchor)] > 0) {
                        continue;
                    }
                    for (int second = 0; second < static_cast<int>(state.clusters.size()); ++second) {
                        if (second == anchor) {
                            continue;
                        }
                        for (int third = second + 1;
                             third < static_cast<int>(state.clusters.size());
                             ++third) {
                            if (third == anchor) {
                                continue;
                            }
                            const auto* anchorSecond = pairBetween(anchor, second);
                            const auto* anchorThird = pairBetween(anchor, third);
                            const auto* secondThird = pairBetween(second, third);
                            const int connectedEdges =
                                (anchorSecond != nullptr ? 1 : 0) +
                                (anchorThird != nullptr ? 1 : 0) +
                                (secondThird != nullptr ? 1 : 0);
                            if (connectedEdges < 2 ||
                                (anchorSecond == nullptr && anchorThird == nullptr)) {
                                continue;
                            }
                            std::vector<int> indices {anchor, second, third};
                            std::sort(indices.begin(), indices.end());
                            std::vector<FaceId> groupedFaces;
                            TripleProposal proposal;
                            proposal.clusterIndices = indices;
                            for (const auto index : indices) {
                                const auto& cluster =
                                    state.clusters[static_cast<std::size_t>(index)];
                                groupedFaces.insert(
                                    groupedFaces.end(),
                                    cluster.faces.begin(),
                                    cluster.faces.end());
                            }
                            proposal.faces = normalizedFaces(groupedFaces);
                            if (internalizesProtectedEdge(
                                    document, proposal.faces, mergeBlockedEdges)) {
                                continue;
                            }
                            for (const auto* edge : {anchorSecond, anchorThird, secondThird}) {
                                if (edge == nullptr) {
                                    continue;
                                }
                                proposal.normalJump = std::max(
                                    proposal.normalJump, edge->normalJump);
                                proposal.curvatureDifference = std::max(
                                    proposal.curvatureDifference,
                                    edge->curvatureDifference);
                                proposal.sharedLength += edge->sharedLength;
                            }
                            uniqueTriples.try_emplace(proposal.faces, std::move(proposal));
                        }
                    }
                }
                std::vector<TripleProposal> tripleQueue;
                for (auto& [groupedFaces, proposal] : uniqueTriples) {
                    tripleQueue.push_back(std::move(proposal));
                }
                std::sort(
                    tripleQueue.begin(),
                    tripleQueue.end(),
                    [](const auto& first, const auto& second) {
                        return std::make_tuple(
                                   first.normalJump,
                                   first.curvatureDifference,
                                   -first.sharedLength,
                                   -static_cast<int>(first.faces.size()),
                                   first.faces) <
                            std::make_tuple(
                                   second.normalJump,
                                   second.curvatureDifference,
                                   -second.sharedLength,
                                   -static_cast<int>(second.faces.size()),
                                   second.faces);
                    });
                report.skipTripleProposals.generatedCount +=
                    static_cast<int>(tripleQueue.size());
                for (const auto& proposal : tripleQueue) {
                    report.skipTripleProposals.maximumCombinedOriginalFaceCount = std::max(
                        report.skipTripleProposals.maximumCombinedOriginalFaceCount,
                        static_cast<int>(proposal.faces.size()));
                }
                const int tripleLimit = std::min(
                    request.options.maximumSkipTripleProposalsPerState,
                    static_cast<int>(tripleQueue.size()));
                selectedTripleProposalCount = tripleLimit;
                report.proposalLimitPrunedCount +=
                    static_cast<int>(tripleQueue.size()) - tripleLimit;
                for (int tripleIndex = 0; tripleIndex < tripleLimit; ++tripleIndex) {
                    if (searchTimeExpired()) {
                        report.searchTimeBudgetExhausted = true;
                        report.stopReason =
                            SurfaceReductionStopReason::SearchTimeBudgetExhausted;
                        stopSearch = true;
                        break;
                    }
                    const auto& proposal =
                        tripleQueue[static_cast<std::size_t>(tripleIndex)];
                    ++report.attemptedMergeCount;
                    ++report.skipTripleProposals.selectedCount;
                    const int restrictedFitCountBefore =
                        report.restrictedFitEvaluationCount;
                    const int cacheHitCountBefore =
                        report.fitCacheHitCount + report.persistentFitCacheHitCount;
                    const auto evaluated = evaluateGroup(proposal.faces);
                    if (report.persistentFitCacheRequiredMiss) {
                        stopSearch = true;
                        break;
                    }
                    report.skipTripleProposals.restrictedFitCount +=
                        report.restrictedFitEvaluationCount - restrictedFitCountBefore;
                    if (report.fitCacheHitCount + report.persistentFitCacheHitCount >
                        cacheHitCountBefore) {
                        ++report.skipTripleProposals.cacheHitCount;
                    }
                    if (!evaluated.has_value() || !evaluated->success ||
                        evaluated->face.IsNull() || evaluated->surface.IsNull()) {
                        if (!report.searchBudgetExhausted) {
                            ++report.skipTripleProposals.rejectedCount;
                        }
                        if (evaluated.has_value()) {
                            search.rejectedGroups.try_emplace(
                                proposal.faces,
                                SurfaceReductionRejectedGroup {
                                    proposal.faces,
                                    mapFailureStage(evaluated->report.failureStage),
                                    evaluated->report.message,
                                    evaluated->report});
                        }
                        continue;
                    }
                    auto candidateClusters = state.clusters;
                    const int targetIndex = proposal.clusterIndices.front();
                    candidateClusters[static_cast<std::size_t>(targetIndex)] = Cluster {
                        proposal.faces,
                        evaluated->face,
                        evaluated->surface,
                        evaluated->report,
                        evaluated->method};
                    for (auto index = proposal.clusterIndices.rbegin();
                         index != proposal.clusterIndices.rend();
                         ++index) {
                        if (*index != targetIndex) {
                            candidateClusters.erase(candidateClusters.begin() + *index);
                        }
                    }
                    auto candidateSeamAngles = state.seamAngles;
                    SeamQuality seam;
                    {
                        ScopedMilliseconds timer(report.timing.seamValidationMs);
                        seam = updateClusterSeams(
                            document,
                            prepared,
                            candidateClusters,
                            proposal.faces,
                            nonSmoothSeamEdges,
                            request.options.smoothSeamToleranceDegrees,
                            candidateSeamAngles);
                    }
                    SearchState child;
                    child.clusters = std::move(candidateClusters);
                    child.seamAngles = std::move(candidateSeamAngles);
                    child.maximumSeamAngle = seam.maximumAngle;
                    ++report.skipTripleProposals.successCount;
                    if (seam.valid) {
                        validStateChildren.push_back(std::move(child));
                    } else {
                        transitionalStateChildren.push_back(std::move(child));
                    }
                }
            }
            if (minimizeFaceCount && !stopSearch && pairProcessingOpen) {
                const int backfillCount = std::min({
                    request.options.maximumSkipTripleProposalsPerState -
                        selectedTripleProposalCount,
                    static_cast<int>(pairOrder.size()) - initialPairCount,
                    request.options.maximumPairExpansionsPerState -
                        initialPairCount - selectedTripleProposalCount});
                for (int offset = 0;
                     offset < backfillCount && pairProcessingOpen;
                     ++offset) {
                    pairProcessingOpen = evaluatePairProposal(
                        static_cast<std::size_t>(initialPairCount + offset));
                }
                const int eligibleCount = static_cast<int>(std::count(
                    eligible.begin(), eligible.end(), true));
                const int allowedBinaryCount = std::min(
                    eligibleCount, initialPairCount + backfillCount);
                report.proposalLimitPrunedCount += std::max(
                    0, eligibleCount - allowedBinaryCount);
            }
            if (stopSearch) {
                break;
            }
            const auto rankChildren = [](std::vector<SearchState>& states) {
                std::sort(states.begin(), states.end(), [](const auto& first, const auto& second) {
                    return stateRank(first) < stateRank(second);
                });
            };
            rankChildren(validStateChildren);
            rankChildren(transitionalStateChildren);
            const int maximumExpansions = request.options.maximumPairExpansionsPerState;
            const auto childStart = children.size();
            if (minimizeFaceCount) {
                int validIndex = 0;
                int transitionalIndex = 0;
                if (!validStateChildren.empty()) {
                    children.push_back(std::move(validStateChildren.front()));
                    validIndex = 1;
                }
                if (!transitionalStateChildren.empty() &&
                    static_cast<int>(children.size() - childStart) < maximumExpansions) {
                    children.push_back(std::move(transitionalStateChildren.front()));
                    transitionalIndex = 1;
                }
                while (static_cast<int>(children.size() - childStart) < maximumExpansions) {
                    const bool hasValid =
                        validIndex < static_cast<int>(validStateChildren.size());
                    const bool hasTransitional =
                        transitionalIndex <
                        static_cast<int>(transitionalStateChildren.size());
                    if (!hasValid && !hasTransitional) {
                        break;
                    }
                    if (hasValid &&
                        (!hasTransitional ||
                         stateRank(validStateChildren[static_cast<std::size_t>(validIndex)]) <
                             stateRank(transitionalStateChildren[
                                 static_cast<std::size_t>(transitionalIndex)]))) {
                        children.push_back(std::move(
                            validStateChildren[static_cast<std::size_t>(validIndex++)]));
                    } else {
                        children.push_back(std::move(
                            transitionalStateChildren[
                                static_cast<std::size_t>(transitionalIndex++)]));
                    }
                }
            } else {
                const int initialValidCount = std::min(
                    validExpansionQuota,
                    static_cast<int>(validStateChildren.size()));
                const int initialTransitionalCount = std::min(
                    transitionalExpansionQuota,
                    static_cast<int>(transitionalStateChildren.size()));
                for (int index = 0; index < initialValidCount; ++index) {
                    children.push_back(std::move(
                        validStateChildren[static_cast<std::size_t>(index)]));
                }
                for (int index = 0; index < initialTransitionalCount; ++index) {
                    children.push_back(std::move(
                        transitionalStateChildren[static_cast<std::size_t>(index)]));
                }
                int validIndex = initialValidCount;
                int transitionalIndex = initialTransitionalCount;
                while (static_cast<int>(children.size() - childStart) < maximumExpansions) {
                    if (validIndex < static_cast<int>(validStateChildren.size())) {
                        children.push_back(std::move(
                            validStateChildren[static_cast<std::size_t>(validIndex++)]));
                    } else if (transitionalIndex <
                               static_cast<int>(transitionalStateChildren.size())) {
                        children.push_back(std::move(
                            transitionalStateChildren[
                                static_cast<std::size_t>(transitionalIndex++)]));
                    } else {
                        break;
                    }
                }
            }
            report.generatedStateCount +=
                static_cast<int>(children.size() - childStart);
        }
        if (children.empty()) {
            recordSearchIteration(
                0,
                frontier.empty()
                    ? static_cast<int>(faces.size())
                    : static_cast<int>(frontier.front().clusters.size()));
            break;
        }

        std::map<std::string, SearchState> uniqueChildren;
        for (auto& child : children) {
            const auto signature = stateSignature(child);
            const auto existing = uniqueChildren.find(signature);
            if (existing == uniqueChildren.end() || stateRank(child) < stateRank(existing->second)) {
                uniqueChildren[signature] = std::move(child);
            }
        }
        report.deduplicatedStateCount +=
            static_cast<int>(children.size() - uniqueChildren.size());
        frontier.clear();
        for (auto& [signature, state] : uniqueChildren) {
            frontier.push_back(std::move(state));
        }
        std::sort(frontier.begin(), frontier.end(), [](const auto& first, const auto& second) {
            return stateRank(first) < stateRank(second);
        });
        for (const auto& state : frontier) {
            archivedStates.try_emplace(stateSignature(state), state);
        }
        if (static_cast<int>(frontier.size()) > request.options.beamWidth) {
            report.beamPrunedStateCount +=
                static_cast<int>(frontier.size()) - request.options.beamWidth;
            frontier.resize(static_cast<std::size_t>(request.options.beamWidth));
        }
        recordSearchIteration(
            static_cast<int>(frontier.size()),
            frontier.empty()
                ? static_cast<int>(faces.size())
                : static_cast<int>(frontier.front().clusters.size()));
        if (!frontier.empty()) {
            publishProgress(
                execution,
                SurfaceReductionProgressStage::Searching,
                report,
                targetCount,
                static_cast<int>(frontier.front().clusters.size()),
                request.options.maximumRestrictedFitEvaluations,
                "当前束搜索层完成，正在进入下一层。",
                searchIteration);
        }
    }

    search.finalCandidates.reserve(archivedStates.size());
    for (const auto& [signature, state] : archivedStates) {
        if (state.clusters.size() < faces.size()) {
            search.finalCandidates.push_back(state);
        }
    }
    std::sort(
        search.finalCandidates.begin(),
        search.finalCandidates.end(),
        [](const auto& first, const auto& second) {
            return stateRank(first) < stateRank(second);
        });
    report.archivedStateCount = static_cast<int>(archivedStates.size());
    report.uniqueGroupSetCount = uniqueGroupSetCount();
    report.speculativePrefetchCount = static_cast<int>(prefetchedGroupSets.size());
    report.unusedPrefetchCount = static_cast<int>(std::count_if(
        prefetchedGroupSets.begin(),
        prefetchedGroupSets.end(),
        [&](const auto& variant) {
            return !usedGroupSets.contains(variant);
        }));
    if (report.persistentFitCacheRequiredMiss) {
        report.stopReason = SurfaceReductionStopReason::RequiredCacheMiss;
    } else if (report.searchTimeBudgetExhausted) {
        report.stopReason = SurfaceReductionStopReason::SearchTimeBudgetExhausted;
    } else if (report.searchBudgetExhausted) {
        report.stopReason = SurfaceReductionStopReason::FitBudgetExhausted;
    } else if (request.options.searchObjective ==
               SurfaceReductionSearchObjective::TargetRatio &&
               !frontier.empty() &&
               static_cast<int>(frontier.front().clusters.size()) <= targetCount) {
        report.stopReason = SurfaceReductionStopReason::TargetReached;
    } else if (report.stopReason == SurfaceReductionStopReason::None) {
        report.stopReason = SurfaceReductionStopReason::FrontierExhausted;
    }
    return search;
}

FinalCandidateSelection validateReductionSearchCandidates(
    const ShapeDocument& document,
    const SurfaceReductionPreparedContext& prepared,
    const std::vector<FaceId>& faces,
    const std::vector<SearchState>& candidates,
    const std::set<EdgeId>& nonSmoothSeamEdges,
    const SurfaceReductionOptions& options,
    const SurfaceReductionExecutionOptions& execution,
    int targetFaceCount,
    SurfaceReductionReport& report,
    std::map<std::vector<FaceId>, SurfaceReductionRejectedGroup>& rejectedGroups) {
    FinalCandidateSelection selection;
    for (std::size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
        const auto& candidateState = candidates[candidateIndex];
        publishProgress(
            execution,
            SurfaceReductionProgressStage::ValidatingSeams,
            report,
            targetFaceCount,
            static_cast<int>(candidateState.clusters.size()),
            options.maximumRestrictedFitEvaluations,
            "正在验证候选组合的保留接缝。",
            0,
            {},
            static_cast<int>(candidateIndex + 1),
            static_cast<int>(candidates.size()));
        SeamQuality seam;
        seam.maximumAngle = candidateState.maximumSeamAngle;
        seam.valid = std::all_of(
            candidateState.seamAngles.begin(),
            candidateState.seamAngles.end(),
            [&](const auto& item) { return item.second <= options.smoothSeamToleranceDegrees; });
        if (!seam.valid) {
            std::vector<FaceId> mergedStateFaces;
            for (const auto& cluster : candidateState.clusters) {
                if (cluster.faces.size() >= 2) {
                    mergedStateFaces.insert(
                        mergedStateFaces.end(),
                        cluster.faces.begin(),
                        cluster.faces.end());
                }
            }
            mergedStateFaces = normalizedFaces(mergedStateFaces);
            rejectedGroups.try_emplace(
                mergedStateFaces,
                SurfaceReductionRejectedGroup {
                    mergedStateFaces,
                    SurfaceReductionFailureStage::Seam,
                    "组合状态中保留的光顺接缝超过配置阈值。"});
            continue;
        }
        if (report.finalValidationAttemptCount >=
            options.maximumFinalValidationEvaluations) {
            report.stopReason =
                SurfaceReductionStopReason::FinalValidationBudgetExhausted;
            break;
        }
        ++report.finalValidationAttemptCount;
        auto validation = validateFinalState(
            document,
            faces,
            candidateState.clusters,
            options,
            report.timing,
            execution,
            report,
            targetFaceCount,
            static_cast<int>(candidateIndex + 1),
            static_cast<int>(candidates.size()));
        if (!validation.passed) {
            if (selection.bestFailure.empty()) {
                selection.bestFailure = validation.message;
                report.commercialCadReports = validation.commercialCadReports;
                report.topologyReport = validation.topologyReport;
            }
            const auto rejectedFaces = validation.failedFaces.empty()
                ? faces
                : validation.failedFaces;
            rejectedGroups.try_emplace(
                rejectedFaces,
                SurfaceReductionRejectedGroup {
                    rejectedFaces,
                    validation.failureStage,
                    validation.message});
            continue;
        }
        selection.accepted = true;
        selection.state = candidateState;
        selection.validation = std::move(validation);
        break;
    }
    return selection;
}

} // namespace

const char* toString(SurfaceReductionOutcome outcome) {
    switch (outcome) {
    case SurfaceReductionOutcome::InvalidSelection:
        return "InvalidSelection";
    case SurfaceReductionOutcome::NoAcceptableReduction:
        return "NoAcceptableReduction";
    case SurfaceReductionOutcome::Reduced:
        return "Reduced";
    case SurfaceReductionOutcome::TargetAchieved:
        return "TargetAchieved";
    }
    return "InvalidSelection";
}

const char* toString(SurfaceReductionSearchObjective objective) {
    switch (objective) {
    case SurfaceReductionSearchObjective::TargetRatio:
        return "TargetRatio";
    case SurfaceReductionSearchObjective::MinimizeFaceCount:
        return "MinimizeFaceCount";
    }
    return "TargetRatio";
}

const char* toString(SurfaceReductionOptimality optimality) {
    switch (optimality) {
    case SurfaceReductionOptimality::NotEvaluated:
        return "NotEvaluated";
    case SurfaceReductionOptimality::OptimalProven:
        return "OptimalProven";
    case SurfaceReductionOptimality::BestFoundWithinBudget:
        return "BestFoundWithinBudget";
    case SurfaceReductionOptimality::NoValidatedResultFound:
        return "NoValidatedResultFound";
    }
    return "NotEvaluated";
}

const char* toString(SurfaceReductionStopReason reason) {
    switch (reason) {
    case SurfaceReductionStopReason::None:
        return "None";
    case SurfaceReductionStopReason::TargetReached:
        return "TargetReached";
    case SurfaceReductionStopReason::LowerBoundAccepted:
        return "LowerBoundAccepted";
    case SurfaceReductionStopReason::FrontierExhausted:
        return "FrontierExhausted";
    case SurfaceReductionStopReason::FitBudgetExhausted:
        return "FitBudgetExhausted";
    case SurfaceReductionStopReason::SearchTimeBudgetExhausted:
        return "SearchTimeBudgetExhausted";
    case SurfaceReductionStopReason::FinalValidationBudgetExhausted:
        return "FinalValidationBudgetExhausted";
    case SurfaceReductionStopReason::RequiredCacheMiss:
        return "RequiredCacheMiss";
    }
    return "None";
}

const char* toString(SurfaceReductionMergeMethod method) {
    switch (method) {
    case SurfaceReductionMergeMethod::OriginalFace:
        return "OriginalFace";
    case SurfaceReductionMergeMethod::ExactSameDomain:
        return "ExactSameDomain";
    case SurfaceReductionMergeMethod::RestrictedBSpline:
        return "RestrictedBSpline";
    }
    return "OriginalFace";
}

const char* toString(SurfaceReductionCacheMode mode) {
    switch (mode) {
    case SurfaceReductionCacheMode::Disabled: return "Disabled";
    case SurfaceReductionCacheMode::ReadWrite: return "ReadWrite";
    case SurfaceReductionCacheMode::ReadOnlyRequired: return "ReadOnlyRequired";
    case SurfaceReductionCacheMode::Refresh: return "Refresh";
    }
    return "Disabled";
}

const char* toString(SurfaceReductionProgressStage stage) {
    switch (stage) {
    case SurfaceReductionProgressStage::ValidatingInput:
        return "ValidatingInput";
    case SurfaceReductionProgressStage::PreparingTopology:
        return "PreparingTopology";
    case SurfaceReductionProgressStage::Searching:
        return "Searching";
    case SurfaceReductionProgressStage::Fitting:
        return "Fitting";
    case SurfaceReductionProgressStage::ValidatingSeams:
        return "ValidatingSeams";
    case SurfaceReductionProgressStage::ValidatingCommercialCad:
        return "ValidatingCommercialCad";
    case SurfaceReductionProgressStage::AssemblingModel:
        return "AssemblingModel";
    case SurfaceReductionProgressStage::ValidatingTopology:
        return "ValidatingTopology";
    case SurfaceReductionProgressStage::Completed:
        return "Completed";
    }
    return "ValidatingInput";
}

const char* toString(SurfaceReductionFailureStage stage) {
    switch (stage) {
    case SurfaceReductionFailureStage::None:
        return "None";
    case SurfaceReductionFailureStage::Parameterization:
        return "Parameterization";
    case SurfaceReductionFailureStage::BoundaryConstraints:
        return "BoundaryConstraints";
    case SurfaceReductionFailureStage::UvIteration:
        return "UvIteration";
    case SurfaceReductionFailureStage::ModelSelection:
        return "ModelSelection";
    case SurfaceReductionFailureStage::Holdout:
        return "Holdout";
    case SurfaceReductionFailureStage::Jacobian:
        return "Jacobian";
    case SurfaceReductionFailureStage::Retrim:
        return "Retrim";
    case SurfaceReductionFailureStage::BoundaryHoldout:
        return "BoundaryHoldout";
    case SurfaceReductionFailureStage::BrepCheck:
        return "BrepCheck";
    case SurfaceReductionFailureStage::Seam:
        return "Seam";
    case SurfaceReductionFailureStage::CommercialCad:
        return "CommercialCad";
    case SurfaceReductionFailureStage::Topology:
        return "Topology";
    case SurfaceReductionFailureStage::StepRoundtrip:
        return "StepRoundtrip";
    }
    return "None";
}

SurfaceReductionResult SurfaceReductionEngine::reduce(
    const ShapeDocument& document,
    const SurfaceReductionRequest& request,
    const SurfaceReductionExecutionOptions& execution) const {
    SurfaceReductionResult result;
    SurfaceReductionReportFinalizer reportFinalizer(result.report);
    result.sourceShape = document.shape();
    result.report.searchObjective = request.options.searchObjective;
    result.report.persistentCacheMode = execution.cacheMode;
    result.report.maximumRestrictedFitEvaluations =
        request.options.maximumRestrictedFitEvaluations;
    result.report.maximumFinalValidationEvaluations =
        request.options.maximumFinalValidationEvaluations;
    result.report.maximumSearchWallTimeMilliseconds =
        execution.maximumSearchWallTimeMilliseconds;
    result.report.scanReferenceCoverage = request.scanReferenceCoverage;
    result.report.fitWorkerCount = std::max(1, execution.fitWorkerCount);
    const auto faces = normalizedFaces(request.selectedFaces);
    result.report.inputFaceCount = static_cast<int>(faces.size());
    result.report.outputFaceCount = static_cast<int>(faces.size());
    if (!document.hasShape() || faces.size() < 2) {
        result.report.message = "选区少于两个面，没有可减少的面数。";
        return result;
    }
    if (!(request.options.tolerance3d > 0.0) ||
        !(request.options.normalToleranceDegrees > 0.0) ||
        !(request.options.smoothSeamToleranceDegrees > 0.0) ||
        (request.options.searchObjective == SurfaceReductionSearchObjective::TargetRatio &&
         !(request.options.targetOutputRatio > 0.0 &&
           request.options.targetOutputRatio < 1.0)) ||
        request.options.maximumControlPoints < 16 ||
        request.options.maximumBoundaryConstraintRefinements < 0 ||
        request.options.maximumUvIterations < 1 ||
        request.options.maximumUvBacktrackingSteps < 1 ||
        request.options.uvConvergenceTolerance <= 0.0 ||
        request.options.beamWidth < 1 ||
        request.options.maximumPairExpansionsPerState < 1 ||
        request.options.maximumSkipTripleProposalsPerState < 0 ||
        request.options.maximumSkipTripleProposalsPerState >
            request.options.maximumPairExpansionsPerState ||
        request.options.maximumRestrictedFitEvaluations < 1 ||
        request.options.maximumFinalValidationEvaluations < 1 ||
        request.options.scanReferenceWeight < 0.0 ||
        request.options.maximumScanReferenceDistance <= 0.0) {
        result.report.message = "曲面减面选项无效：容差和预算必须为正，比例模式目标必须在 0 与 1 之间。";
        return result;
    }
    if (execution.fitWorkerCount < 1 || execution.fitWorkerCount > 64) {
        result.report.message = "拟合工作线程数必须在 1 到 64 之间。";
        return result;
    }
    if (execution.maximumProposalGroupSize < 0 || execution.maximumProposalGroupSize == 1) {
        result.report.message = "渐进提案分组上限必须为 0 或至少为 2。";
        return result;
    }
    if (request.options.searchObjective == SurfaceReductionSearchObjective::MinimizeFaceCount &&
        execution.maximumProposalGroupSize != 0) {
        result.report.message = "最少面数模式不允许限制提案包含的原始面数。";
        return result;
    }
    if (execution.maximumSearchWallTimeMilliseconds < 0) {
        result.report.message = "搜索墙钟预算不能为负数。";
        return result;
    }
    if (execution.cacheMode != SurfaceReductionCacheMode::Disabled &&
        (execution.cacheDirectory.empty() || execution.sourceFingerprint.empty() ||
         execution.algorithmFingerprint.empty() ||
         (request.scanReferenceStl != nullptr && execution.scanFingerprint.empty()))) {
        result.report.message = "启用持久拟合缓存时必须提供缓存目录、源 STEP、扫描 STL 和算法指纹。";
        return result;
    }
    const bool hasScanReference = request.scanReferenceStl != nullptr;
    if (hasScanReference != (request.scanReferenceCoverage != ScanReferenceCoverage::None) ||
        (hasScanReference && request.scanReferenceStl->empty())) {
        result.report.message = "扫描参考与 WholeModel/SelectionCrop 覆盖类型必须同时有效。";
        return result;
    }
    if (std::any_of(faces.begin(), faces.end(), [&](FaceId face) {
            return face >= document.topology().faceCount();
        })) {
        result.report.message = "选区包含不存在的面。";
        return result;
    }
    result.report.selectionConnected = selectionConnected(document, faces);
    if (!result.report.selectionConnected) {
        result.report.message = "首版只接受一个连通选区，请分别处理不连通区域。";
        return result;
    }
    int targetCount = request.options.searchObjective ==
            SurfaceReductionSearchObjective::TargetRatio
        ? std::max(1, static_cast<int>(std::floor(
            static_cast<double>(faces.size()) *
            std::clamp(request.options.targetOutputRatio, 0.0, 1.0))))
        : 1;
    publishProgress(
        execution,
        SurfaceReductionProgressStage::ValidatingInput,
        result.report,
        targetCount,
        static_cast<int>(faces.size()),
        request.options.maximumRestrictedFitEvaluations,
        "候选选区和执行参数验证完成。");
    publishProgress(
        execution,
        SurfaceReductionProgressStage::PreparingTopology,
        result.report,
        targetCount,
        static_cast<int>(faces.size()),
        request.options.maximumRestrictedFitEvaluations,
        "正在预计算选区拓扑、边指标、三角化和扫描索引。");

    std::set<EdgeId> mergeBlockedEdges = request.lockedEdges;
    std::set<EdgeId> nonSmoothSeamEdges = request.lockedEdges;
    SurfaceReductionPreparedContext prepared;
    const std::set<FaceId> selected(faces.begin(), faces.end());
    {
    ScopedMilliseconds timer(result.report.timing.topologyPrecomputationMs);
    for (EdgeId edgeId = 0; edgeId < document.topology().edgeCount(); ++edgeId) {
        const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
        if (adjacency == nullptr) {
            continue;
        }
        const int selectedAdjacent = static_cast<int>(std::count_if(
            adjacency->faces.begin(), adjacency->faces.end(), [&](FaceId face) { return selected.contains(face); }));
        if (selectedAdjacent == 1 || adjacency->faces.size() != 2) {
            mergeBlockedEdges.insert(edgeId);
            nonSmoothSeamEdges.insert(edgeId);
        } else if (selectedAdjacent == 2) {
            const double originalAngle = originalFaceAngleAtEdge(document, edgeId);
            prepared.internalEdges.push_back({
                edgeId,
                adjacency->faces[0],
                adjacency->faces[1],
                originalAngle,
                originalCurvatureDifferenceAtEdge(document, edgeId),
                edgeLength(document.topology().edge(edgeId))});
            if (originalAngle > request.options.normalToleranceDegrees) {
                mergeBlockedEdges.insert(edgeId);
                nonSmoothSeamEdges.insert(edgeId);
            } else if (originalAngle > request.options.smoothSeamToleranceDegrees) {
                nonSmoothSeamEdges.insert(edgeId);
            }
        }
    }
    }

    result.report.theoreticalMinimumFaceCount = static_cast<int>(
        mergeableComponents(faces, prepared, mergeBlockedEdges).size());
    if (request.options.searchObjective ==
        SurfaceReductionSearchObjective::MinimizeFaceCount) {
        targetCount = result.report.theoreticalMinimumFaceCount;
    }

    SearchState initialState;
    initialState.clusters.reserve(faces.size());
    for (const auto face : faces) {
        GProp_GProps faceProperties;
        BRepGProp::SurfaceProperties(document.topology().face(face), faceProperties);
        prepared.faceCentroids[face] = faceProperties.CentreOfMass();
        prepared.faceAreas[face] = std::max(faceProperties.Mass(), 1.0e-12);
        initialState.clusters.push_back(Cluster {
            {face},
            {},
            BRep_Tool::Surface(document.topology().face(face)),
            {},
            SurfaceReductionMergeMethod::OriginalFace});
    }
    RestrictedBSplinePatchFitOptions fitOptions;
    fitOptions.tolerance3d = request.options.tolerance3d;
    fitOptions.normalToleranceDegrees = request.options.normalToleranceDegrees;
    fitOptions.maximumControlPoints = request.options.maximumControlPoints;
    fitOptions.maximumBoundaryConstraintRefinements =
        request.options.maximumBoundaryConstraintRefinements;
    fitOptions.maximumUvIterations = request.options.maximumUvIterations;
    fitOptions.maximumUvBacktrackingSteps = request.options.maximumUvBacktrackingSteps;
    fitOptions.uvConvergenceTolerance = request.options.uvConvergenceTolerance;
    fitOptions.scanReference = request.scanReferenceStl.get();
    fitOptions.scanReferenceCoverage = request.scanReferenceCoverage;
    fitOptions.scanReferenceWeight = request.options.scanReferenceWeight;
    fitOptions.maximumScanReferenceDistance = request.options.maximumScanReferenceDistance;
    if (execution.fitWorkerCount > 1) {
        ScopedMilliseconds timer(result.report.timing.topologyPrecomputationMs);
        BRepMesh_IncrementalMesh mesher(
            document.shape(),
            std::max(request.options.tolerance3d * 2.0, 0.01),
            Standard_False,
            0.2,
            Standard_False);
        mesher.Perform();
    }
    std::unique_ptr<StlRegionIndex> scanRegionIndex;
    if (request.scanReferenceStl != nullptr &&
        request.scanReferenceCoverage == ScanReferenceCoverage::WholeModel) {
        ScopedMilliseconds timer(result.report.timing.topologyPrecomputationMs);
        scanRegionIndex = std::make_unique<StlRegionIndex>(*request.scanReferenceStl);
        fitOptions.scanRegionIndex = scanRegionIndex.get();
    }

    publishProgress(
        execution,
        SurfaceReductionProgressStage::Searching,
        result.report,
        targetCount,
        static_cast<int>(faces.size()),
        request.options.maximumRestrictedFitEvaluations,
        "拓扑准备完成，开始确定性束搜索和局部拟合。");
    auto search = runDeterministicReductionSearch(
        document,
        request,
        execution,
        prepared,
        faces,
        targetCount,
        initialState,
        mergeBlockedEdges,
        nonSmoothSeamEdges,
        fitOptions,
        result.report);
    FinalCandidateSelection selection;
    if (search.lowerBoundAccepted) {
        selection.accepted = true;
        selection.state = std::move(search.lowerBoundState);
        selection.validation = std::move(search.lowerBoundValidation);
    } else {
        selection = validateReductionSearchCandidates(
            document,
            prepared,
            faces,
            search.finalCandidates,
            nonSmoothSeamEdges,
            request.options,
            execution,
            targetCount,
            result.report,
            search.rejectedGroups);
    }

    if (result.report.persistentFitCacheRequiredMiss) {
        result.report.outcome = SurfaceReductionOutcome::NoAcceptableReduction;
        if (request.options.searchObjective ==
            SurfaceReductionSearchObjective::MinimizeFaceCount) {
            result.report.optimality =
                SurfaceReductionOptimality::NoValidatedResultFound;
        }
        result.report.stopReason = SurfaceReductionStopReason::RequiredCacheMiss;
        result.report.message = "只读缓存回放缺少或损坏了必需的完整局部拟合项。";
        publishProgress(
            execution,
            SurfaceReductionProgressStage::Completed,
            result.report,
            targetCount,
            result.report.outputFaceCount,
            request.options.maximumRestrictedFitEvaluations,
            result.report.message);
        return result;
    }

    for (const auto& [facesKey, rejected] : search.rejectedGroups) {
        result.report.rejectedGroups.push_back(rejected);
    }
    if (!selection.accepted) {
        result.report.outcome = SurfaceReductionOutcome::NoAcceptableReduction;
        if (request.options.searchObjective ==
            SurfaceReductionSearchObjective::MinimizeFaceCount) {
            result.report.optimality =
                SurfaceReductionOptimality::NoValidatedResultFound;
            result.report.message =
                result.report.stopReason ==
                    SurfaceReductionStopReason::FinalValidationBudgetExhausted
                ? "最终验证预算已耗尽，预算内尚未找到通过全部质量门的减面状态；未验证候选不作不可行结论。"
                : (selection.bestFailure.empty()
                    ? "预算内没有找到通过全部质量门的减面状态。"
                    : "预算内已验证候选均未通过全局质量门：" +
                        selection.bestFailure);
        } else {
            result.report.message = selection.bestFailure.empty()
                ? "无法在当前容差和搜索预算内减少选区面数。"
                : "所有局部减面状态均未通过全局质量门：" +
                    selection.bestFailure;
        }
        publishProgress(
            execution,
            SurfaceReductionProgressStage::Completed,
            result.report,
            targetCount,
            result.report.outputFaceCount,
            request.options.maximumRestrictedFitEvaluations,
            result.report.message);
        return result;
    }

    const auto& acceptedState = selection.state;
    auto& acceptedValidation = selection.validation;
    result.report.outputFaceCount = static_cast<int>(acceptedState.clusters.size());
    result.report.bestValidatedFaceCount = result.report.outputFaceCount;
    result.report.reductionRatio = 1.0 -
        static_cast<double>(acceptedState.clusters.size()) / static_cast<double>(faces.size());
    result.report.targetAchieved =
        request.options.searchObjective ==
            SurfaceReductionSearchObjective::TargetRatio &&
        result.report.outputFaceCount <= targetCount;
    result.report.acceptedMergeCount = result.report.inputFaceCount - result.report.outputFaceCount;
    result.report.maxSmoothSeamAngleDegrees = acceptedState.maximumSeamAngle;
    result.report.parameterizationValid = true;
    for (const auto& cluster : acceptedState.clusters) {
        result.report.outputGroups.push_back(cluster.faces);
        result.report.groupReports.push_back({cluster.faces, cluster.method, cluster.fitReport});
        if (cluster.faces.size() < 2) {
            continue;
        }
        result.report.parameterizationValid = result.report.parameterizationValid &&
            cluster.fitReport.parameterizationValid;
        result.report.maxDistance = std::max({
            result.report.maxDistance,
            cluster.fitReport.maxBoundaryDistance,
            cluster.fitReport.maxSelectionDistance,
            cluster.fitReport.maxHoldoutDistance});
        result.report.maxNormalAngleDegrees = std::max({
            result.report.maxNormalAngleDegrees,
            cluster.fitReport.maxSelectionNormalAngleDegrees,
            cluster.fitReport.maxHoldoutNormalAngleDegrees});
        result.report.stepParameterizationUsed = result.report.stepParameterizationUsed ||
            cluster.fitReport.parameterizationSource == SurfaceParameterizationSource::StepHarmonic;
        result.report.scanReferenceSampleCount += cluster.fitReport.scanReferenceSampleCount;
        result.report.maxStepToScanDistance = std::max(
            result.report.maxStepToScanDistance,
            cluster.fitReport.maxStepToScanDistance);
    }
    result.reducedShape = acceptedValidation.reducedShape;
    result.previewShape = acceptedValidation.previewShape;
    result.replacements = std::move(acceptedValidation.replacements);
    result.report.commercialCadReports = std::move(acceptedValidation.commercialCadReports);
    result.report.topologyReport = std::move(acceptedValidation.topologyReport);
    result.report.allQualityGatesPassed = true;
    if (request.options.searchObjective ==
        SurfaceReductionSearchObjective::MinimizeFaceCount) {
        const bool optimal =
            result.report.outputFaceCount ==
            result.report.theoreticalMinimumFaceCount;
        result.report.outcome = SurfaceReductionOutcome::Reduced;
        result.report.optimality = optimal
            ? SurfaceReductionOptimality::OptimalProven
            : SurfaceReductionOptimality::BestFoundWithinBudget;
        if (optimal) {
            result.report.stopReason =
                SurfaceReductionStopReason::LowerBoundAccepted;
            result.report.message =
                "结果达到受保护边决定的理论最少面数，并通过全部质量门。";
        } else {
            result.report.message =
                "已找到通过全部质量门的预算内最少面数结果，但未达到理论下界。";
        }
    } else {
        result.report.outcome = result.report.targetAchieved
            ? SurfaceReductionOutcome::TargetAchieved
            : SurfaceReductionOutcome::Reduced;
        result.report.message = result.report.targetAchieved
            ? "确定性束搜索已找到通过全部质量门且达到目标比例的减面状态。"
            : "确定性束搜索已找到通过全部质量门的部分减面状态。";
    }
    publishProgress(
        execution,
        SurfaceReductionProgressStage::Completed,
        result.report,
        targetCount,
        result.report.outputFaceCount,
        request.options.maximumRestrictedFitEvaluations,
        result.report.message);
    return result;
}

} // namespace spo
