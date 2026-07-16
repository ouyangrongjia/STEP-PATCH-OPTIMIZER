#include "patch/PatchClosureDiagnostics.h"

#include "brep/ShapeDocument.h"
#include "brep/SurfaceTypeProbe.h"
#include "brep/TopologyGraph.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "validate/ShapeValidator.h"

#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace spo {

struct EdgePointSet {
    bool startValid = false;
    gp_Pnt start;
    bool endValid = false;
    gp_Pnt end;
    bool midpointValid = false;
    gp_Pnt midpoint;
};

struct ClosureFreeEdge {
    int edgeIndex = -1;
    TopoDS_Edge edge;
    int adjacentFaceCount = 0;
    double edgeLength = 0.0;
    double edgeTolerance = 0.0;
    bool degenerated = false;
    EdgePointSet points;
};

struct PatchClosureSnapshotData {
    bool captured = false;
    ShapeStats stats;
    bool brepCheckValid = false;
    int freeEdges = 0;
    int multipleEdges = 0;
    int degeneratedFreeEdges = 0;
    std::vector<ClosureFreeEdge> freeEdgeDetails;
};

namespace {

int count_shapes(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
    int count = 0;
    if (shape.IsNull()) {
        return count;
    }
    for (TopExp_Explorer explorer(shape, type); explorer.More(); explorer.Next()) {
        ++count;
    }
    return count;
}

double distance_or_infinity(const gp_Pnt& lhs, const gp_Pnt& rhs) {
    const auto distance = lhs.Distance(rhs);
    return std::isfinite(distance) ? distance : std::numeric_limits<double>::infinity();
}

double edge_curve_length(const TopoDS_Edge& edge) {
    if (edge.IsNull()) {
        return 0.0;
    }

    double firstParameter = 0.0;
    double lastParameter = 0.0;
    const auto curve = BRep_Tool::Curve(edge, firstParameter, lastParameter);
    if (!curve.IsNull() &&
        std::isfinite(firstParameter) &&
        std::isfinite(lastParameter) &&
        std::abs(lastParameter - firstParameter) > Precision::PConfusion()) {
        try {
            GeomAdaptor_Curve adaptor(curve, firstParameter, lastParameter);
            const auto length = GCPnts_AbscissaPoint::Length(adaptor);
            if (std::isfinite(length)) {
                return std::abs(length);
            }
        } catch (const Standard_Failure&) {
        } catch (...) {
        }
    }

    TopoDS_Vertex firstVertex;
    TopoDS_Vertex lastVertex;
    TopExp::Vertices(edge, firstVertex, lastVertex);
    if (!firstVertex.IsNull() && !lastVertex.IsNull()) {
        const auto length = BRep_Tool::Pnt(firstVertex).Distance(BRep_Tool::Pnt(lastVertex));
        return std::isfinite(length) ? length : 0.0;
    }
    return 0.0;
}

double edge_tolerance(const TopoDS_Edge& edge) {
    if (edge.IsNull()) {
        return 0.0;
    }
    const auto tolerance = BRep_Tool::Tolerance(edge);
    return std::isfinite(tolerance) ? tolerance : 0.0;
}

bool edge_parameter_range(
    const TopoDS_Edge& edge,
    double& firstParameter,
    double& lastParameter) {
    firstParameter = 0.0;
    lastParameter = 0.0;
    if (edge.IsNull()) {
        return false;
    }
    const auto curve = BRep_Tool::Curve(edge, firstParameter, lastParameter);
    return !curve.IsNull() &&
        std::isfinite(firstParameter) &&
        std::isfinite(lastParameter);
}

bool is_degenerated_edge(const TopoDS_Edge& edge, double length, double tolerance) {
    if (edge.IsNull()) {
        return false;
    }
    if (BRep_Tool::Degenerated(edge)) {
        return true;
    }
    return length <= std::max(tolerance, Precision::Confusion());
}

EdgePointSet edge_points(const TopoDS_Edge& edge) {
    EdgePointSet points;
    if (edge.IsNull()) {
        return points;
    }

    double firstParameter = 0.0;
    double lastParameter = 0.0;
    const auto curve = BRep_Tool::Curve(edge, firstParameter, lastParameter);
    if (!curve.IsNull() &&
        std::isfinite(firstParameter) &&
        std::isfinite(lastParameter) &&
        std::abs(lastParameter - firstParameter) > Precision::PConfusion()) {
        points.start = curve->Value(firstParameter);
        points.end = curve->Value(lastParameter);
        points.midpoint = curve->Value((firstParameter + lastParameter) * 0.5);
        points.startValid = true;
        points.endValid = true;
        points.midpointValid = true;
    }

    TopoDS_Vertex firstVertex;
    TopoDS_Vertex lastVertex;
    TopExp::Vertices(edge, firstVertex, lastVertex);
    if (!points.startValid && !firstVertex.IsNull()) {
        points.start = BRep_Tool::Pnt(firstVertex);
        points.startValid = true;
    }
    if (!points.endValid && !lastVertex.IsNull()) {
        points.end = BRep_Tool::Pnt(lastVertex);
        points.endValid = true;
    }
    if (!points.midpointValid && points.startValid && points.endValid) {
        points.midpoint = gp_Pnt(
            (points.start.X() + points.end.X()) * 0.5,
            (points.start.Y() + points.end.Y()) * 0.5,
            (points.start.Z() + points.end.Z()) * 0.5);
        points.midpointValid = true;
    }

    return points;
}

double distance_to_points(const gp_Pnt& point, const EdgePointSet& points) {
    double best = std::numeric_limits<double>::infinity();
    if (points.midpointValid) {
        best = std::min(best, distance_or_infinity(point, points.midpoint));
    }
    if (points.startValid) {
        best = std::min(best, distance_or_infinity(point, points.start));
    }
    if (points.endValid) {
        best = std::min(best, distance_or_infinity(point, points.end));
    }
    return best;
}

double edge_pointset_distance(const EdgePointSet& lhs, const EdgePointSet& rhs) {
    double best = std::numeric_limits<double>::infinity();
    if (lhs.midpointValid && rhs.midpointValid) {
        best = std::min(best, distance_or_infinity(lhs.midpoint, rhs.midpoint));
    }
    if (lhs.startValid && rhs.startValid) {
        best = std::min(best, distance_or_infinity(lhs.start, rhs.start));
    }
    if (lhs.endValid && rhs.endValid) {
        best = std::min(best, distance_or_infinity(lhs.end, rhs.end));
    }
    if (lhs.startValid && rhs.endValid) {
        best = std::min(best, distance_or_infinity(lhs.start, rhs.end));
    }
    if (lhs.endValid && rhs.startValid) {
        best = std::min(best, distance_or_infinity(lhs.end, rhs.start));
    }
    return best;
}

PatchClosureSnapshotData capture_closure_snapshot(const TopoDS_Shape& shape) {
    PatchClosureSnapshotData snapshot;
    if (shape.IsNull()) {
        return snapshot;
    }

    const ShapeDocument document(shape, {});
    const auto validation = ShapeValidator().validate(document);
    snapshot.captured = validation.has_shape;
    snapshot.stats = validation.stats;
    snapshot.brepCheckValid = validation.brep_check_valid;
    snapshot.freeEdges = validation.free_edges;
    snapshot.multipleEdges = validation.multiple_edges;

    const auto& topology = document.topology();
    for (EdgeId edgeId = 0; edgeId < topology.edgeCount(); ++edgeId) {
        const auto* adjacency = topology.adjacencyForEdge(edgeId);
        if (adjacency == nullptr || adjacency->faces.size() != 1) {
            continue;
        }
        ClosureFreeEdge detail;
        detail.edgeIndex = static_cast<int>(edgeId);
        detail.edge = topology.edge(edgeId);
        detail.adjacentFaceCount = static_cast<int>(adjacency->faces.size());
        detail.edgeLength = edge_curve_length(detail.edge);
        detail.edgeTolerance = edge_tolerance(detail.edge);
        detail.degenerated = is_degenerated_edge(detail.edge, detail.edgeLength, detail.edgeTolerance);
        detail.points = edge_points(detail.edge);
        if (detail.degenerated) {
            ++snapshot.degeneratedFreeEdges;
        }
        snapshot.freeEdgeDetails.push_back(detail);
    }

    return snapshot;
}

void copy_pre_repair_closure(PatchReplacementReport& report, const PatchClosureSnapshotData& snapshot) {
    report.preRepairClosureCaptured = snapshot.captured;
    report.preRepairFaceCount = snapshot.stats.faces;
    report.preRepairEdgeCount = snapshot.stats.edges;
    report.preRepairShellCount = snapshot.stats.shells;
    report.preRepairSolidCount = snapshot.stats.solids;
    report.preRepairBRepCheckValid = snapshot.brepCheckValid;
    report.preRepairFreeEdgeCount = snapshot.freeEdges;
    report.preRepairMultipleEdgeCount = snapshot.multipleEdges;
    report.preRepairDegeneratedFreeEdgeCount = snapshot.degeneratedFreeEdges;
}

void copy_post_repair_closure(PatchReplacementReport& report, const PatchClosureSnapshotData& snapshot) {
    report.postRepairClosureCaptured = snapshot.captured;
    report.postRepairFaceCount = snapshot.stats.faces;
    report.postRepairEdgeCount = snapshot.stats.edges;
    report.postRepairShellCount = snapshot.stats.shells;
    report.postRepairSolidCount = snapshot.stats.solids;
    report.postRepairBRepCheckValid = snapshot.brepCheckValid;
    report.postRepairFreeEdgeCount = snapshot.freeEdges;
    report.postRepairMultipleEdgeCount = snapshot.multipleEdges;
    report.postRepairDegeneratedFreeEdgeCount = snapshot.degeneratedFreeEdges;
}

void copy_point_fields(
    PatchReplacementFreeEdgeDiagnostic& diagnostic,
    const EdgePointSet& points) {
    diagnostic.midpointValid = points.midpointValid;
    if (points.midpointValid) {
        diagnostic.midpointX = points.midpoint.X();
        diagnostic.midpointY = points.midpoint.Y();
        diagnostic.midpointZ = points.midpoint.Z();
    }
    diagnostic.startPointValid = points.startValid;
    if (points.startValid) {
        diagnostic.startX = points.start.X();
        diagnostic.startY = points.start.Y();
        diagnostic.startZ = points.start.Z();
    }
    diagnostic.endPointValid = points.endValid;
    if (points.endValid) {
        diagnostic.endX = points.end.X();
        diagnostic.endY = points.end.Y();
        diagnostic.endZ = points.end.Z();
    }
}

void copy_original_boundary_point_fields(
    PatchReplacementFreeEdgeDiagnostic& diagnostic,
    const EdgePointSet& points) {
    diagnostic.nearestOriginalBoundaryMidpointValid = points.midpointValid;
    if (points.midpointValid) {
        diagnostic.nearestOriginalBoundaryMidpointX = points.midpoint.X();
        diagnostic.nearestOriginalBoundaryMidpointY = points.midpoint.Y();
        diagnostic.nearestOriginalBoundaryMidpointZ = points.midpoint.Z();
    }
    diagnostic.nearestOriginalBoundaryStartPointValid = points.startValid;
    if (points.startValid) {
        diagnostic.nearestOriginalBoundaryStartX = points.start.X();
        diagnostic.nearestOriginalBoundaryStartY = points.start.Y();
        diagnostic.nearestOriginalBoundaryStartZ = points.start.Z();
    }
    diagnostic.nearestOriginalBoundaryEndPointValid = points.endValid;
    if (points.endValid) {
        diagnostic.nearestOriginalBoundaryEndX = points.end.X();
        diagnostic.nearestOriginalBoundaryEndY = points.end.Y();
        diagnostic.nearestOriginalBoundaryEndZ = points.end.Z();
    }
}

void fill_original_boundary_edge_context(
    PatchReplacementFreeEdgeDiagnostic& diagnostic,
    const ShapeDocument& beforeDocument) {
    if (diagnostic.nearestOriginalBoundaryEdgeId < 0 || !beforeDocument.hasShape()) {
        return;
    }
    const auto edgeId = static_cast<EdgeId>(diagnostic.nearestOriginalBoundaryEdgeId);
    if (edgeId >= beforeDocument.topology().edgeCount()) {
        return;
    }

    const auto& edge = beforeDocument.topology().edge(edgeId);
    diagnostic.nearestOriginalBoundaryEdgeLength = edge_curve_length(edge);
    diagnostic.nearestOriginalBoundaryEdgeTolerance = edge_tolerance(edge);
    copy_original_boundary_point_fields(diagnostic, edge_points(edge));

    double firstParameter = 0.0;
    double lastParameter = 0.0;
    if (edge_parameter_range(edge, firstParameter, lastParameter)) {
        diagnostic.nearestOriginalBoundaryParameterRangeValid = true;
        diagnostic.nearestOriginalBoundaryFirstParameter = firstParameter;
        diagnostic.nearestOriginalBoundaryLastParameter = lastParameter;
    }

    const auto* adjacency = beforeDocument.topology().adjacencyForEdge(edgeId);
    if (adjacency == nullptr) {
        return;
    }
    diagnostic.nearestOriginalBoundaryAdjacentFaceCount = static_cast<int>(adjacency->faces.size());
    for (const auto faceId : adjacency->faces) {
        if (faceId >= beforeDocument.topology().faceCount()) {
            continue;
        }
        const auto& face = beforeDocument.topology().face(faceId);
        diagnostic.nearestOriginalBoundaryAdjacentFaceIds.push_back(static_cast<int>(faceId));
        diagnostic.nearestOriginalBoundaryAdjacentSurfaceTypes.push_back(surfaceTypeName(face));

        double first = 0.0;
        double last = 0.0;
        const auto pcurve = BRep_Tool::CurveOnSurface(edge, face, first, last);
        if (!pcurve.IsNull()) {
            ++diagnostic.nearestOriginalBoundaryPcurveAvailableFaceCount;
        }
    }
}

void copy_split_segment_fields(
    PatchReplacementFreeEdgeDiagnostic& diagnostic,
    const BoundaryConstrainedPatchSplitBoundarySegment& segment,
    double distance) {
    diagnostic.nearestSplitBoundarySegment = true;
    diagnostic.nearestSplitBoundaryOriginalEdgeId = static_cast<int>(segment.edgeId);
    diagnostic.nearestSplitBoundarySegmentDistance = std::isfinite(distance) ? distance : 0.0;
    diagnostic.nearestSplitBoundaryFirstParameter = segment.firstParameter;
    diagnostic.nearestSplitBoundaryLastParameter = segment.lastParameter;
    diagnostic.nearestSplitBoundaryLength = edge_curve_length(segment.edge);
    diagnostic.nearestSplitBoundaryTolerance = edge_tolerance(segment.edge);
    diagnostic.nearestSplitBoundaryPatchFaceOwner = segment.patchFaceIndex;
}

void match_original_boundary_edge(
    PatchReplacementFreeEdgeDiagnostic& diagnostic,
    const ClosureFreeEdge& freeEdge,
    const ShapeDocument& beforeDocument,
    const RegionBoundaryAnalysis& boundary) {
    if (!freeEdge.points.midpointValid || !beforeDocument.hasShape()) {
        return;
    }

    double bestDistance = std::numeric_limits<double>::infinity();
    int bestEdgeId = -1;
    for (const auto edgeId : boundary.ordered_boundary_edges) {
        if (edgeId >= beforeDocument.topology().edgeCount()) {
            continue;
        }
        const auto points = edge_points(beforeDocument.topology().edge(edgeId));
        const auto distance = distance_to_points(freeEdge.points.midpoint, points);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestEdgeId = static_cast<int>(edgeId);
        }
    }

