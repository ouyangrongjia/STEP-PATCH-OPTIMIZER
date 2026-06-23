#include "patch/BoundaryConstrainedSurfaceRetrim.h"

#include "brep/BoundaryWireBuilder.h"
#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_Curve.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_Edge.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeConstruct_ProjectCurveOnSurface.hxx>
#include <ShapeFix_Face.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace spo {

namespace {

BoundaryConstrainedSurfaceRetrimResult fail(std::string message) {
    BoundaryConstrainedSurfaceRetrimResult result;
    result.message = std::move(message);
    return result;
}

double edge_length(const TopoDS_Edge& edge) {
    GProp_GProps properties;
    BRepGProp::LinearProperties(edge, properties);
    return std::max(0.0, properties.Mass());
}

BoundaryConstrainedSurfaceCandidateReport evaluate_surface(
    const TopoDS_Face& face,
    int patchFaceIndex,
    const std::vector<BoundarySurfaceSample>& samples,
    double projectionTolerance) {
    BoundaryConstrainedSurfaceCandidateReport report;
    report.patchFaceIndex = patchFaceIndex;
    report.boundarySampleCount = static_cast<int>(samples.size());

    const auto surface = BRep_Tool::Surface(face);
    if (surface.IsNull() || samples.empty()) {
        report.failedProjectionCount = report.boundarySampleCount;
        return report;
    }

    double totalDistance = 0.0;
    for (const auto& sample : samples) {
        const auto bestDistance = projectionDistanceToSurface(sample.point, surface);
        report.maxProjectionDistance = std::max(report.maxProjectionDistance, bestDistance);
        totalDistance += bestDistance;
        if (bestDistance <= projectionTolerance) {
            ++report.projectedSampleCount;
        } else {
            ++report.failedProjectionCount;
        }
    }

    report.averageProjectionDistance = samples.empty()
        ? 0.0
        : totalDistance / static_cast<double>(samples.size());
    report.accepted = report.boundarySampleCount > 0 && report.failedProjectionCount == 0;
    return report;
}

bool better_candidate(
    const BoundaryConstrainedSurfaceCandidateReport& lhs,
    const BoundaryConstrainedSurfaceCandidateReport& rhs) {
    if (lhs.failedProjectionCount != rhs.failedProjectionCount) {
        return lhs.failedProjectionCount < rhs.failedProjectionCount;
    }
    if (lhs.maxProjectionDistance != rhs.maxProjectionDistance) {
        return lhs.maxProjectionDistance < rhs.maxProjectionDistance;
    }
    return lhs.averageProjectionDistance < rhs.averageProjectionDistance;
}

void append_unique_edge(std::vector<EdgeId>& edgeIds, EdgeId edgeId) {
    if (std::find(edgeIds.begin(), edgeIds.end(), edgeId) == edgeIds.end()) {
        edgeIds.push_back(edgeId);
    }
}

TopoDS_Edge make_projected_boundary_edge(
    const ShapeDocument& document,
    EdgeId edgeId,
    const TopoDS_Face& targetFace,
    double projectionTolerance,
    BoundaryConstrainedSurfaceRetrimResult& result) {
    if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= document.topology().edgeCount()) {
        return {};
    }
    if (targetFace.IsNull()) {
        append_unique_edge(result.boundaryEdgePcurveRebuildFailedEdgeIds, edgeId);
        ++result.boundaryEdgePcurveRebuildFailureCount;
        return {};
    }

    const auto& sourceEdge = document.topology().edge(edgeId);
    double firstParameter = 0.0;
    double lastParameter = 0.0;
    const auto curve = BRep_Tool::Curve(sourceEdge, firstParameter, lastParameter);
    TopLoc_Location surfaceLocation;
    const auto surface = BRep_Tool::Surface(targetFace, surfaceLocation);
    if (curve.IsNull() || surface.IsNull()) {
        append_unique_edge(result.boundaryEdgePcurveRebuildFailedEdgeIds, edgeId);
        ++result.boundaryEdgePcurveRebuildFailureCount;
        return {};
    }
    if (std::abs(lastParameter - firstParameter) <= Precision::PConfusion()) {
        append_unique_edge(result.boundaryEdgePcurveRebuildFailedEdgeIds, edgeId);
        ++result.boundaryEdgePcurveRebuildFailureCount;
        return {};
    }

