#include "patch/BoundaryConstrainedMultiSurfaceShellBuilder.h"

#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"

#include <BRepGProp.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
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
    bool splitSegment = false;
};

struct SurfaceOwner {
    int faceIndex = -1;
    double distance = std::numeric_limits<double>::infinity();
};

struct FaceEdgeCandidate {
    TopoDS_Edge edge;
    bool originalBoundarySegment = false;
    EdgeId sourceEdgeId = 0;
};

struct WireBuildResult {
    std::vector<TopoDS_Wire> wires;
    int inputEdgeCount = 0;
    int closedWireCount = 0;
    int openWireCount = 0;
    int ignoredOpenWireCount = 0;
    bool multipleClosedWires = false;
    std::vector<EdgeId> openOriginalBoundaryEdgeIds;
    std::string message;
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

void append_warning(std::string& warning, const std::string& message) {
    if (message.empty()) {
        return;
    }
    if (!warning.empty()) {
        warning += " ";
    }
    warning += message;
}

bool edge_id_in_list(const std::vector<EdgeId>& edgeIds, EdgeId edgeId) {
    return std::find(edgeIds.begin(), edgeIds.end(), edgeId) != edgeIds.end();
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

SurfaceOwner nearest_owner(
    const std::vector<TopoDS_Face>& faces,
    const BoundarySurfaceSample& sample) {
    SurfaceOwner owner;
    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto surface = BRep_Tool::Surface(faces[faceIndex]);
        if (surface.IsNull()) {
            continue;
        }
        const auto distance = projectionDistanceToSurface(sample.point, surface);
        if (distance < owner.distance) {
            owner.faceIndex = static_cast<int>(faceIndex);
            owner.distance = distance;
        }
    }
    return owner;
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
    BoundaryConstrainedMultiSurfaceShellResult& result,
    const std::vector<EdgeId>& forceWholeEdgeIds = {}) {
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
            std::vector<SurfaceOwner> owners;
            owners.reserve(edgeSamples.size());
            bool splitSupported = true;
            for (const auto& sample : edgeSamples) {
                const auto owner = nearest_owner(faces, sample);
                if (owner.faceIndex < 0 || owner.distance > projectionTolerance) {
                    splitSupported = false;
                }
                owners.push_back(owner);
            }

            if (!splitSupported) {
                append_unique_edge(result.failedEdgeIds, edgeId);
                continue;
            }

            if (selectedFace >= 0 && edge_id_in_list(forceWholeEdgeIds, edgeId)) {
                segments.push_back({selectedFace, edgeId, edgeSamples.front().parameter, edgeSamples.back().parameter, false});
                continue;
            }

            ++result.splitBoundaryEdgeCount;
            auto currentFace = owners.front().faceIndex;
            auto segmentStart = edgeSamples.front().parameter;
            for (std::size_t sampleIndex = 1; sampleIndex < edgeSamples.size(); ++sampleIndex) {
                if (owners[sampleIndex].faceIndex == currentFace) {
                    continue;
                }
                const auto splitParameter = (edgeSamples[sampleIndex - 1].parameter + edgeSamples[sampleIndex].parameter) * 0.5;
                segments.push_back({currentFace, edgeId, segmentStart, splitParameter, true});
                segmentStart = splitParameter;
                currentFace = owners[sampleIndex].faceIndex;
            }
            segments.push_back({currentFace, edgeId, segmentStart, edgeSamples.back().parameter, true});
            continue;
        }