    diagnostic.nearestOriginalBoundaryEdgeId = bestEdgeId;
    diagnostic.nearestOriginalBoundaryEdgeDistance =
        std::isfinite(bestDistance) ? bestDistance : 0.0;
    fill_original_boundary_edge_context(diagnostic, beforeDocument);
}

void match_split_boundary_segment(
    PatchReplacementFreeEdgeDiagnostic& diagnostic,
    const ClosureFreeEdge& freeEdge,
    const BoundaryConstrainedPatchBuildResult& buildResult) {
    if (!freeEdge.points.midpointValid || diagnostic.nearestOriginalBoundaryEdgeId < 0) {
        return;
    }

    double bestSameEdgeDistance = std::numeric_limits<double>::infinity();
    const BoundaryConstrainedPatchSplitBoundarySegment* bestSameEdgeSegment = nullptr;
    double bestAnyDistance = std::numeric_limits<double>::infinity();
    const BoundaryConstrainedPatchSplitBoundarySegment* bestAnySegment = nullptr;
    int previousOwner = -1;
    for (const auto& segment : buildResult.multiSurfaceSplitBoundarySegments) {
        const auto points = edge_points(segment.edge);
        const auto distance = distance_to_points(freeEdge.points.midpoint, points);
        if (distance < bestAnyDistance) {
            bestAnyDistance = distance;
            bestAnySegment = &segment;
        }

        if (static_cast<int>(segment.edgeId) != diagnostic.nearestOriginalBoundaryEdgeId) {
            continue;
        }
        ++diagnostic.sameOriginalBoundaryEdgeSplitSegmentCount;
        const auto segmentLength = edge_curve_length(segment.edge);
        const auto segmentTolerance = edge_tolerance(segment.edge);
        if (is_degenerated_edge(segment.edge, segmentLength, segmentTolerance)) {
            ++diagnostic.sameOriginalBoundaryEdgeDegeneratedSegmentCount;
        }
        if (previousOwner >= 0 && previousOwner != segment.patchFaceIndex) {
            ++diagnostic.sameOriginalBoundaryEdgeOwnerSwitchCount;
        }
        previousOwner = segment.patchFaceIndex;

        if (distance < bestSameEdgeDistance) {
            bestSameEdgeDistance = distance;
            bestSameEdgeSegment = &segment;
        }
    }

    if (bestSameEdgeSegment != nullptr) {
        diagnostic.matchedSplitBoundarySegment = true;
        diagnostic.matchedSplitBoundaryFirstParameter = bestSameEdgeSegment->firstParameter;
        diagnostic.matchedSplitBoundaryLastParameter = bestSameEdgeSegment->lastParameter;
        diagnostic.patchFaceOwner = bestSameEdgeSegment->patchFaceIndex;
        copy_split_segment_fields(diagnostic, *bestSameEdgeSegment, bestSameEdgeDistance);
        return;
    }

    if (bestAnySegment != nullptr) {
        copy_split_segment_fields(diagnostic, *bestAnySegment, bestAnyDistance);
        diagnostic.patchFaceOwner = bestAnySegment->patchFaceIndex;
    }
}