    ++result.boundaryEdgePcurveRebuildAttemptCount;
    BRepBuilderAPI_MakeEdge edgeBuilder(curve, firstParameter, lastParameter);
    if (!edgeBuilder.IsDone()) {
        append_unique_edge(result.boundaryEdgePcurveRebuildFailedEdgeIds, edgeId);
        ++result.boundaryEdgePcurveRebuildFailureCount;
        return {};
    }
    auto edge = edgeBuilder.Edge();
    edge.Orientation(sourceEdge.Orientation());

    Handle(Geom_Curve) projectedCurve = curve;
    if (!surfaceLocation.IsIdentity()) {
        projectedCurve = Handle(Geom_Curve)::DownCast(
            curve->Transformed(surfaceLocation.Transformation().Inverted()));
    }
    if (projectedCurve.IsNull()) {
        append_unique_edge(result.boundaryEdgePcurveRebuildFailedEdgeIds, edgeId);
        ++result.boundaryEdgePcurveRebuildFailureCount;
        return {};
    }

    Handle(Geom2d_Curve) pcurve;
    bool projected = false;
    try {
        Handle(ShapeConstruct_ProjectCurveOnSurface) projector =
            new ShapeConstruct_ProjectCurveOnSurface;
        projector->Init(surface, projectionTolerance);
        projected = projector->Perform(
            projectedCurve,
            firstParameter,
            lastParameter,
            pcurve,
            projectionTolerance,
            projectionTolerance);
    } catch (const Standard_Failure&) {
        projected = false;
    }

    if (!projected || pcurve.IsNull()) {
        append_unique_edge(result.boundaryEdgePcurveRebuildFailedEdgeIds, edgeId);
        ++result.boundaryEdgePcurveRebuildFailureCount;
        return {};
    }

    BRep_Builder builder;
    const gp_Pnt2d firstUv = pcurve->Value(firstParameter);
    const gp_Pnt2d lastUv = pcurve->Value(lastParameter);
    builder.UpdateEdge(edge, pcurve, surface, surfaceLocation, projectionTolerance, firstUv, lastUv);
    builder.Range(edge, firstParameter, lastParameter, Standard_False);
    builder.Range(edge, surface, surfaceLocation, firstParameter, lastParameter);
    builder.SameRange(edge, Standard_True);
    builder.UpdateEdge(edge, projectionTolerance);

    ++result.boundaryEdgePcurveRebuildSuccessCount;
    ++result.boundaryEdgeSameParameterCheckCount;
    Standard_Real maxDeviation = 0.0;
    ShapeAnalysis_Edge edgeAnalyzer;
    edgeAnalyzer.CheckSameParameter(edge, targetFace, maxDeviation, 23);
    if (std::isfinite(maxDeviation)) {
        result.boundaryEdgeMaxSameParameterDeviation =
            std::max(result.boundaryEdgeMaxSameParameterDeviation, static_cast<double>(maxDeviation));
    } else {
        maxDeviation = std::numeric_limits<Standard_Real>::infinity();
        result.boundaryEdgeMaxSameParameterDeviation = std::numeric_limits<double>::infinity();
    }
    if (!std::isfinite(maxDeviation) || maxDeviation > projectionTolerance) {
        append_unique_edge(result.boundaryEdgeSameParameterFailedEdgeIds, edgeId);
        ++result.boundaryEdgeSameParameterFailureCount;
        return {};
    }
    builder.SameParameter(edge, Standard_True);
    return edge;
}

