#include "reduction/SurfaceReductionEngine.h"

#include "feature/FeatureEdgeDetector.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/PatchReplacementRepair.h"
#include "reduction/RestrictedBSplinePatchFitter.h"
#include "reduction/StlHybridParameterization.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepGProp.hxx>
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
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <utility>

namespace spo {

namespace {

struct Cluster {
    std::vector<FaceId> faces;
    TopoDS_Face replacementFace;
    Handle(Geom_Surface) supportSurface;
    RestrictedBSplinePatchFitReport fitReport;
    SurfaceReductionMergeMethod method = SurfaceReductionMergeMethod::OriginalFace;
};

struct PairCandidate {
    int first = -1;
    int second = -1;
    double normalJump = 0.0;
    double curvatureDifference = 0.0;
    double sharedLength = 0.0;
    FaceId stableFace = 0;
};

struct SeamQuality {
    bool valid = true;
    double maximumAngle = 0.0;
};

struct SearchState {
    std::vector<Cluster> clusters;
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

std::map<FaceId, int> clusterMembership(const std::vector<Cluster>& clusters) {
    std::map<FaceId, int> membership;
    for (int index = 0; index < static_cast<int>(clusters.size()); ++index) {
        for (const auto face : clusters[static_cast<std::size_t>(index)].faces) {
            membership[face] = index;
        }
    }
    return membership;
}

std::vector<PairCandidate> buildPairQueue(
    const ShapeDocument& document,
    const std::vector<Cluster>& clusters,
    const std::set<EdgeId>& protectedEdges,
    int diversityBins) {
    const auto membership = clusterMembership(clusters);
    std::map<std::pair<int, int>, PairCandidate> pairs;
    for (EdgeId edgeId = 0; edgeId < document.topology().edgeCount(); ++edgeId) {
        if (protectedEdges.contains(edgeId)) {
            continue;
        }
        const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
        if (adjacency == nullptr || adjacency->faces.size() != 2) {
            continue;
        }
        const auto firstMember = membership.find(adjacency->faces[0]);
        const auto secondMember = membership.find(adjacency->faces[1]);
        if (firstMember == membership.end() || secondMember == membership.end() ||
            firstMember->second == secondMember->second) {
            continue;
        }
        const int first = std::min(firstMember->second, secondMember->second);
        const int second = std::max(firstMember->second, secondMember->second);
        auto& pair = pairs[{first, second}];
        pair.first = first;
        pair.second = second;
        pair.normalJump = std::max(pair.normalJump, originalFaceAngleAtEdge(document, edgeId));
        pair.curvatureDifference = std::max(
            pair.curvatureDifference,
            originalCurvatureDifferenceAtEdge(document, edgeId));
        pair.sharedLength += edgeLength(document.topology().edge(edgeId));
        pair.stableFace = std::min(clusters[static_cast<std::size_t>(first)].faces.front(),
                                   clusters[static_cast<std::size_t>(second)].faces.front());
    }
    std::vector<PairCandidate> queue;
    for (const auto& [key, pair] : pairs) {
        queue.push_back(pair);
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
    const std::vector<Cluster>& clusters,
    const std::set<EdgeId>& protectedEdges,
    double toleranceDegrees) {
    SeamQuality quality;
    for (int index = 0; index < static_cast<int>(clusters.size()); ++index) {
        if (clusters[static_cast<std::size_t>(index)].faces.size() < 2) {
            continue;
        }
        const auto clusterQuality = validateClusterSeams(
            document,
            clusters,
            index,
            protectedEdges,
            toleranceDegrees);
        quality.valid = quality.valid && clusterQuality.valid;
        quality.maximumAngle = std::max(
            quality.maximumAngle,
            clusterQuality.maximumAngle);
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
    const SurfaceReductionOptions& options) {
    FinalValidationResult result;
    const auto featureEdges = FeatureEdgeDetector().detect(
        document.topology(),
        options.smoothSeamToleranceDegrees,
        0.0);
    for (const auto& cluster : clusters) {
        if (cluster.faces.size() < 2 || cluster.replacementFace.IsNull()) {
            continue;
        }
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
    auto stepOnlyFallback = [&](const std::vector<FaceId>& groupedFaces) -> const CachedFit* {
        const auto existing = stepOnlyFitCache.find(groupedFaces);
        if (existing != stepOnlyFitCache.end()) {
            ++report.fitCacheHitCount;
            return &existing->second;
        }
        if (report.fitCacheMissCount >=
            request.options.maximumRestrictedFitEvaluations) {
            report.searchBudgetExhausted = true;
            return nullptr;
        }
        auto options = fitOptions;
        options.scanReference = nullptr;
        options.scanReferenceCoverage = ScanReferenceCoverage::None;
        const auto fit = RestrictedBSplinePatchFitter().fit(
            document,
            groupedFaces,
            options);
        ++report.fitCacheMissCount;
        CachedFit cached;
        cached.success = fit.success();
        cached.face = fit.face;
        cached.surface = fit.surface;
        cached.report = fit.report;
        cached.method = SurfaceReductionMergeMethod::RestrictedBSpline;
        return &stepOnlyFitCache.emplace(groupedFaces, std::move(cached)).first->second;
    };
    bool stopSearch = false;
    while (!frontier.empty() &&
           static_cast<int>(frontier.front().clusters.size()) > targetCount &&
           !stopSearch) {
        report.expandedStateCount += static_cast<int>(frontier.size());
        std::vector<SearchState> children;
        for (const auto& state : frontier) {
            const auto queue = buildPairQueue(
                document,
                state.clusters,
                mergeBlockedEdges,
                std::max(1, std::min(8, request.options.maximumPairExpansionsPerState / 2)));
            std::vector<bool> exact(queue.size(), false);
            for (std::size_t pairIndex = 0; pairIndex < queue.size(); ++pairIndex) {
                const auto& pair = queue[pairIndex];
                const auto groupedFaces = mergedFaces(
                    state.clusters[static_cast<std::size_t>(pair.first)],
                    state.clusters[static_cast<std::size_t>(pair.second)]);
                const auto cachedFit = fitCache.find(groupedFaces);
                if (cachedFit != fitCache.end()) {
                    exact[pairIndex] = cachedFit->second.success &&
                        cachedFit->second.method == SurfaceReductionMergeMethod::ExactSameDomain;
                    continue;
                }
                auto exactResult = exactCache.find(groupedFaces);
                if (exactResult == exactCache.end()) {
                    exactResult = exactCache.emplace(
                        groupedFaces,
                        tryExactSameDomainGroup(document, groupedFaces)).first;
                }
                exact[pairIndex] = !exactResult->second.IsNull();
            }

            std::vector<std::size_t> pairOrder;
            pairOrder.reserve(queue.size());
            for (std::size_t index = 0; index < queue.size(); ++index) {
                if (exact[index]) pairOrder.push_back(index);
            }
            for (std::size_t index = 0; index < queue.size(); ++index) {
                if (!exact[index]) pairOrder.push_back(index);
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
            for (const auto pairIndex : pairOrder) {
                if (static_cast<int>(validStateChildren.size()) >= validExpansionQuota &&
                    static_cast<int>(validStateChildren.size() + transitionalStateChildren.size()) >=
                        request.options.maximumPairExpansionsPerState) {
                    break;
                }
                ++report.attemptedMergeCount;
                const auto& pair = queue[pairIndex];
                const auto groupedFaces = mergedFaces(
                    state.clusters[static_cast<std::size_t>(pair.first)],
                    state.clusters[static_cast<std::size_t>(pair.second)]);

                CachedFit cached;
                const auto cachedIterator = fitCache.find(groupedFaces);
                if (cachedIterator != fitCache.end()) {
                    cached = cachedIterator->second;
                    ++report.fitCacheHitCount;
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
                    if (report.fitCacheMissCount >=
                        request.options.maximumRestrictedFitEvaluations) {
                        report.searchBudgetExhausted = true;
                        stopSearch = true;
                        break;
                    }
                    const auto fit = RestrictedBSplinePatchFitter().fit(
                        document,
                        groupedFaces,
                        fitOptions);
                    ++report.fitCacheMissCount;
                    cached.success = fit.success();
                    cached.face = fit.face;
                    cached.surface = fit.surface;
                    cached.report = fit.report;
                    cached.method = SurfaceReductionMergeMethod::RestrictedBSpline;
                    fitCache.emplace(groupedFaces, cached);
                }

                if ((!cached.success || cached.face.IsNull() || cached.surface.IsNull()) &&
                    hasScanReference &&
                    cached.method == SurfaceReductionMergeMethod::RestrictedBSpline) {
                    const auto* fallback = stepOnlyFallback(groupedFaces);
                    if (fallback != nullptr && fallback->success &&
                        !fallback->face.IsNull() && !fallback->surface.IsNull()) {
                        cached = *fallback;
                    }
                }
                if (!cached.success || cached.face.IsNull() || cached.surface.IsNull()) {
                    search.rejectedGroups.try_emplace(
                        groupedFaces,
                        SurfaceReductionRejectedGroup {
                            groupedFaces,
                            mapFailureStage(cached.report.failureStage),
                            cached.report.message,
                            cached.report});
                    continue;
                }

                auto candidateClusters = state.clusters;
                candidateClusters[static_cast<std::size_t>(pair.first)] = Cluster {
                    groupedFaces,
                    cached.face,
                    cached.surface,
                    cached.report,
                    cached.method};
                candidateClusters.erase(candidateClusters.begin() + pair.second);
                auto seam = validateAllClusterSeams(
                    document,
                    candidateClusters,
                    nonSmoothSeamEdges,
                    request.options.smoothSeamToleranceDegrees);
                if (!seam.valid && hasScanReference &&
                    cached.method == SurfaceReductionMergeMethod::RestrictedBSpline &&
                    cached.report.scanReferenceSampleCount > 0) {
                    const auto* fallback = stepOnlyFallback(groupedFaces);
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
                        const auto fallbackSeam = validateAllClusterSeams(
                            document,
                            fallbackClusters,
                            nonSmoothSeamEdges,
                            request.options.smoothSeamToleranceDegrees);
                        if ((fallbackSeam.valid && !seam.valid) ||
                            fallbackSeam.maximumAngle < seam.maximumAngle) {
                            cached = *fallback;
                            candidateClusters = std::move(fallbackClusters);
                            seam = fallbackSeam;
                        }
                    }
                }
                SearchState child;
                child.clusters = std::move(candidateClusters);
                child.maximumSeamAngle = seam.maximumAngle;
                if (seam.valid) {
                    validStateChildren.push_back(std::move(child));
                } else {
                    transitionalStateChildren.push_back(std::move(child));
                }
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
            const int initialValidCount = std::min(
                validExpansionQuota,
                static_cast<int>(validStateChildren.size()));
            const int initialTransitionalCount = std::min(
                transitionalExpansionQuota,
                static_cast<int>(transitionalStateChildren.size()));
            const auto childStart = children.size();
            for (int index = 0; index < initialValidCount; ++index) {
                children.push_back(std::move(validStateChildren[static_cast<std::size_t>(index)]));
            }
            for (int index = 0; index < initialTransitionalCount; ++index) {
                children.push_back(std::move(transitionalStateChildren[static_cast<std::size_t>(index)]));
            }
            int validIndex = initialValidCount;
            int transitionalIndex = initialTransitionalCount;
            while (static_cast<int>(children.size() - childStart) < maximumExpansions) {
                if (validIndex < static_cast<int>(validStateChildren.size())) {
                    children.push_back(std::move(
                        validStateChildren[static_cast<std::size_t>(validIndex++)]));
                } else if (transitionalIndex < static_cast<int>(transitionalStateChildren.size())) {
                    children.push_back(std::move(
                        transitionalStateChildren[static_cast<std::size_t>(transitionalIndex++)]));
                } else {
                    break;
                }
            }
            report.generatedStateCount +=
                static_cast<int>(children.size() - childStart);
        }
        if (children.empty()) {
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
            frontier.resize(static_cast<std::size_t>(request.options.beamWidth));
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
    return search;
}

FinalCandidateSelection validateReductionSearchCandidates(
    const ShapeDocument& document,
    const std::vector<FaceId>& faces,
    const std::vector<SearchState>& candidates,
    const std::set<EdgeId>& nonSmoothSeamEdges,
    const SurfaceReductionOptions& options,
    SurfaceReductionReport& report,
    std::map<std::vector<FaceId>, SurfaceReductionRejectedGroup>& rejectedGroups) {
    FinalCandidateSelection selection;
    for (const auto& candidateState : candidates) {
        const auto seam = validateAllClusterSeams(
            document,
            candidateState.clusters,
            nonSmoothSeamEdges,
            options.smoothSeamToleranceDegrees);
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
        auto validation = validateFinalState(
            document,
            faces,
            candidateState.clusters,
            options);
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
    const SurfaceReductionRequest& request) const {
    SurfaceReductionResult result;
    result.sourceShape = document.shape();
    result.report.scanReferenceCoverage = request.scanReferenceCoverage;
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
        !(request.options.targetOutputRatio > 0.0 && request.options.targetOutputRatio < 1.0) ||
        request.options.maximumControlPoints < 16 ||
        request.options.maximumBoundaryConstraintRefinements < 0 ||
        request.options.maximumUvIterations < 1 ||
        request.options.maximumUvBacktrackingSteps < 1 ||
        request.options.uvConvergenceTolerance <= 0.0 ||
        request.options.beamWidth < 1 ||
        request.options.maximumPairExpansionsPerState < 1 ||
        request.options.maximumRestrictedFitEvaluations < 1 ||
        request.options.scanReferenceWeight < 0.0 ||
        request.options.maximumScanReferenceDistance <= 0.0) {
        result.report.message = "曲面减面选项无效：容差必须为正，目标比例必须在 0 与 1 之间，控制点上限不得小于 16。";
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

    std::set<EdgeId> mergeBlockedEdges = request.lockedEdges;
    std::set<EdgeId> nonSmoothSeamEdges = request.lockedEdges;
    const std::set<FaceId> selected(faces.begin(), faces.end());
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
            if (originalAngle > request.options.normalToleranceDegrees) {
                mergeBlockedEdges.insert(edgeId);
                nonSmoothSeamEdges.insert(edgeId);
            } else if (originalAngle > request.options.smoothSeamToleranceDegrees) {
                nonSmoothSeamEdges.insert(edgeId);
            }
        }
    }

    SearchState initialState;
    initialState.clusters.reserve(faces.size());
    for (const auto face : faces) {
        initialState.clusters.push_back(Cluster {
            {face},
            {},
            BRep_Tool::Surface(document.topology().face(face)),
            {},
            SurfaceReductionMergeMethod::OriginalFace});
    }
    const int targetCount = std::max(1, static_cast<int>(std::floor(
        static_cast<double>(faces.size()) * std::clamp(request.options.targetOutputRatio, 0.0, 1.0))));
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

    auto search = runDeterministicReductionSearch(
        document,
        request,
        faces,
        targetCount,
        initialState,
        mergeBlockedEdges,
        nonSmoothSeamEdges,
        fitOptions,
        result.report);
    auto selection = validateReductionSearchCandidates(
        document,
        faces,
        search.finalCandidates,
        nonSmoothSeamEdges,
        request.options,
        result.report,
        search.rejectedGroups);

    for (const auto& [facesKey, rejected] : search.rejectedGroups) {
        result.report.rejectedGroups.push_back(rejected);
    }
    if (!selection.accepted) {
        result.report.outcome = SurfaceReductionOutcome::NoAcceptableReduction;
        result.report.message = selection.bestFailure.empty()
            ? "无法在当前容差和搜索预算内减少选区面数。"
            : "所有局部减面状态均未通过全局质量门：" + selection.bestFailure;
        return result;
    }

    const auto& acceptedState = selection.state;
    auto& acceptedValidation = selection.validation;
    result.report.outputFaceCount = static_cast<int>(acceptedState.clusters.size());
    result.report.reductionRatio = 1.0 -
        static_cast<double>(acceptedState.clusters.size()) / static_cast<double>(faces.size());
    result.report.targetAchieved = result.report.outputFaceCount <= targetCount;
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
    result.report.outcome = result.report.targetAchieved
        ? SurfaceReductionOutcome::TargetAchieved
        : SurfaceReductionOutcome::Reduced;
    result.report.message = result.report.targetAchieved
        ? "确定性束搜索已找到通过全部质量门且达到目标比例的减面状态。"
        : "确定性束搜索已找到通过全部质量门的部分减面状态。";
    return result;
}

} // namespace spo