void fill_fitted_patch_projection_context(
    PatchReplacementFreeEdgeDiagnostic& diagnostic,
    const ClosureFreeEdge& freeEdge,
    const BoundaryConstrainedPatchBuildResult& buildResult) {
    diagnostic.fittedPatchProjectionFaceCount = static_cast<int>(buildResult.replacementFaces.size());
    if (buildResult.replacementFaces.empty()) {
        return;
    }

    std::vector<gp_Pnt> samplePoints;
    if (freeEdge.points.startValid) {
        samplePoints.push_back(freeEdge.points.start);
    }
    if (freeEdge.points.midpointValid) {
        samplePoints.push_back(freeEdge.points.midpoint);
    }
    if (freeEdge.points.endValid) {
        samplePoints.push_back(freeEdge.points.end);
    }
    if (samplePoints.empty()) {
        return;
    }

    double minDistance = std::numeric_limits<double>::infinity();
    double maxDistance = 0.0;
    double sumDistance = 0.0;
    int projected = 0;
    int failed = 0;
    int nearestFace = -1;
    for (std::size_t faceIndex = 0; faceIndex < buildResult.replacementFaces.size(); ++faceIndex) {
        const auto surface = BRep_Tool::Surface(buildResult.replacementFaces[faceIndex]);
        if (surface.IsNull()) {
            failed += static_cast<int>(samplePoints.size());
            continue;
        }
        for (const auto& point : samplePoints) {
            const auto distance = projectionDistanceToSurface(point, surface);
            if (!std::isfinite(distance)) {
                ++failed;
                continue;
            }
            ++projected;
            sumDistance += distance;
            if (distance < minDistance) {
                minDistance = distance;
                nearestFace = static_cast<int>(faceIndex);
            }
            maxDistance = std::max(maxDistance, distance);
        }
    }

    diagnostic.fittedPatchProjectionSampleCount = projected;
    diagnostic.fittedPatchProjectionFailedCount = failed;
    diagnostic.nearestFittedPatchFaceIndex = nearestFace;
    if (projected > 0) {
        diagnostic.fittedPatchProjectionMinDistance = minDistance;
        diagnostic.fittedPatchProjectionMaxDistance = maxDistance;
        diagnostic.fittedPatchProjectionAverageDistance = sumDistance / static_cast<double>(projected);
    }
}