TopoDS_Wire connect_projected_boundary_edges(
    const std::vector<TopoDS_Edge>& edges,
    double tolerance,
    std::string& message) {
    if (edges.empty()) {
        message = "No projected original CAD boundary edges were available for strict re-trim.";
        return {};
    }

    Handle(TopTools_HSequenceOfShape) edgeSequence = new TopTools_HSequenceOfShape;
    for (const auto& edge : edges) {
        if (!edge.IsNull()) {
            edgeSequence->Append(edge);
        }
    }
    if (edgeSequence->Length() == 0) {
        message = "Only null projected original CAD boundary edges were available for strict re-trim.";
        return {};
    }

    Handle(TopTools_HSequenceOfShape) wires = new TopTools_HSequenceOfShape;
    ShapeAnalysis_FreeBounds::ConnectEdgesToWires(
        edgeSequence,
        tolerance,
        Standard_False,
        wires);

    Handle(TopTools_HSequenceOfShape) closedWires = new TopTools_HSequenceOfShape;
    Handle(TopTools_HSequenceOfShape) openWires = new TopTools_HSequenceOfShape;
    ShapeAnalysis_FreeBounds::SplitWires(
        wires,
        tolerance,
        Standard_False,
        closedWires,
        openWires);

    if (openWires->Length() != 0 || closedWires->Length() != 1) {
        message = "Strict original-boundary re-trim projected edges did not form exactly one closed wire.";
        return {};
    }

    auto wire = TopoDS::Wire(closedWires->Value(1));
    if (wire.IsNull() || !wire.Closed()) {
        message = "Strict original-boundary re-trim projected wire is not closed.";
        return {};
    }
    return wire;
}

TopoDS_Wire build_strict_projected_boundary_wire(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& boundary,
    const TopoDS_Face& targetFace,
    double projectionTolerance,
    BoundaryConstrainedSurfaceRetrimResult& result,
    std::string& message) {
    std::vector<TopoDS_Edge> projectedEdges;
    projectedEdges.reserve(boundary.ordered_boundary_edges.size());
    for (const auto edgeId : boundary.ordered_boundary_edges) {
        const auto edge = make_projected_boundary_edge(
            document,
            edgeId,
            targetFace,
            projectionTolerance,
            result);
        if (edge.IsNull()) {
            message = "Strict original-boundary re-trim could not rebuild a boundary edge pcurve on the selected Geomagic surface.";
            return {};
        }
        projectedEdges.push_back(edge);
    }

    return connect_projected_boundary_edges(projectedEdges, projectionTolerance, message);
}

}

std::vector<BoundarySurfaceSample> sampleBoundaryForSurfaceProjection(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& boundary,
    int samplesPerEdge) {
    std::vector<BoundarySurfaceSample> samples;
    const auto& topology = document.topology();
    const auto minCount = std::max(2, samplesPerEdge);
    for (const auto edgeId : boundary.ordered_boundary_edges) {
        if (edgeId >= topology.edgeCount()) {
            continue;
        }
        const auto& edge = topology.edge(edgeId);
        double firstParameter = 0.0;
        double lastParameter = 0.0;
        const auto curve = BRep_Tool::Curve(edge, firstParameter, lastParameter);
        if (curve.IsNull()) {
            continue;
        }

        const auto length = edge_length(edge);
        const auto count = std::clamp(
            std::max(minCount, static_cast<int>(std::ceil(length / 0.5)) + 1),
            2,
            96);
        for (int index = 0; index < count; ++index) {
            const double ratio = count == 1
                ? 0.0
                : static_cast<double>(index) / static_cast<double>(count - 1);
            const auto parameter = firstParameter + (lastParameter - firstParameter) * ratio;
            samples.push_back({edgeId, parameter, curve->Value(parameter)});
        }
    }
    return samples;
}

