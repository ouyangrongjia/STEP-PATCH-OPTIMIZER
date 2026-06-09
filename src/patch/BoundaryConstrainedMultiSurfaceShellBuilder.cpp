#include "patch/BoundaryConstrainedMultiSurfaceShellBuilder.h"

#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeFix_Face.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Wire.hxx>
#include <TopTools_HSequenceOfShape.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace spo {

namespace {

struct OwnedBoundarySegment {
    int faceIndex = -1;
    EdgeId edgeId = 0;
    double firstParameter = 0.0;
    double lastParameter = 0.0;
};

BoundaryConstrainedMultiSurfaceShellResult fail(std::string message) {
    BoundaryConstrainedMultiSurfaceShellResult result;
    result.message = std::move(message);
    return result;
}

void append_unique_edge(std::vector<EdgeId>& edgeIds, EdgeId edgeId) {
    if (std::find(edgeIds.begin(), edgeIds.end(), edgeId) == edgeIds.end()) {
        edgeIds.push_back(edgeId);
    }
}

std::vector<TopoDS_Face> valid_faces(const MultiFacePatchAnalysis& analysis) {
    std::vector<TopoDS_Face> faces;
    for (const auto& face : analysis.faces) {
        if (!face.IsNull()) {
            faces.push_back(face);
        }
    }
    return faces;
}

std::vector<BoundarySurfaceSample> samples_for_edge(
    const std::vector<BoundarySurfaceSample>& samples,
    EdgeId edgeId) {
    std::vector<BoundarySurfaceSample> edgeSamples;
    for (const auto& sample : samples) {
        if (sample.edgeId == edgeId) {
            edgeSamples.push_back(sample);
        }
    }
    return edgeSamples;
}

TopoDS_Edge make_boundary_segment_edge(
    const ShapeDocument& document,
    EdgeId edgeId,
    double firstParameter,
    double lastParameter) {
    if (edgeId >= document.topology().edgeCount()) {
        return {};
    }

    const auto& sourceEdge = document.topology().edge(edgeId);
    double edgeFirst = 0.0;
    double edgeLast = 0.0;
    const auto curve = BRep_Tool::Curve(sourceEdge, edgeFirst, edgeLast);
    if (curve.IsNull()) {
        return {};
    }

    if (std::abs(lastParameter - firstParameter) <= Precision::PConfusion()) {
        return {};
    }

    BRepBuilderAPI_MakeEdge edgeBuilder(curve, firstParameter, lastParameter);
    if (!edgeBuilder.IsDone()) {
        return {};
    }
    return edgeBuilder.Edge();
}

std::vector<OwnedBoundarySegment> assign_boundary_segments(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& boundary,
    const std::vector<TopoDS_Face>& faces,
    const std::vector<BoundarySurfaceSample>& samples,
    double projectionTolerance,
    BoundaryConstrainedMultiSurfaceShellResult& result) {
    std::vector<OwnedBoundarySegment> segments;

    for (const auto edgeId : boundary.ordered_boundary_edges) {
        if (edgeId >= document.topology().edgeCount()) {
            append_unique_edge(result.failedEdgeIds, edgeId);
            continue;
        }

        auto edgeSamples = samples_for_edge(samples, edgeId);
        if (edgeSamples.size() < 2) {
            append_unique_edge(result.failedEdgeIds, edgeId);
            continue;
        }

        int selectedFace = -1;
        int selectedFailedCount = static_cast<int>(edgeSamples.size()) + 1;
        double selectedMaxDistance = std::numeric_limits<double>::infinity();
        double selectedAverageDistance = std::numeric_limits<double>::infinity();
        for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
            const auto surface = BRep_Tool::Surface(faces[faceIndex]);
            if (surface.IsNull()) {
                continue;
            }
            int failedCount = 0;
            double maxDistance = 0.0;
            double totalDistance = 0.0;
            for (const auto& sample : edgeSamples) {
                const auto distance = projectionDistanceToSurface(sample.point, surface);
                maxDistance = std::max(maxDistance, distance);
                totalDistance += distance;
                if (distance > projectionTolerance) {
                    ++failedCount;
                }
            }
            const auto averageDistance = totalDistance / static_cast<double>(edgeSamples.size());
            if (failedCount < selectedFailedCount ||
                (failedCount == selectedFailedCount && maxDistance < selectedMaxDistance) ||
                (failedCount == selectedFailedCount &&
                 maxDistance == selectedMaxDistance &&
                 averageDistance < selectedAverageDistance)) {
                selectedFace = static_cast<int>(faceIndex);
                selectedFailedCount = failedCount;
                selectedMaxDistance = maxDistance;
                selectedAverageDistance = averageDistance;
            }
        }

        if (selectedFace < 0 || selectedFailedCount > 0) {
            append_unique_edge(result.failedEdgeIds, edgeId);
            continue;
        }

        segments.push_back({selectedFace, edgeId, edgeSamples.front().parameter, edgeSamples.back().parameter});
    }

