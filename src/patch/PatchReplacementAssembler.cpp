#include "patch/PatchReplacementAssembler.h"

#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"
#include "merge/BoundaryWireBuilder.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/BoundaryConstrainedPatchBuilder.h"
#include "patch/BoundaryConstrainedSurfaceRetrim.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRep_Builder.hxx>
#include <BRepLib.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools_ReShape.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_Curve.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_Edge.hxx>
#include <ShapeConstruct_ProjectCurveOnSurface.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace spo {

namespace {

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

PatchReplacementAssemblyResult assemble_face_compound_replacement(
    const ShapeDocument& beforeDocument,
    const BoundaryConstrainedPatchBuildResult& buildResult) {
    PatchReplacementAssemblyResult result;
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
    result.warning = "Boundary-constrained replacement used face-compound assembly before repair sewing.";
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

PatchReplacementAssemblyResult assemble_replacement_shape(
    const ShapeDocument& beforeDocument,
    const RegionBoundaryAnalysis& boundary,
    const BoundaryConstrainedPatchBuildResult& buildResult,
    bool forceFaceCompoundAssembly) {
    PatchReplacementAssemblyResult result;
    if (buildResult.usedMultiSurfaceBoundaryShell ||
        (forceFaceCompoundAssembly && buildResult.usedOriginalBoundarySurfaceRetrim)) {
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

}

PatchReplacementAssemblyResult PatchReplacementAssembler::assemble(
    const ShapeDocument& beforeDocument,
    const RegionBoundaryAnalysis& boundary,
    const BoundaryConstrainedPatchBuildResult& buildResult,
    const bool forceFaceCompoundAssembly) const {
    return assemble_replacement_shape(
        beforeDocument,
        boundary,
        buildResult,
        forceFaceCompoundAssembly);
}

}