double projectionDistanceToSurface(
    const gp_Pnt& point,
    const Handle(Geom_Surface)& surface) {
    if (surface.IsNull()) {
        return std::numeric_limits<double>::infinity();
    }

    GeomAPI_ProjectPointOnSurf projector(point, surface);
    if (!projector.IsDone() || projector.NbPoints() == 0) {
        return std::numeric_limits<double>::infinity();
    }

    auto bestDistance = std::numeric_limits<double>::infinity();
    for (int index = 1; index <= projector.NbPoints(); ++index) {
        bestDistance = std::min(bestDistance, projector.Distance(index));
    }
    return bestDistance;
}

BoundarySurfaceCoverageReport evaluateBoundarySurfaceCoverage(
    const std::vector<TopoDS_Face>& faces,
    const std::vector<BoundarySurfaceSample>& samples,
    double projectionTolerance) {
    BoundarySurfaceCoverageReport report;
    report.boundarySampleCount = static_cast<int>(samples.size());
    if (samples.empty()) {
        return report;
    }

    std::vector<Handle(Geom_Surface)> surfaces;
    surfaces.reserve(faces.size());
    for (const auto& face : faces) {
        if (face.IsNull()) {
            continue;
        }
        const auto surface = BRep_Tool::Surface(face);
        if (!surface.IsNull()) {
            surfaces.push_back(surface);
        }
    }

    double totalDistance = 0.0;
    for (const auto& sample : samples) {
        auto bestDistance = std::numeric_limits<double>::infinity();
        for (const auto& surface : surfaces) {
            bestDistance = std::min(
                bestDistance,
                projectionDistanceToSurface(sample.point, surface));
        }

        report.maxProjectionDistance = std::max(report.maxProjectionDistance, bestDistance);
        totalDistance += bestDistance;
        if (bestDistance <= projectionTolerance) {
            ++report.projectedSampleCount;
        } else {
            ++report.failedProjectionCount;
            if (std::find(report.uncoveredEdgeIds.begin(), report.uncoveredEdgeIds.end(), sample.edgeId) ==
                report.uncoveredEdgeIds.end()) {
                report.uncoveredEdgeIds.push_back(sample.edgeId);
            }
        }
    }

    report.averageProjectionDistance = totalDistance / static_cast<double>(samples.size());
    return report;
}