    return segments;
}

bool edge_is_in_list(const TopoDS_Edge& edge, const std::vector<TopoDS_Edge>& edges) {
    for (const auto& candidate : edges) {
        if (edge.IsSame(candidate)) {
            return true;
        }
    }
    return false;
}

std::vector<TopoDS_Edge> internal_edges_for_face(
    const TopoDS_Face& face,
    const std::vector<TopoDS_Edge>& outerEdges) {
    std::vector<TopoDS_Edge> result;
    if (face.IsNull()) {
        return result;
    }
    for (TopExp_Explorer explorer(face, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        auto edge = TopoDS::Edge(explorer.Current());
        if (!edge.IsNull() && !edge_is_in_list(edge, outerEdges)) {
            result.push_back(edge);
        }
    }
    return result;
}

TopoDS_Wire connect_one_closed_wire(
    const std::vector<TopoDS_Edge>& edges,
    double tolerance,
    int& openWireCount,
    std::string& message) {
    if (edges.empty()) {
        message = "No edges were available for a multi-surface replacement face.";
        return {};
    }

    Handle(TopTools_HSequenceOfShape) edgeSequence = new TopTools_HSequenceOfShape;
    for (const auto& edge : edges) {
        if (!edge.IsNull()) {
            edgeSequence->Append(edge);
        }
    }
    if (edgeSequence->Length() == 0) {
        message = "Only null edges were available for a multi-surface replacement face.";
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

    openWireCount += openWires->Length();
    if (closedWires->Length() != 1 || openWires->Length() != 0) {
        message = "Multi-surface replacement face edges did not form exactly one closed wire.";
        return {};
    }

    auto wire = TopoDS::Wire(closedWires->Value(1));
    if (wire.IsNull() || !wire.Closed()) {
        message = "Multi-surface replacement wire is not closed.";
        return {};
    }
    return wire;
}

TopoDS_Face build_face_on_surface(
    const TopoDS_Face& patchFace,
    const TopoDS_Wire& wire,
    double projectionTolerance,
    std::string& message) {
    const auto surface = BRep_Tool::Surface(patchFace);
    if (surface.IsNull()) {
        message = "Imported patch face has no usable surface.";
        return {};
    }

    BRepBuilderAPI_MakeFace faceBuilder(surface, wire, Standard_True);
    if (!faceBuilder.IsDone()) {
        message = "Could not build a multi-surface replacement face from the assigned wire.";
        return {};
    }

    auto face = faceBuilder.Face();
    face.Orientation(patchFace.Orientation());
    BRepLib::BuildCurves3d(face);
    BRepLib::SameParameter(face, projectionTolerance, Standard_True);

    ShapeFix_Face fixer(face);
    fixer.SetPrecision(projectionTolerance);
    fixer.Perform();
    auto fixedFace = fixer.Face();
    if (!fixedFace.IsNull()) {
        face = fixedFace;
    }

    return face;
}

}

BoundaryConstrainedMultiSurfaceShellResult BoundaryConstrainedMultiSurfaceShellBuilder::build(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& boundary,
    const MultiFacePatchAnalysis& analysis,
    const BoundaryConstrainedMultiSurfaceShellOptions& options) const {
    if (!document.hasShape()) {
        return fail("Multi-surface boundary shell requires a loaded document.");
    }
    if (!boundary.valid || boundary.ordered_boundary_edges.empty()) {
        return fail("Multi-surface boundary shell requires a valid ordered original CAD boundary.");
    }
    if (options.samplesPerEdge < 2) {
        return fail("Multi-surface boundary shell requires at least two samples per edge.");
    }
    if (options.projectionTolerance < 0.0 || options.wireConnectTolerance < 0.0) {
        return fail("Multi-surface boundary shell tolerances must not be negative.");
    }

    const auto faces = valid_faces(analysis);
    if (faces.size() < 2) {
        return fail("Multi-surface boundary shell requires at least two imported patch faces.");
    }

    BoundaryConstrainedMultiSurfaceShellResult result;
    const auto samples = sampleBoundaryForSurfaceProjection(document, boundary, options.samplesPerEdge);
    if (samples.empty()) {
        return fail("Multi-surface boundary shell could not sample the original CAD boundary.");
    }

    const auto coverage = evaluateBoundarySurfaceCoverage(
        faces,
        samples,
        options.projectionTolerance);
    result.boundarySampleCount = coverage.boundarySampleCount;
    result.projectedSampleCount = coverage.projectedSampleCount;
    result.failedProjectionCount = coverage.failedProjectionCount;
    result.maxProjectionDistance = coverage.maxProjectionDistance;
    result.averageProjectionDistance = coverage.averageProjectionDistance;
    result.failedEdgeIds = coverage.uncoveredEdgeIds;
    if (coverage.failedProjectionCount > 0) {
        result.message = "Multi-surface boundary shell failed because some original CAD boundary samples are not supported by any imported Geomagic surface.";
        return result;
    }

    const auto segments = assign_boundary_segments(
        document,
        boundary,
        faces,
        samples,
        options.projectionTolerance,
        result);
    result.assignedBoundarySegmentCount = static_cast<int>(segments.size());
    if (!result.failedEdgeIds.empty()) {
        result.message = "Multi-surface boundary shell could not assign all original CAD boundary edges to imported surfaces.";
        return result;
    }
    if (segments.empty()) {
        return fail("Multi-surface boundary shell produced no original CAD boundary segments.");
    }

    std::vector<std::vector<TopoDS_Edge>> faceEdges(faces.size());
    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        faceEdges[faceIndex] = internal_edges_for_face(faces[faceIndex], analysis.outerEdges);
    }
    for (const auto& segment : segments) {
        if (segment.faceIndex < 0 || static_cast<std::size_t>(segment.faceIndex) >= faces.size()) {
            append_unique_edge(result.failedEdgeIds, segment.edgeId);
            continue;
        }
        const auto edge = make_boundary_segment_edge(
            document,
            segment.edgeId,
            segment.firstParameter,
            segment.lastParameter);
        if (edge.IsNull()) {
            append_unique_edge(result.failedEdgeIds, segment.edgeId);
            continue;
        }
        faceEdges[static_cast<std::size_t>(segment.faceIndex)].push_back(edge);
    }
    if (!result.failedEdgeIds.empty()) {
        result.message = "Multi-surface boundary shell failed to create some original CAD boundary segment edges.";
        return result;
    }

    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        if (faceEdges[faceIndex].empty()) {
            continue;
        }

        std::string wireMessage;
        auto wire = connect_one_closed_wire(
            faceEdges[faceIndex],
            options.wireConnectTolerance,
            result.openWireCount,
            wireMessage);
        if (wire.IsNull()) {
            result.message = wireMessage.empty()
                ? "Multi-surface boundary shell failed to connect replacement face wire."
                : wireMessage;
            return result;
        }

        std::string faceMessage;
        auto face = build_face_on_surface(
            faces[faceIndex],
            wire,
            options.projectionTolerance,
            faceMessage);
        if (face.IsNull()) {
            result.message = faceMessage.empty()
                ? "Multi-surface boundary shell failed to build replacement face."
                : faceMessage;
            return result;
        }
        result.replacementFaces.push_back(face);
    }

    result.builtFaceCount = static_cast<int>(result.replacementFaces.size());
    if (result.replacementFaces.empty()) {
        return fail("Multi-surface boundary shell built no replacement faces.");
    }

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& face : result.replacementFaces) {
        builder.Add(compound, face);
    }

    result.replacementShape = compound;
    result.success = true;
    result.message = "Built strict multi-surface boundary-constrained replacement shell from Geomagic surfaces and the original CAD boundary.";
    return result;
}

}
