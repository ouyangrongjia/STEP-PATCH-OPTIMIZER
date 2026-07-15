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
    const std::set<std::pair<std::vector<FaceId>, std::vector<FaceId>>>& rejectedPairs) {
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
        auto firstFaces = clusters[static_cast<std::size_t>(first)].faces;
        auto secondFaces = clusters[static_cast<std::size_t>(second)].faces;
        if (rejectedPairs.contains({firstFaces, secondFaces}) ||
            rejectedPairs.contains({secondFaces, firstFaces})) {
            continue;
        }
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
    std::sort(queue.begin(), queue.end(), [](const PairCandidate& lhs, const PairCandidate& rhs) {
        return std::make_tuple(
                   lhs.normalJump, lhs.curvatureDifference, -lhs.sharedLength,
                   lhs.stableFace, lhs.first, lhs.second) <
            std::make_tuple(
                   rhs.normalJump, rhs.curvatureDifference, -rhs.sharedLength,
                   rhs.stableFace, rhs.first, rhs.second);
    });
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

TopoDS_Shape makePreviewShape(const std::vector<Cluster>& clusters) {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const auto& cluster : clusters) {
        if (!cluster.replacementFace.IsNull()) {
            builder.Add(compound, cluster.replacementFace);
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

SurfaceReductionResult SurfaceReductionEngine::reduce(
    const ShapeDocument& document,
    const SurfaceReductionRequest& request) const {
    SurfaceReductionResult result;
    result.sourceShape = document.shape();
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
        request.options.stlReferenceWeight < 0.0 ||
        request.options.maximumStlReferenceDistance <= 0.0) {
        result.report.message = "曲面减面选项无效：容差必须为正，目标比例必须在 0 与 1 之间，控制点上限不得小于 16。";
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

    std::shared_ptr<const StlHybridParameterization> hybridParameterization;
    if (request.referenceStl) {
        hybridParameterization = request.referenceStlIsSelectionCrop
            ? StlHybridParameterization::buildFromPatchMesh(
                *request.referenceStl,
                result.report.hybridParameterization)
            : StlHybridParameterization::buildForSelection(
                document,
                faces,
                *request.referenceStl,
                request.options.tolerance3d,
                result.report.hybridParameterization);
        if (!hybridParameterization) {
            result.report.outcome = SurfaceReductionOutcome::NoAcceptableReduction;
            result.report.message = "STEP 边界与原始 STL 混合参数化失败：" +
                result.report.hybridParameterization.message;
            return result;
        }
        result.report.hybridParameterizationUsed = true;
    }

    std::set<EdgeId> protectedEdges = request.lockedEdges;
    const std::set<FaceId> selected(faces.begin(), faces.end());
    for (EdgeId edgeId = 0; edgeId < document.topology().edgeCount(); ++edgeId) {
        const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
        if (adjacency == nullptr) {
            continue;
        }
        const int selectedAdjacent = static_cast<int>(std::count_if(
            adjacency->faces.begin(), adjacency->faces.end(), [&](FaceId face) { return selected.contains(face); }));
        if (selectedAdjacent == 1 || adjacency->faces.size() != 2 ||
            (selectedAdjacent == 2 &&
             originalFaceAngleAtEdge(document, edgeId) > request.options.smoothSeamToleranceDegrees)) {
            protectedEdges.insert(edgeId);
        }
    }

    std::vector<Cluster> clusters;
    clusters.reserve(faces.size());
    for (const auto face : faces) {
        clusters.push_back(Cluster {{face}, {}, BRep_Tool::Surface(document.topology().face(face)), {}});
    }
    const int targetCount = std::max(1, static_cast<int>(std::floor(
        static_cast<double>(faces.size()) * std::clamp(request.options.targetOutputRatio, 0.0, 1.0))));
    std::set<std::pair<std::vector<FaceId>, std::vector<FaceId>>> rejectedPairs;
    RestrictedBSplinePatchFitOptions fitOptions;
    fitOptions.tolerance3d = request.options.tolerance3d;
    fitOptions.normalToleranceDegrees = request.options.normalToleranceDegrees;
    fitOptions.maximumControlPoints = request.options.maximumControlPoints;
    fitOptions.hybridParameterization = hybridParameterization.get();
    fitOptions.stlReferenceWeight = request.options.stlReferenceWeight;
    fitOptions.maximumStlReferenceDistance = request.options.maximumStlReferenceDistance;

    while (static_cast<int>(clusters.size()) > targetCount) {
        const auto queue = buildPairQueue(document, clusters, protectedEdges, rejectedPairs);
        std::vector<TopoDS_Face> exactFaces;
        exactFaces.reserve(queue.size());
        for (const auto& pair : queue) {
            exactFaces.push_back(tryExactSameDomainGroup(
                document,
                mergedFaces(
                    clusters[static_cast<std::size_t>(pair.first)],
                    clusters[static_cast<std::size_t>(pair.second)])));
        }
        std::vector<std::size_t> pairOrder;
        pairOrder.reserve(queue.size());
        for (std::size_t index = 0; index < queue.size(); ++index) {
            if (!exactFaces[index].IsNull()) {
                pairOrder.push_back(index);
            }
        }
        for (std::size_t index = 0; index < queue.size(); ++index) {
            if (exactFaces[index].IsNull()) {
                pairOrder.push_back(index);
            }
        }
        bool accepted = false;
        for (const auto pairIndex : pairOrder) {
            const auto& pair = queue[pairIndex];
            ++result.report.attemptedMergeCount;
            const auto groupedFaces = mergedFaces(
                clusters[static_cast<std::size_t>(pair.first)],
                clusters[static_cast<std::size_t>(pair.second)]);
            TopoDS_Face fittedFace = exactFaces[pairIndex];
            Handle(Geom_Surface) fittedSurface;
            RestrictedBSplinePatchFitReport fitReport;
            if (!fittedFace.IsNull()) {
                fittedSurface = BRep_Tool::Surface(fittedFace);
                fitReport.success = !fittedSurface.IsNull();
                fitReport.parameterizationValid = fitReport.success;
                fitReport.selectionPassed = fitReport.success;
                fitReport.holdoutPassed = fitReport.success;
                fitReport.brepCheckValid = fitReport.success;
                fitReport.message = "同域面已执行无损精确合并。";
            }
            if (!fitReport.success) {
                const auto fit = RestrictedBSplinePatchFitter().fit(document, groupedFaces, fitOptions);
                fittedFace = fit.face;
                fittedSurface = fit.surface;
                fitReport = fit.report;
                result.report.maxStepToStlDistance = std::max(
                    result.report.maxStepToStlDistance,
                    fitReport.maxStepToStlDistance);
                result.report.stlReferenceSampleCount += fitReport.stlReferenceSampleCount;
            }
            if (!fitReport.success || fittedFace.IsNull() || fittedSurface.IsNull()) {
                result.report.rejectedGroups.push_back({groupedFaces, fitReport.message});
                rejectedPairs.insert({clusters[static_cast<std::size_t>(pair.first)].faces,
                                      clusters[static_cast<std::size_t>(pair.second)].faces});
                continue;
            }

            auto candidateClusters = clusters;
            candidateClusters[static_cast<std::size_t>(pair.first)] =
                Cluster {groupedFaces, fittedFace, fittedSurface, fitReport};
            candidateClusters.erase(candidateClusters.begin() + pair.second);
            const auto seam = validateClusterSeams(
                document,
                candidateClusters,
                pair.first,
                protectedEdges,
                request.options.smoothSeamToleranceDegrees);
            if (!seam.valid) {
                result.report.rejectedGroups.push_back({
                    groupedFaces,
                    "合并后保留的光顺接缝超过配置阈值，已回滚该次合并。"});
                rejectedPairs.insert({clusters[static_cast<std::size_t>(pair.first)].faces,
                                      clusters[static_cast<std::size_t>(pair.second)].faces});
                continue;
            }
            result.report.maxSmoothSeamAngleDegrees = std::max(
                result.report.maxSmoothSeamAngleDegrees,
                seam.maximumAngle);
            clusters.swap(candidateClusters);
            rejectedPairs.clear();
            ++result.report.acceptedMergeCount;
            result.report.maxDistance = std::max({
                result.report.maxDistance,
                fitReport.maxBoundaryDistance,
                fitReport.maxSelectionDistance,
                fitReport.maxHoldoutDistance});
            result.report.maxNormalAngleDegrees = std::max({
                result.report.maxNormalAngleDegrees,
                fitReport.maxSelectionNormalAngleDegrees,
                fitReport.maxHoldoutNormalAngleDegrees});
            accepted = true;
            break;
        }
        if (!accepted) {
            break;
        }
    }

    result.report.outputFaceCount = static_cast<int>(clusters.size());
    result.report.reductionRatio = 1.0 -
        static_cast<double>(clusters.size()) / static_cast<double>(faces.size());
    result.report.targetAchieved = static_cast<int>(clusters.size()) <= targetCount;
    for (const auto& cluster : clusters) {
        result.report.outputGroups.push_back(cluster.faces);
    }
    if (clusters.size() >= faces.size()) {
        result.report.outcome = SurfaceReductionOutcome::NoAcceptableReduction;
        result.report.message = "无法在当前容差内减少选区面数。";
        return result;
    }
    result.report.parameterizationValid = std::all_of(
        clusters.begin(), clusters.end(), [](const Cluster& cluster) {
            return cluster.faces.size() < 2 || cluster.fitReport.parameterizationValid;
        });
    if (!result.report.parameterizationValid) {
        result.report.outcome = SurfaceReductionOutcome::NoAcceptableReduction;
        result.report.message = "输出分组存在无效参数化，整次事务已回滚。";
        return result;
    }

    const auto featureEdges = FeatureEdgeDetector().detect(
        document.topology(),
        request.options.smoothSeamToleranceDegrees,
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
        qualityInput.options.maxBoundaryDistance = request.options.tolerance3d;
        qualityInput.options.maxCornerAnchorDistance = request.options.tolerance3d;
        qualityInput.options.maxFeatureEdgeDistance = request.options.tolerance3d;
        qualityInput.options.maxSeamNormalOffset = request.options.tolerance3d;
        auto qualityReport = CommercialCadQualityGate().evaluate(qualityInput);
        const bool passed = qualityReport.passed;
        const auto message = qualityReport.message;
        result.report.commercialCadReports.push_back(std::move(qualityReport));
        if (!passed) {
            result.report.rejectedGroups.push_back({cluster.faces, message});
            result.report.outcome = SurfaceReductionOutcome::NoAcceptableReduction;
            result.report.message = "商业 CAD 类边界质量门失败，整次事务已回滚：" + message;
            return result;
        }
    }

    for (const auto& cluster : clusters) {
        if (cluster.faces.size() < 2 || cluster.replacementFace.IsNull()) {
            continue;
        }
        result.replacements.push_back({cluster.faces, cluster.replacementFace});
    }
    const auto replacedShape = document.stats().solids == 1 && document.stats().shells == 1
        ? assembleFaceCompound(document, clusters)
        : assembleWithReShape(document, clusters);
    if (replacedShape.IsNull()) {
        result.report.outcome = SurfaceReductionOutcome::NoAcceptableReduction;
        result.report.message = "事务组装未生成有效 B-rep。";
        return result;
    }

    const double topologicalSewingTolerance = std::max(
        1.0e-9,
        std::min(1.0e-6, request.options.tolerance3d * 0.01));
    TopoDS_Shape assembledShape = replacedShape;
    if (document.stats().solids == 0) {
        BRepBuilderAPI_Sewing sewing(topologicalSewingTolerance);
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
    repairOptions.sewingTolerance = topologicalSewingTolerance;
    repairOptions.preferredSewingTolerance = topologicalSewingTolerance;
    repairOptions.minSewingTolerance = topologicalSewingTolerance;
    repairOptions.maxSewingTolerance = topologicalSewingTolerance;
    const auto repaired = repairPatchReplacementShape(assembledShape, repairOptions);
    if (!repaired.success || repaired.shape.IsNull()) {
        result.report.outcome = SurfaceReductionOutcome::NoAcceptableReduction;
        result.report.message = "B-rep 修复未生成可验证结果。";
        return result;
    }

    const ShapeDocument reducedDocument(repaired.shape, document.sourcePath());
    StrictTopologyGateInput gateInput;
    gateInput.beforeDocument = &document;
    gateInput.afterDocument = &reducedDocument;
    gateInput.requireStepRoundtrip = true;
    gateInput.allowMultiFaceReplacement = true;
    gateInput.allowFaceCountIncrease = false;
    result.report.topologyReport = StrictTopologyGate().evaluate(gateInput);
    const int expectedFaceCount = document.stats().faces - static_cast<int>(faces.size()) +
        static_cast<int>(clusters.size());
    if (!result.report.topologyReport.passed || reducedDocument.stats().faces > expectedFaceCount) {
        result.report.outcome = SurfaceReductionOutcome::NoAcceptableReduction;
        result.report.message = "全局 B-rep 或 STEP 回读质量门失败，整次事务已回滚：" +
            result.report.topologyReport.message;
        return result;
    }

    result.reducedShape = repaired.shape;
    result.previewShape = makePreviewShape(clusters);
    result.report.allQualityGatesPassed = true;
    result.report.outcome = result.report.targetAchieved
        ? SurfaceReductionOutcome::TargetAchieved
        : SurfaceReductionOutcome::Reduced;
    result.report.message = result.report.targetAchieved
        ? "选区面数已减少并达到目标输出比例。"
        : "选区面数已减少，但未达到目标输出比例。";
    return result;
}

} // namespace spo