bool appeared_after_repair(
    const ClosureFreeEdge& postRepairFreeEdge,
    const PatchClosureSnapshotData& preRepairSnapshot) {
    constexpr double sameEdgeTolerance = 1.0e-5;
    for (const auto& preRepairFreeEdge : preRepairSnapshot.freeEdgeDetails) {
        if (edge_pointset_distance(postRepairFreeEdge.points, preRepairFreeEdge.points) <= sameEdgeTolerance) {
            return false;
        }
    }
    return true;
}

PatchReplacementFreeEdgeDiagnostic make_free_edge_diagnostic(
    const ClosureFreeEdge& freeEdge,
    bool afterRepair,
    bool appearedAfterRepair,
    const ShapeDocument& beforeDocument,
    const RegionBoundaryAnalysis& boundary,
    const BoundaryConstrainedPatchBuildResult& buildResult) {
    PatchReplacementFreeEdgeDiagnostic diagnostic;
    diagnostic.afterRepair = afterRepair;
    diagnostic.appearedAfterRepair = appearedAfterRepair;
    diagnostic.edgeIndex = freeEdge.edgeIndex;
    diagnostic.adjacentFaceCount = freeEdge.adjacentFaceCount;
    diagnostic.edgeLength = freeEdge.edgeLength;
    diagnostic.edgeTolerance = freeEdge.edgeTolerance;
    diagnostic.degenerated = freeEdge.degenerated;
    copy_point_fields(diagnostic, freeEdge.points);
    match_original_boundary_edge(diagnostic, freeEdge, beforeDocument, boundary);
    match_split_boundary_segment(diagnostic, freeEdge, buildResult);
    fill_fitted_patch_projection_context(diagnostic, freeEdge, buildResult);
    return diagnostic;
}