BoundaryConstrainedSurfaceRetrimResult BoundaryConstrainedSurfaceRetrim::retrim(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& boundary,
    const std::vector<TopoDS_Face>& patchFaces,
    const TopoDS_Face& sourceOrientationFace,
    const BoundaryConstrainedSurfaceRetrimOptions& options) const {
    if (options.samplesPerEdge < 2) {
        return fail("Boundary-constrained surface re-trim requires at least two samples per edge.");
    }
    if (options.projectionTolerance < 0.0) {
        return fail("Boundary-constrained surface re-trim projection tolerance must not be negative.");
    }
    if (!document.hasShape()) {
        return fail("Boundary-constrained surface re-trim requires a loaded document.");
    }
    if (patchFaces.empty()) {
        return fail("Boundary-constrained surface re-trim requires at least one patch face.");
    }

    const auto wire = BoundaryWireBuilder().buildOuterWire(document, boundary);
    if (!wire.success) {
        return fail(wire.message);
    }

    const auto samples = sampleBoundaryForSurfaceProjection(document, boundary, options.samplesPerEdge);
    if (samples.empty()) {
        return fail("Boundary-constrained surface re-trim could not sample the original CAD boundary.");
    }

    BoundaryConstrainedSurfaceRetrimResult result;
    result.candidates.reserve(patchFaces.size());
    const auto coverage = evaluateBoundarySurfaceCoverage(
        patchFaces,
        samples,
        options.projectionTolerance);
    result.surfaceCoverageProjectedSampleCount = coverage.projectedSampleCount;
    result.surfaceCoverageFailedProjectionCount = coverage.failedProjectionCount;
    result.surfaceCoverageMaxProjectionDistance = coverage.maxProjectionDistance;
    result.surfaceCoverageAverageProjectionDistance = coverage.averageProjectionDistance;
    result.surfaceCoverageUncoveredEdgeIds = coverage.uncoveredEdgeIds;

    for (std::size_t index = 0; index < patchFaces.size(); ++index) {
        if (patchFaces[index].IsNull()) {
            continue;
        }
        result.candidates.push_back(evaluate_surface(
            patchFaces[index],
            static_cast<int>(index),
            samples,
            options.projectionTolerance));
    }
    if (result.candidates.empty()) {
        return fail("Boundary-constrained surface re-trim found no usable patch faces.");
    }

    const auto selected = std::min_element(
        result.candidates.begin(),
        result.candidates.end(),
        better_candidate);
    result.selectedPatchFaceIndex = selected->patchFaceIndex;
    result.boundarySampleCount = selected->boundarySampleCount;
    result.projectedSampleCount = selected->projectedSampleCount;
    result.failedProjectionCount = selected->failedProjectionCount;
    result.maxProjectionDistance = selected->maxProjectionDistance;
    result.averageProjectionDistance = selected->averageProjectionDistance;

    if (!selected->accepted) {
        if (result.surfaceCoverageFailedProjectionCount == 0) {
            result.message = "No single imported Geomagic surface covers the original CAD boundary loop within projection tolerance.";
            result.warningMessage = "The patch requires multi-surface boundary-constrained shell construction; direct patch outer-boundary replacement remains disabled.";
        } else {
            result.message = "No imported Geomagic surface set covers the original CAD boundary loop within projection tolerance.";
            result.warningMessage = "Some original CAD boundary samples are not supported by any imported Geomagic surface; direct patch outer-boundary replacement remains disabled.";
        }
        return result;
    }

    const auto& selectedFace = patchFaces[static_cast<std::size_t>(result.selectedPatchFaceIndex)];
    TopoDS_Wire boundaryWire;
    if (options.rebuildBoundaryPcurves) {
        std::string strictWireMessage;
        boundaryWire = build_strict_projected_boundary_wire(
            document,
            boundary,
            selectedFace,
            options.projectionTolerance,
            result,
            strictWireMessage);
        if (boundaryWire.IsNull()) {
            return fail(strictWireMessage.empty()
                ? "Strict original-boundary re-trim failed to build a projected boundary wire."
                : strictWireMessage);
        }
    } else {
        boundaryWire = wire.wire;
        if (boundaryWire.IsNull()) {
            return fail("Boundary-constrained surface re-trim failed to build the original CAD boundary wire.");
        }
    }

    const auto surface = BRep_Tool::Surface(selectedFace);
    BRepBuilderAPI_MakeFace faceBuilder(surface, boundaryWire, Standard_True);
    if (!faceBuilder.IsDone()) {
        return fail("Could not strictly re-trim the selected Geomagic surface with the projected original CAD boundary wire.");
    }

    result.replacementFace = faceBuilder.Face();
    if (!sourceOrientationFace.IsNull()) {
        result.replacementFace.Orientation(sourceOrientationFace.Orientation());
    }
    ShapeFix_Face faceFixer(result.replacementFace);
    faceFixer.SetPrecision(options.projectionTolerance);
    faceFixer.Perform();
    const auto fixedFace = faceFixer.Face();
    if (!fixedFace.IsNull()) {
        result.replacementFace = fixedFace;
        if (!sourceOrientationFace.IsNull()) {
            result.replacementFace.Orientation(sourceOrientationFace.Orientation());
        }
    }
    BRepLib::BuildCurves3d(result.replacementFace);
    BRepLib::SameParameter(result.replacementFace, options.projectionTolerance, Standard_True);

    result.success = true;
    result.message = options.rebuildBoundaryPcurves
        ? "Strictly re-trimmed selected Geomagic surface with original CAD boundary curves and rebuilt pcurves."
        : "Re-trimmed selected Geomagic surface with the original CAD boundary wire.";
    return result;
}

}