        segments.push_back({selectedFace, edgeId, edgeSamples.front().parameter, edgeSamples.back().parameter, false});
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

std::vector<FaceEdgeCandidate> internal_edges_for_face(
    const TopoDS_Face& face,
    const std::vector<TopoDS_Edge>& outerEdges) {
    std::vector<FaceEdgeCandidate> result;
    if (face.IsNull()) {
        return result;
    }
    for (TopExp_Explorer explorer(face, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        auto edge = TopoDS::Edge(explorer.Current());
        if (!edge.IsNull() && !edge_is_in_list(edge, outerEdges)) {
            result.push_back({edge, false, 0});
        }
    }
    return result;
}

double edge_length(const TopoDS_Edge& edge) {
    if (edge.IsNull()) {
        return 0.0;
    }
    GProp_GProps properties;
    BRepGProp::LinearProperties(edge, properties);
    return std::max(0.0, properties.Mass());
}

double wire_length(const TopoDS_Wire& wire) {
    double length = 0.0;
    for (TopExp_Explorer explorer(wire, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        length += edge_length(TopoDS::Edge(explorer.Current()));
    }
    return length;
}

bool wire_contains_edge(const TopoDS_Wire& wire, const TopoDS_Edge& edge) {
    if (wire.IsNull() || edge.IsNull()) {
        return false;
    }
    for (TopExp_Explorer explorer(wire, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        if (TopoDS::Edge(explorer.Current()).IsSame(edge)) {
            return true;
        }
    }
    return false;
}

bool wire_contains_original_boundary_segment(
    const TopoDS_Wire& wire,
    const std::vector<FaceEdgeCandidate>& candidates) {
    for (const auto& candidate : candidates) {
        if (candidate.originalBoundarySegment && wire_contains_edge(wire, candidate.edge)) {
            return true;
        }
    }
    return false;
}

bool wires_contain_original_boundary_segment(
    const Handle(TopTools_HSequenceOfShape)& wires,
    const std::vector<FaceEdgeCandidate>& candidates) {
    for (int index = 1; index <= wires->Length(); ++index) {
        if (wire_contains_original_boundary_segment(TopoDS::Wire(wires->Value(index)), candidates)) {
            return true;
        }
    }
    return false;
}

std::vector<EdgeId> original_boundary_edge_ids_in_wires(
    const Handle(TopTools_HSequenceOfShape)& wires,
    const std::vector<FaceEdgeCandidate>& candidates) {
    std::vector<EdgeId> edgeIds;
    for (int index = 1; index <= wires->Length(); ++index) {
        const auto wire = TopoDS::Wire(wires->Value(index));
        for (const auto& candidate : candidates) {
            if (!candidate.originalBoundarySegment) {
                continue;
            }
            if (wire_contains_edge(wire, candidate.edge)) {
                append_unique_edge(edgeIds, candidate.sourceEdgeId);
            }
        }
    }
    return edgeIds;
}

std::vector<TopoDS_Wire> collect_closed_wires(
    const Handle(TopTools_HSequenceOfShape)& closedWires,
    const std::vector<FaceEdgeCandidate>& candidates) {
    struct CandidateWire {
        TopoDS_Wire wire;
        bool hasBoundary = false;
        double length = 0.0;
    };

    std::vector<CandidateWire> candidateWires;
    for (int index = 1; index <= closedWires->Length(); ++index) {
        auto wire = TopoDS::Wire(closedWires->Value(index));
        if (wire.IsNull() || !wire.Closed()) {
            continue;
        }

        candidateWires.push_back({
            wire,
            wire_contains_original_boundary_segment(wire, candidates),
            wire_length(wire)});
    }

    std::sort(candidateWires.begin(), candidateWires.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.hasBoundary != rhs.hasBoundary) {
            return lhs.hasBoundary;
        }
        return lhs.length > rhs.length;
    });

    std::vector<TopoDS_Wire> wires;
    wires.reserve(candidateWires.size());
    for (const auto& candidateWire : candidateWires) {
        wires.push_back(candidateWire.wire);
    }
    return wires;
}

WireBuildResult connect_one_closed_wire(
    const std::vector<FaceEdgeCandidate>& candidates,
    double tolerance) {
    WireBuildResult result;
    if (candidates.empty()) {
        result.message = "No edges were available for a multi-surface replacement face.";
        return result;
    }

    Handle(TopTools_HSequenceOfShape) edgeSequence = new TopTools_HSequenceOfShape;
    for (const auto& candidate : candidates) {
        if (!candidate.edge.IsNull()) {
            edgeSequence->Append(candidate.edge);
        }
    }
    result.inputEdgeCount = edgeSequence->Length();
    if (edgeSequence->Length() == 0) {
        result.message = "Only null edges were available for a multi-surface replacement face.";
        return result;
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

    result.closedWireCount = closedWires->Length();
    result.openWireCount = openWires->Length();
    result.openOriginalBoundaryEdgeIds = original_boundary_edge_ids_in_wires(openWires, candidates);
    const bool openWireHasOriginalBoundary = !result.openOriginalBoundaryEdgeIds.empty();
    if (openWires->Length() != 0 && openWireHasOriginalBoundary) {
        result.message = "Multi-surface replacement face edges produced open wires.";
        return result;
    }
    if (closedWires->Length() < 1) {
        if (openWires->Length() != 0 && !openWireHasOriginalBoundary) {
            result.ignoredOpenWireCount = openWires->Length();
            return result;
        }
        result.message = "Multi-surface replacement face edges did not form a closed wire.";
        return result;
    }

    result.wires = collect_closed_wires(closedWires, candidates);
    result.ignoredOpenWireCount = openWires->Length();
    result.multipleClosedWires = closedWires->Length() > 1;
    if (result.wires.empty()) {
        result.message = "Multi-surface replacement wire is not closed.";
        return result;
    }
    return result;
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
    result.attempted = true;
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

    std::vector<EdgeId> forceWholeEdgeIds;
    bool retriedWholeEdgeAssignment = false;
    std::string retryWarning;

retry_boundary_assignment:
    result.assignedBoundarySegmentCount = 0;
    result.splitBoundaryEdgeCount = 0;
    result.builtFaceCount = 0;
    result.closedWireCount = 0;
    result.openWireCount = 0;
    result.multipleClosedWireFaceCount = 0;
    result.failedPatchFaceIndex = -1;
    result.failedFaceEdgeCount = 0;
    result.replacementFaces.clear();
    result.replacementShape.Nullify();
    result.splitBoundarySegments.clear();
    result.failedEdgeIds = coverage.uncoveredEdgeIds;
    result.message.clear();
    result.warningMessage = retryWarning;

    const auto segments = assign_boundary_segments(
        document,
        boundary,
        faces,
        samples,
        options.projectionTolerance,
        result,
        forceWholeEdgeIds);
    result.assignedBoundarySegmentCount = static_cast<int>(segments.size());
    if (!result.failedEdgeIds.empty()) {
        result.message = "Multi-surface boundary shell could not assign all original CAD boundary edges to imported surfaces.";
        return result;
    }
    if (segments.empty()) {
        return fail("Multi-surface boundary shell produced no original CAD boundary segments.");
    }

    std::vector<std::vector<FaceEdgeCandidate>> faceEdges(faces.size());
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
        if (segment.splitSegment) {
            result.splitBoundarySegments.push_back({
                segment.edgeId,
                segment.firstParameter,
                segment.lastParameter,
                edge});
        }
        faceEdges[static_cast<std::size_t>(segment.faceIndex)].push_back({edge, true, segment.edgeId});
    }
    if (!result.failedEdgeIds.empty()) {
        result.message = "Multi-surface boundary shell failed to create some original CAD boundary segment edges.";
        return result;
    }

    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        if (faceEdges[faceIndex].empty()) {
            continue;
        }

        const auto wireResult = connect_one_closed_wire(
            faceEdges[faceIndex],
            options.wireConnectTolerance);
        result.closedWireCount += wireResult.closedWireCount;
        result.openWireCount += wireResult.openWireCount;
        if (wireResult.multipleClosedWires) {
            ++result.multipleClosedWireFaceCount;
        }
        if (wireResult.ignoredOpenWireCount > 0) {
            append_warning(
                result.warningMessage,
                "Ignored open imported-patch internal seam wires that do not contain original CAD boundary segments.");
        }
        if (wireResult.wires.empty()) {
            if (wireResult.ignoredOpenWireCount > 0 && wireResult.openWireCount == wireResult.ignoredOpenWireCount) {
                continue;
            }
            if (!retriedWholeEdgeAssignment &&
                result.builtFaceCount > 0 &&
                !wireResult.openOriginalBoundaryEdgeIds.empty()) {
                forceWholeEdgeIds = wireResult.openOriginalBoundaryEdgeIds;
                retriedWholeEdgeAssignment = true;
                retryWarning = "Split boundary edge assignment produced open original-boundary wires; retried failed edge(s) as whole-edge surface assignments.";
                goto retry_boundary_assignment;
            }
            result.failedPatchFaceIndex = static_cast<int>(faceIndex);
            result.failedFaceEdgeCount = wireResult.inputEdgeCount;
            for (const auto edgeId : wireResult.openOriginalBoundaryEdgeIds) {
                append_unique_edge(result.failedEdgeIds, edgeId);
            }
            result.message = wireResult.message.empty()
                ? "Multi-surface boundary shell failed to connect replacement face wire."
                : wireResult.message;
            return result;
        }

        for (const auto& wire : wireResult.wires) {
            std::string faceMessage;
            auto face = build_face_on_surface(
                faces[faceIndex],
                wire,
                options.projectionTolerance,
                faceMessage);
            if (face.IsNull()) {
                result.failedPatchFaceIndex = static_cast<int>(faceIndex);
                result.failedFaceEdgeCount = wireResult.inputEdgeCount;
                result.message = faceMessage.empty()
                    ? "Multi-surface boundary shell failed to build replacement face."
                    : faceMessage;
                return result;
            }
            result.replacementFaces.push_back(face);
            result.builtFaceCount = static_cast<int>(result.replacementFaces.size());
        }
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
    if (result.multipleClosedWireFaceCount > 0) {
        append_warning(
            result.warningMessage,
            "Some multi-surface face edge sets produced multiple closed wires; each closed wire was built as a replacement face.");
    }
    return result;
}

}