std::vector<PatchReplacementFreeEdgeDiagnostic> build_free_edge_diagnostics(
    const PatchClosureSnapshotData& preRepairSnapshot,
    const PatchClosureSnapshotData& postRepairSnapshot,
    const ShapeDocument& beforeDocument,
    const RegionBoundaryAnalysis& boundary,
    const BoundaryConstrainedPatchBuildResult& buildResult) {
    std::vector<PatchReplacementFreeEdgeDiagnostic> diagnostics;
    diagnostics.reserve(preRepairSnapshot.freeEdgeDetails.size() + postRepairSnapshot.freeEdgeDetails.size());

    for (const auto& freeEdge : preRepairSnapshot.freeEdgeDetails) {
        diagnostics.push_back(make_free_edge_diagnostic(
            freeEdge,
            false,
            false,
            beforeDocument,
            boundary,
            buildResult));
    }
    for (const auto& freeEdge : postRepairSnapshot.freeEdgeDetails) {
        diagnostics.push_back(make_free_edge_diagnostic(
            freeEdge,
            true,
            appeared_after_repair(freeEdge, preRepairSnapshot),
            beforeDocument,
            boundary,
            buildResult));
    }

    return diagnostics;
}

}

PatchClosureSnapshot PatchClosureDiagnostics::capture(const TopoDS_Shape& shape) const {
    PatchClosureSnapshot snapshot;
    snapshot.data_ = std::make_shared<PatchClosureSnapshotData>(
        capture_closure_snapshot(shape));
    return snapshot;
}

