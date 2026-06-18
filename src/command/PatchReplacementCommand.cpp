#include "command/PatchReplacementCommand.h"

#include "brep/BoundaryWireBuilder.h"
#include "brep/SurfaceTypeProbe.h"
#include "command/CommandContext.h"
#include "patch/BoundaryConstrainedPatchBuilder.h"
#include "patch/BoundaryConstrainedSurfaceRetrim.h"
#include "patch/MultiFacePatchAnalyzer.h"
#include "patch/PatchReplacementRepair.h"
#include "patch/PatchTrimDiagnostics.h"
#include "validate/StrictTopologyGate.h"
#include "validate/ShapeValidator.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRep_Builder.hxx>
#include <BRepLib.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools_ReShape.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_Curve.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_Edge.hxx>
#include <ShapeConstruct_ProjectCurveOnSurface.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace spo {

namespace {

void append_warning(PatchReplacementReport& report, const std::string& warning) {
    if (warning.empty()) {
        return;
    }
    if (!report.warningMessage.empty()) {
        report.warningMessage += " ";
    }
    report.warningMessage += warning;
}

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

PatchReplacementFailureReason map_build_failure(BoundaryConstrainedBuildFailureReason reason) {
    switch (reason) {
    case BoundaryConstrainedBuildFailureReason::InvalidInput:
        return PatchReplacementFailureReason::BuildFailed;
    case BoundaryConstrainedBuildFailureReason::InvalidBoundary:
        return PatchReplacementFailureReason::InvalidBoundary;
    case BoundaryConstrainedBuildFailureReason::NoPatchFaces:
        return PatchReplacementFailureReason::NoPatchFaces;
    case BoundaryConstrainedBuildFailureReason::ReplacementBuildFailed:
    case BoundaryConstrainedBuildFailureReason::BoundaryMismatch:
    case BoundaryConstrainedBuildFailureReason::ShapeFixFailed:
    case BoundaryConstrainedBuildFailureReason::None:
        return PatchReplacementFailureReason::BuildFailed;
    }
    return PatchReplacementFailureReason::BuildFailed;
}

std::string gate_failure_message(const StrictTopologyGateReport& gateReport) {
    std::string message = "StrictTopologyGate failed: ";
    message += toString(gateReport.failureReason);
    if (!gateReport.message.empty()) {
        message += ". ";
        message += gateReport.message;
    }
    return message;
}

struct ReplacementAssemblyResult {
    bool success = false;
    TopoDS_Shape shape;
    std::string message;
    std::string warning;
};

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

struct ClosureSnapshot {
    bool captured = false;
    ShapeStats stats;
    bool brepCheckValid = false;
    int freeEdges = 0;
    int multipleEdges = 0;
    int degeneratedFreeEdges = 0;
    std::vector<ClosureFreeEdge> freeEdgeDetails;
};

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

ClosureSnapshot capture_closure_snapshot(const TopoDS_Shape& shape) {
    ClosureSnapshot snapshot;
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

void copy_pre_repair_closure(PatchReplacementReport& report, const ClosureSnapshot& snapshot) {
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

void copy_post_repair_closure(PatchReplacementReport& report, const ClosureSnapshot& snapshot) {
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
    const ClosureSnapshot& preRepairSnapshot) {
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
    const ClosureSnapshot& preRepairSnapshot,
    const ClosureSnapshot& postRepairSnapshot,
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

std::vector<BoundaryConstrainedPatchSplitBoundarySegment> split_segments_for_edge(
    const BoundaryConstrainedPatchBuildResult& buildResult,
    EdgeId edgeId) {
    std::vector<BoundaryConstrainedPatchSplitBoundarySegment> segments;
    for (const auto& segment : buildResult.multiSurfaceSplitBoundarySegments) {
        if (segment.edgeId == edgeId && !segment.edge.IsNull()) {
            segments.push_back(segment);
        }
    }
    std::sort(segments.begin(), segments.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.firstParameter < rhs.firstParameter;
    });
    return segments;
}

std::optional<EdgeId> matching_split_edge_id(
    const ShapeDocument& document,
    const BoundaryConstrainedPatchBuildResult& buildResult,
    const TopoDS_Edge& edge) {
    if (edge.IsNull()) {
        return std::nullopt;
    }
    for (const auto& segment : buildResult.multiSurfaceSplitBoundarySegments) {
        if (segment.edgeId < document.topology().edgeCount() &&
            edge.IsSame(document.topology().edge(segment.edgeId))) {
            return segment.edgeId;
        }
    }
    return std::nullopt;
}

TopoDS_Edge oriented_segment_edge(
    TopoDS_Edge edge,
    TopAbs_Orientation orientation) {
    edge.Orientation(orientation);
    return edge;
}

bool rebuild_edge_pcurve_on_face(
    const TopoDS_Edge& edge,
    const TopoDS_Face& face,
    double tolerance) {
    if (edge.IsNull() || face.IsNull()) {
        return false;
    }

    double firstParameter = 0.0;
    double lastParameter = 0.0;
    const auto curve = BRep_Tool::Curve(edge, firstParameter, lastParameter);
    TopLoc_Location surfaceLocation;
    const auto surface = BRep_Tool::Surface(face, surfaceLocation);
    if (curve.IsNull() || surface.IsNull()) {
        return false;
    }
    if (std::abs(lastParameter - firstParameter) <= Precision::PConfusion()) {
        return false;
    }

    Handle(Geom_Curve) projectedCurve = curve;
    if (!surfaceLocation.IsIdentity()) {
        projectedCurve = Handle(Geom_Curve)::DownCast(
            curve->Transformed(surfaceLocation.Transformation().Inverted()));
    }
    if (projectedCurve.IsNull()) {
        return false;
    }

    Handle(Geom2d_Curve) pcurve;
    bool projected = false;
    try {
        Handle(ShapeConstruct_ProjectCurveOnSurface) projector =
            new ShapeConstruct_ProjectCurveOnSurface;
        projector->Init(surface, tolerance);
        projected = projector->Perform(
            projectedCurve,
            firstParameter,
            lastParameter,
            pcurve,
            tolerance,
            tolerance);
    } catch (const Standard_Failure&) {
        projected = false;
    }
    if (!projected || pcurve.IsNull()) {
        return false;
    }

    BRep_Builder builder;
    const gp_Pnt2d firstUv = pcurve->Value(firstParameter);
    const gp_Pnt2d lastUv = pcurve->Value(lastParameter);
    builder.UpdateEdge(edge, pcurve, surface, surfaceLocation, tolerance, firstUv, lastUv);
    builder.Range(edge, firstParameter, lastParameter, Standard_False);
    builder.Range(edge, surface, surfaceLocation, firstParameter, lastParameter);
    builder.SameRange(edge, Standard_True);
    builder.UpdateEdge(edge, tolerance);

    Standard_Real maxDeviation = 0.0;
    ShapeAnalysis_Edge edgeAnalyzer;
    edgeAnalyzer.CheckSameParameter(edge, face, maxDeviation, 23);
    if (!std::isfinite(maxDeviation) || maxDeviation > tolerance) {
        return false;
    }
    builder.SameParameter(edge, Standard_True);
    return true;
}

TopoDS_Face rebuild_face_with_split_boundary_edges(
    const ShapeDocument& document,
    const BoundaryConstrainedPatchBuildResult& buildResult,
    const TopoDS_Face& face) {
    const auto surface = BRep_Tool::Surface(face);
    if (surface.IsNull()) {
        return {};
    }

    TopoDS_Wire rebuiltWire;
    int wireCount = 0;
    for (TopExp_Explorer wireExplorer(face, TopAbs_WIRE); wireExplorer.More(); wireExplorer.Next()) {
        ++wireCount;
        if (wireCount > 1) {
            return {};
        }

        BRepBuilderAPI_MakeWire wireBuilder;
        for (TopExp_Explorer edgeExplorer(wireExplorer.Current(), TopAbs_EDGE); edgeExplorer.More(); edgeExplorer.Next()) {
            const auto originalEdge = TopoDS::Edge(edgeExplorer.Current());
            const auto splitEdgeId = matching_split_edge_id(document, buildResult, originalEdge);
            if (!splitEdgeId.has_value()) {
                wireBuilder.Add(originalEdge);
                continue;
            }

            auto segments = split_segments_for_edge(buildResult, *splitEdgeId);
            if (segments.empty()) {
                wireBuilder.Add(originalEdge);
                continue;
            }

            if (originalEdge.Orientation() == TopAbs_REVERSED) {
                for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
                    if (!rebuild_edge_pcurve_on_face(it->edge, face, Precision::Confusion())) {
                        return {};
                    }
                    wireBuilder.Add(oriented_segment_edge(it->edge, TopAbs_REVERSED));
                }
            } else {
                for (const auto& segment : segments) {
                    if (!rebuild_edge_pcurve_on_face(segment.edge, face, Precision::Confusion())) {
                        return {};
                    }
                    wireBuilder.Add(oriented_segment_edge(segment.edge, originalEdge.Orientation()));
                }
            }
        }

        if (!wireBuilder.IsDone()) {
            return {};
        }
        rebuiltWire = wireBuilder.Wire();
    }

    if (wireCount != 1 || rebuiltWire.IsNull()) {
        return {};
    }

    BRepBuilderAPI_MakeFace faceBuilder(surface, rebuiltWire, Standard_True);
    if (!faceBuilder.IsDone()) {
        return {};
    }

    auto rebuiltFace = faceBuilder.Face();
    rebuiltFace.Orientation(face.Orientation());
    BRepLib::BuildCurves3d(rebuiltFace);
    BRepLib::SameParameter(rebuiltFace, Precision::Confusion(), Standard_True);
    return rebuiltFace;
}

TopoDS_Face face_for_compound_assembly(
    const ShapeDocument& document,
    const BoundaryConstrainedPatchBuildResult& buildResult,
    const TopoDS_Face& face) {
    if (buildResult.multiSurfaceSplitBoundarySegments.empty()) {
        return face;
    }

    bool usesSplitBoundaryEdge = false;
    for (TopExp_Explorer edgeExplorer(face, TopAbs_EDGE); edgeExplorer.More(); edgeExplorer.Next()) {
        if (matching_split_edge_id(
                document,
                buildResult,
                TopoDS::Edge(edgeExplorer.Current())).has_value()) {
            usesSplitBoundaryEdge = true;
            break;
        }
    }
    if (!usesSplitBoundaryEdge) {
        return face;
    }

    const auto rebuilt = rebuild_face_with_split_boundary_edges(document, buildResult, face);
    return rebuilt.IsNull() ? face : rebuilt;
}

ReplacementAssemblyResult assemble_face_compound_replacement(
    const ShapeDocument& beforeDocument,
    const BoundaryConstrainedPatchBuildResult& buildResult) {
    ReplacementAssemblyResult result;
    if (!beforeDocument.hasShape()) {
        result.message = "Patch replacement face-compound assembly requires a before document.";
        return result;
    }
    if (buildResult.replacementFaces.empty()) {
        result.message = "Patch replacement face-compound assembly requires replacement faces.";
        return result;
    }

    const std::set<FaceId> sourceFaceIds(
        buildResult.sourceFaceIds.begin(),
        buildResult.sourceFaceIds.end());

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    const auto& topology = beforeDocument.topology();
    for (FaceId faceId = 0; faceId < topology.faceCount(); ++faceId) {
        if (sourceFaceIds.find(faceId) != sourceFaceIds.end()) {
            continue;
        }
        builder.Add(compound, face_for_compound_assembly(
            beforeDocument,
            buildResult,
            topology.face(faceId)));
    }
    for (const auto& face : buildResult.replacementFaces) {
        if (!face.IsNull()) {
            builder.Add(compound, face);
        }
    }

    result.shape = compound;
    result.success = !result.shape.IsNull();
    result.message = result.success
        ? "Assembled replacement as a face compound before repair sewing."
        : "Patch replacement face-compound assembly produced an empty shape.";
    result.warning = "Multi-surface boundary shell used face-compound assembly before repair sewing.";
    return result;
}

TopoDS_Shape boundary_trimmed_patch_face(
    const ShapeDocument& beforeDocument,
    const RegionBoundaryAnalysis& boundary,
    const TopoDS_Face& sourceFace,
    const TopoDS_Face& patchFace,
    std::string& message) {
    const auto surface = BRep_Tool::Surface(patchFace);
    if (surface.IsNull()) {
        message = "Imported one-face patch has no usable surface.";
        return {};
    }

    const auto wire = BoundaryWireBuilder().buildOuterWire(beforeDocument, boundary);
    if (!wire.success) {
        message = wire.message;
        return {};
    }

    BRepBuilderAPI_MakeFace faceBuilder(surface, wire.wire, Standard_True);
    if (!faceBuilder.IsDone()) {
        message = "Could not trim imported patch surface with the original CAD boundary wire.";
        return {};
    }

    auto face = faceBuilder.Face();
    face.Orientation(sourceFace.Orientation());
    message = "Trimmed one-face patch surface with the original CAD boundary wire.";
    return face;
}

ReplacementAssemblyResult assemble_replacement_shape(
    const ShapeDocument& beforeDocument,
    const RegionBoundaryAnalysis& boundary,
    const BoundaryConstrainedPatchBuildResult& buildResult) {
    ReplacementAssemblyResult result;
    if (buildResult.usedMultiSurfaceBoundaryShell) {
        return assemble_face_compound_replacement(beforeDocument, buildResult);
    }

    if (!beforeDocument.hasShape()) {
        result.message = "Patch replacement assembly requires a before document.";
        return result;
    }
    if (buildResult.replacementShape.IsNull()) {
        result.message = "Patch replacement assembly requires a non-empty replacement shape.";
        return result;
    }
    if (buildResult.sourceFaceIds.empty()) {
        result.message = "Patch replacement assembly requires source faces.";
        return result;
    }

    BRepTools_ReShape reshaper;
    const auto& topology = beforeDocument.topology();
    TopoDS_Shape replacementShape = buildResult.replacementShape;
    bool replacedFirstFace = false;
    for (const auto faceId : buildResult.sourceFaceIds) {
        if (faceId < 0 || static_cast<std::size_t>(faceId) >= topology.faceCount()) {
            result.message = "Patch replacement assembly source face id is out of range.";
            return result;
        }
        const auto& sourceFace = topology.face(faceId);
        if (!replacedFirstFace) {
            if (!buildResult.usedOriginalBoundarySurfaceRetrim &&
                buildResult.sourceFaceIds.size() == 1 &&
                buildResult.replacementFaces.size() == 1) {
                std::string trimMessage;
                const auto trimmedFace = boundary_trimmed_patch_face(
                    beforeDocument,
                    boundary,
                    sourceFace,
                    buildResult.replacementFaces.front(),
                    trimMessage);
                if (!trimmedFace.IsNull()) {
                    replacementShape = trimmedFace;
                    result.warning = trimMessage;
                } else if (!trimMessage.empty()) {
                    result.warning = trimMessage;
                }
            }
            reshaper.Replace(sourceFace, replacementShape);
            replacedFirstFace = true;
        } else {
            reshaper.Remove(sourceFace);
        }
    }

    result.shape = reshaper.Apply(beforeDocument.shape());
    if (result.shape.IsNull()) {
        result.message = "Patch replacement assembly produced an empty shape.";
        return result;
    }
    result.success = true;
    result.message = "Assembled replacement shape with BRepTools_ReShape.";
    return result;
}

void copy_repair_report(
    PatchReplacementReport& report,
    const PatchReplacementRepairReport& repairReport) {
    report.repairApplied = true;
    report.sameParameterApplied = repairReport.sameParameterApplied;
    report.shapeFixShapeApplied = repairReport.shapeFixShapeApplied;
    report.shapeFixFaceApplied = repairReport.shapeFixFaceApplied;
    report.shapeFixWireApplied = repairReport.shapeFixWireApplied;
    report.shapeFixApplied = report.shapeFixShapeApplied || report.shapeFixFaceApplied || report.shapeFixWireApplied;
    report.unifySameDomainApplied = repairReport.unifySameDomainApplied;
    report.sewingApplied = repairReport.sewingApplied;
    report.adaptiveSewingApplied = repairReport.adaptiveSewingApplied;
    report.shellToSolidApplied = repairReport.shellToSolidApplied;
    report.repairRunCount += 1;

    report.faceCountBeforeRepair = repairReport.faceCountBeforeRepair;
    report.edgeCountBeforeRepair = repairReport.edgeCountBeforeRepair;
    report.shellCountBeforeRepair = repairReport.shellCountBeforeRepair;
    report.solidCountBeforeRepair = repairReport.solidCountBeforeRepair;
    report.faceCountAfterRepair = repairReport.faceCountAfterRepair;
    report.edgeCountAfterRepair = repairReport.edgeCountAfterRepair;
    report.shellCountAfterRepair = repairReport.shellCountAfterRepair;
    report.solidCountAfterRepair = repairReport.solidCountAfterRepair;

    report.freeEdgesBeforeRepair = repairReport.freeEdgesBeforeRepair;
    report.freeEdgesAfterRepair = repairReport.freeEdgesAfterRepair;
    report.multipleEdgesBeforeRepair = repairReport.multipleEdgesBeforeRepair;
    report.multipleEdgesAfterRepair = repairReport.multipleEdgesAfterRepair;
    report.degeneratedFreeEdgesBeforeRepair = repairReport.degeneratedFreeEdgesBeforeRepair;
    report.degeneratedFreeEdgesAfterRepair = repairReport.degeneratedFreeEdgesAfterRepair;

    report.selectedSewingTolerance = repairReport.selectedSewingTolerance;
    report.sewingAttemptCount = repairReport.sewingAttemptCount;
    report.bestSewingFreeEdges = repairReport.bestSewingFreeEdges;
    report.bestSewingMultipleEdges = repairReport.bestSewingMultipleEdges;
    report.bestSewingDegeneratedFreeEdgeCount = repairReport.bestSewingDegeneratedFreeEdgeCount;
    report.bestSewingFaceCount = repairReport.bestSewingFaceCount;
    report.bestSewingEdgeCount = repairReport.bestSewingEdgeCount;
    report.bestSewingShellCount = repairReport.bestSewingShellCount;
    report.bestSewingSolidCount = repairReport.bestSewingSolidCount;
    report.bestSewingBRepCheckValid = repairReport.bestSewingBRepCheckValid;
    report.bestSewingCollapsed = repairReport.bestSewingCollapsed;

    report.repairWarningMessage = repairReport.warningMessage;
    append_warning(report, repairReport.warningMessage);
}

void copy_gate_report(
    PatchReplacementReport& report,
    const StrictTopologyGateReport& gateReport) {
    report.gateEvaluated = true;
    report.gatePassed = gateReport.passed;
    report.gateFailureReason = toString(gateReport.failureReason);
    report.gateMessage = gateReport.message;
    report.gateWarningMessage = gateReport.warningMessage;

    report.gateBeforeFaceCount = gateReport.beforeStats.faces;
    report.gateBeforeEdgeCount = gateReport.beforeStats.edges;
    report.gateBeforeShellCount = gateReport.beforeStats.shells;
    report.gateBeforeSolidCount = gateReport.beforeStats.solids;
    report.gateAfterFaceCount = gateReport.afterStats.faces;
    report.gateAfterEdgeCount = gateReport.afterStats.edges;
    report.gateAfterShellCount = gateReport.afterStats.shells;
    report.gateAfterSolidCount = gateReport.afterStats.solids;
    report.gateRoundtripFaceCount = gateReport.roundtripStats.faces;
    report.gateRoundtripEdgeCount = gateReport.roundtripStats.edges;
    report.gateRoundtripShellCount = gateReport.roundtripStats.shells;
    report.gateRoundtripSolidCount = gateReport.roundtripStats.solids;

    report.gateBeforeFreeEdges = gateReport.beforeFreeEdges;
    report.gateAfterFreeEdges = gateReport.afterFreeEdges;
    report.gateRoundtripFreeEdges = gateReport.roundtripFreeEdges;
    report.gateBeforeMultipleEdges = gateReport.beforeMultipleEdges;
    report.gateAfterMultipleEdges = gateReport.afterMultipleEdges;
    report.gateRoundtripMultipleEdges = gateReport.roundtripMultipleEdges;

    report.gateBeforeBRepCheckValid = gateReport.beforeBRepCheckValid;
    report.gateAfterBRepCheckValid = gateReport.afterBRepCheckValid;
    report.gateRoundtripBRepCheckValid = gateReport.roundtripBRepCheckValid;
    report.gateStepExportOk = gateReport.stepExportOk;
    report.gateStepRoundtripOk = gateReport.stepRoundtripOk;
    report.gateWatertightSolidRequired = gateReport.watertightSolidRequired;
    report.gateRoundtripWatertightRequired = gateReport.roundtripWatertightRequired;
}

}

PatchReplacementCommand::PatchReplacementCommand(
    PatchReplacementInput input,
    PatchReplacementReport* outReport,
    PatchReplacementCommandOptions options)
    : outReport_(outReport),
      options_(options) {
    hasDocumentInput_ = input.document != nullptr;
    hasCandidateInput_ = input.candidate != nullptr;
    hasBoundaryInput_ = input.boundary != nullptr;
    hasImportedPatchInput_ = input.importedPatch != nullptr;
    hasArtifactPathsInput_ = input.artifactPaths != nullptr;
    hasPreviewReportInput_ = input.previewReport != nullptr;

    if (hasDocumentInput_) {
        documentSnapshot_ = *input.document;
        input_.document = &documentSnapshot_;
    }
    if (hasCandidateInput_) {
        candidateSnapshot_ = *input.candidate;
        input_.candidate = &candidateSnapshot_;
    }
    if (hasBoundaryInput_) {
        boundarySnapshot_ = *input.boundary;
        input_.boundary = &boundarySnapshot_;
    }
    if (hasImportedPatchInput_) {
        importedPatchSnapshot_ = *input.importedPatch;
        input_.importedPatch = &importedPatchSnapshot_;
    }
    if (hasArtifactPathsInput_) {
        artifactPathsSnapshot_ = *input.artifactPaths;
        input_.artifactPaths = &artifactPathsSnapshot_;
    }
    if (hasPreviewReportInput_) {
        previewReportSnapshot_ = *input.previewReport;
        input_.previewReport = &previewReportSnapshot_;
    }
}

const char* PatchReplacementCommand::name() const {
    return "PatchReplacementCommand";
}

Result PatchReplacementCommand::execute(CommandContext& context) {
    executed_ = false;
    committed_ = false;
    beforeDocument_ = {};
    afterDocument_ = {};
    report_ = {};

    if (hasDocumentInput_) {
        rebuildStableInputFromContext(context.document);
    }

    const auto inputReport = validatePatchReplacementInput(input_);
    report_ = inputReport;
    if (!inputReport.success) {
        publishReport();
        return Result::error(report_.message);
    }

    beforeDocument_ = context.document;
    const auto analysis = MultiFacePatchAnalyzer().analyze(*input_.importedPatch);
    report_.patchFaceCount = analysis.faceCount;
    report_.patchEdgeCount = analysis.edgeCount;
    report_.patchShellCount = analysis.shellCount;
    report_.patchSolidCount = analysis.solidCount;
    report_.usedMultiFacePatch = analysis.isMultiFace;
    if (!analysis.success) {
        report_.success = false;
        report_.failureReason = PatchReplacementFailureReason::NoPatchFaces;
        report_.message = analysis.message;
        publishReport();
        return Result::error(report_.message);
    }

    const auto buildResult = BoundaryConstrainedPatchBuilder().build(input_, analysis);
    report_.sourceFaceCount = buildResult.sourceFaceCount;
    report_.patchFaceCount = buildResult.patchFaceCount;
    report_.replacementFaceCount = buildResult.replacementFaceCount;
    report_.replacementEdgeCount = count_shapes(buildResult.replacementShape, TopAbs_EDGE);
    report_.replacementShellCount = count_shapes(buildResult.replacementShape, TopAbs_SHELL);
    report_.replacementSolidCount = count_shapes(buildResult.replacementShape, TopAbs_SOLID);
    report_.usedMultiFacePatch = buildResult.usedMultiFaceFragment || analysis.isMultiFace;
    report_.usedOriginalBoundarySurfaceRetrim = buildResult.usedOriginalBoundarySurfaceRetrim;
    report_.attemptedMultiSurfaceBoundaryShell = buildResult.attemptedMultiSurfaceBoundaryShell;
    report_.usedMultiSurfaceBoundaryShell = buildResult.usedMultiSurfaceBoundaryShell;
    report_.retrimSelectedPatchFaceIndex = buildResult.retrimSelectedPatchFaceIndex;
    report_.retrimBoundarySampleCount = buildResult.retrimBoundarySampleCount;
    report_.retrimProjectedSampleCount = buildResult.retrimProjectedSampleCount;
    report_.retrimFailedProjectionCount = buildResult.retrimFailedProjectionCount;
    report_.retrimMaxProjectionDistance = buildResult.retrimMaxProjectionDistance;
    report_.retrimAverageProjectionDistance = buildResult.retrimAverageProjectionDistance;
    report_.retrimSurfaceCoverageProjectedSampleCount = buildResult.retrimSurfaceCoverageProjectedSampleCount;
    report_.retrimSurfaceCoverageFailedProjectionCount = buildResult.retrimSurfaceCoverageFailedProjectionCount;
    report_.retrimSurfaceCoverageMaxProjectionDistance = buildResult.retrimSurfaceCoverageMaxProjectionDistance;
    report_.retrimSurfaceCoverageAverageProjectionDistance = buildResult.retrimSurfaceCoverageAverageProjectionDistance;
    report_.retrimSurfaceCoverageUncoveredEdgeIds = buildResult.retrimSurfaceCoverageUncoveredEdgeIds;
    report_.multiSurfaceBoundarySampleCount = buildResult.multiSurfaceBoundarySampleCount;
    report_.multiSurfaceProjectedSampleCount = buildResult.multiSurfaceProjectedSampleCount;
    report_.multiSurfaceFailedProjectionCount = buildResult.multiSurfaceFailedProjectionCount;
    report_.multiSurfaceMaxProjectionDistance = buildResult.multiSurfaceMaxProjectionDistance;
    report_.multiSurfaceAverageProjectionDistance = buildResult.multiSurfaceAverageProjectionDistance;
    report_.multiSurfaceAssignedBoundarySegmentCount = buildResult.multiSurfaceAssignedBoundarySegmentCount;
    report_.multiSurfaceSplitBoundaryEdgeCount = buildResult.multiSurfaceSplitBoundaryEdgeCount;
    report_.multiSurfaceBuiltFaceCount = buildResult.multiSurfaceBuiltFaceCount;
    report_.multiSurfaceClosedWireCount = buildResult.multiSurfaceClosedWireCount;
    report_.multiSurfaceOpenWireCount = buildResult.multiSurfaceOpenWireCount;
    report_.multiSurfaceMultipleClosedWireFaceCount = buildResult.multiSurfaceMultipleClosedWireFaceCount;
    report_.multiSurfaceFailedPatchFaceIndex = buildResult.multiSurfaceFailedPatchFaceIndex;
    report_.multiSurfaceFailedFaceEdgeCount = buildResult.multiSurfaceFailedFaceEdgeCount;
    report_.multiSurfaceFailedEdgeIds = buildResult.multiSurfaceFailedEdgeIds;
    report_.multiSurfaceBoundaryEdgePcurveRebuildAttemptCount = buildResult.multiSurfaceBoundaryEdgePcurveRebuildAttemptCount;
    report_.multiSurfaceBoundaryEdgePcurveRebuildSuccessCount = buildResult.multiSurfaceBoundaryEdgePcurveRebuildSuccessCount;
    report_.multiSurfaceBoundaryEdgePcurveRebuildFailureCount = buildResult.multiSurfaceBoundaryEdgePcurveRebuildFailureCount;
    report_.multiSurfaceBoundaryEdgeSameParameterCheckCount = buildResult.multiSurfaceBoundaryEdgeSameParameterCheckCount;
    report_.multiSurfaceBoundaryEdgeSameParameterFailureCount = buildResult.multiSurfaceBoundaryEdgeSameParameterFailureCount;
    report_.multiSurfaceBoundaryEdgeMaxSameParameterDeviation = buildResult.multiSurfaceBoundaryEdgeMaxSameParameterDeviation;
    report_.multiSurfaceBoundaryEdgePcurveRebuildFailedEdgeIds = buildResult.multiSurfaceBoundaryEdgePcurveRebuildFailedEdgeIds;
    report_.multiSurfaceBoundaryEdgeSameParameterFailedEdgeIds = buildResult.multiSurfaceBoundaryEdgeSameParameterFailedEdgeIds;
    append_warning(report_, buildResult.warningMessage);

    if (!buildResult.success) {
        report_.success = false;
        report_.failureReason = map_build_failure(buildResult.failureReason);
        report_.message = buildResult.message;
        publishReport();
        return Result::error(report_.message);
    }

    PatchTrimDiagnosticsInput trimInput;
    trimInput.beforeDocument = &beforeDocument_;
    trimInput.candidate = input_.candidate;
    trimInput.boundary = input_.boundary;
    trimInput.replacementShape = &buildResult.replacementShape;
    PatchTrimDiagnosticsOptions trimOptions;
    trimOptions.boundarySamplesPerEdge = 5;
    trimOptions.surfaceGridDivisions = 3;
    trimOptions.maxBoundarySamples = 160;
    trimOptions.maxSurfaceSamples = 256;
    report_.trimDiagnostics = PatchTrimDiagnostics().analyze(trimInput, trimOptions);

    const auto assemblyResult = assemble_replacement_shape(beforeDocument_, *input_.boundary, buildResult);
    if (!assemblyResult.success) {
        report_.success = false;
        report_.failureReason = PatchReplacementFailureReason::BuildFailed;
        report_.message = assemblyResult.message;
        publishReport();
        return Result::error(report_.message);
    }
    report_.sourceFacesReplaced = true;
    append_warning(report_, assemblyResult.warning);

    const auto preRepairClosure = capture_closure_snapshot(assemblyResult.shape);
    copy_pre_repair_closure(report_, preRepairClosure);

    PatchReplacementRepairOptions repairOptions;
    if (buildResult.usedMultiSurfaceBoundaryShell &&
        buildResult.multiSurfaceMaxProjectionDistance > repairOptions.maxSewingTolerance) {
        const auto measuredBoundaryTolerance = buildResult.multiSurfaceMaxProjectionDistance * 2.0;
        repairOptions.maxSewingTolerance = std::min(0.25, measuredBoundaryTolerance);
        repairOptions.preferredSewingTolerance = repairOptions.maxSewingTolerance;
        append_warning(report_, "Multi-surface boundary shell raised repair sewing tolerance to cover measured original-boundary projection deviation.");
    }
    const auto repairResult = repairPatchReplacementShape(assemblyResult.shape, repairOptions);
    copy_repair_report(report_, repairResult.report);
    const auto postRepairClosure = capture_closure_snapshot(repairResult.shape);
    copy_post_repair_closure(report_, postRepairClosure);
    report_.freeEdgeDiagnostics = build_free_edge_diagnostics(
        preRepairClosure,
        postRepairClosure,
        beforeDocument_,
        *input_.boundary,
        buildResult);
    report_.appearedAfterRepairDegeneratedFreeEdgeCount = static_cast<int>(std::count_if(
        report_.freeEdgeDiagnostics.begin(),
        report_.freeEdgeDiagnostics.end(),
        [](const PatchReplacementFreeEdgeDiagnostic& diagnostic) {
            return diagnostic.afterRepair &&
                diagnostic.appearedAfterRepair &&
                diagnostic.degenerated;
        }));
    if (!repairResult.success) {
        report_.success = false;
        report_.failureReason = PatchReplacementFailureReason::BuildFailed;
        report_.message = repairResult.report.message;
        publishReport();
        return Result::error(report_.message);
    }

    afterDocument_ = ShapeDocument(repairResult.shape, beforeDocument_.sourcePath());
    if (!afterDocument_.hasShape()) {
        report_.success = false;
        report_.failureReason = PatchReplacementFailureReason::BuildFailed;
        report_.message = "Patch replacement command did not construct an after document after repair.";
        publishReport();
        return Result::error(report_.message);
    }

    StrictTopologyGateInput gateInput;
    gateInput.beforeDocument = &beforeDocument_;
    gateInput.afterDocument = &afterDocument_;
    gateInput.replacementReport = &report_;
    gateInput.allowMultiFaceReplacement = true;
    gateInput.allowFaceCountIncrease = true;
    gateInput.requireStepRoundtrip = true;
    gateInput.requireWatertightSolid = options_.requireWatertightSolidGate;
    gateInput.requireZeroFreeEdges = options_.requireZeroFreeEdges;
    gateInput.requireZeroMultipleEdges = options_.requireZeroMultipleEdges;
    gateInput.requireRoundtripWatertight = options_.requireRoundtripWatertight;

    const auto gateReport = StrictTopologyGate().evaluate(gateInput);
    copy_gate_report(report_, gateReport);
    report_.trimDiagnostics.roundtripCompared = gateReport.stepRoundtripOk;
    report_.trimDiagnostics.roundtripChanged =
        gateReport.stepRoundtripOk &&
        (gateReport.roundtripStats.faces != gateReport.afterStats.faces ||
            gateReport.roundtripStats.edges != gateReport.afterStats.edges ||
            gateReport.roundtripStats.shells != gateReport.afterStats.shells ||
            gateReport.roundtripStats.solids != gateReport.afterStats.solids ||
            gateReport.roundtripFreeEdges != gateReport.afterFreeEdges ||
            gateReport.roundtripMultipleEdges != gateReport.afterMultipleEdges ||
            gateReport.roundtripBRepCheckValid != gateReport.afterBRepCheckValid);
    append_warning(report_, gateReport.warningMessage);
    if (!gateReport.passed) {
        report_.success = false;
        report_.rollbackApplied = true;
        report_.failureReason = PatchReplacementFailureReason::GateFailed;
        report_.message = gate_failure_message(gateReport);
        publishReport();
        return Result::error(report_.message);
    }

    context.document = afterDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;

    report_.success = true;
    report_.rollbackApplied = false;
    report_.failureReason = PatchReplacementFailureReason::None;
    report_.message = "Patch replacement command committed after StrictTopologyGate passed.";
    executed_ = true;
    committed_ = true;
    publishReport();
    return Result::ok();
}

bool PatchReplacementCommand::undoable() const {
    return true;
}

Result PatchReplacementCommand::undo(CommandContext& context) {
    if (!committed_ || !beforeDocument_.hasShape()) {
        return Result::error("No committed patch replacement state to undo.");
    }

    context.document = beforeDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

Result PatchReplacementCommand::redo(CommandContext& context) {
    if (!committed_ || !afterDocument_.hasShape()) {
        return Result::error("No committed patch replacement state to redo.");
    }

    context.document = afterDocument_;
    context.featureEdges = {};
    context.validationReport = {};
    context.dirty = true;
    return Result::ok();
}

const PatchReplacementReport& PatchReplacementCommand::report() const {
    return report_;
}

void PatchReplacementCommand::publishReport() {
    if (outReport_ != nullptr) {
        *outReport_ = report_;
    }
}

void PatchReplacementCommand::rebuildStableInputFromContext(const ShapeDocument& document) {
    documentSnapshot_ = document;
    input_.document = &documentSnapshot_;
}

}