void PatchClosureDiagnostics::copyPreRepairReport(
    PatchReplacementReport& report,
    const PatchClosureSnapshot& snapshot) const {
    if (snapshot.data_ != nullptr) {
        copy_pre_repair_closure(report, *snapshot.data_);
    }
}

void PatchClosureDiagnostics::copyPostRepairReport(
    PatchReplacementReport& report,
    const PatchClosureSnapshot& snapshot) const {
    if (snapshot.data_ != nullptr) {
        copy_post_repair_closure(report, *snapshot.data_);
    }
}

std::vector<PatchReplacementFreeEdgeDiagnostic>
PatchClosureDiagnostics::buildFreeEdgeDiagnostics(
    const PatchClosureSnapshot& preRepairSnapshot,
    const PatchClosureSnapshot& postRepairSnapshot,
    const ShapeDocument& beforeDocument,
    const RegionBoundaryAnalysis& boundary,
    const BoundaryConstrainedPatchBuildResult& buildResult) const {
    if (preRepairSnapshot.data_ == nullptr || postRepairSnapshot.data_ == nullptr) {
        return {};
    }
    return build_free_edge_diagnostics(
        *preRepairSnapshot.data_,
        *postRepairSnapshot.data_,
        beforeDocument,
        boundary,
        buildResult);
}

}
