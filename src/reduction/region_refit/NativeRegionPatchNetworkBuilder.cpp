#include "reduction/region_refit/NativeRegionPatchNetworkBuilder.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Curve.hxx>
#include <ShapeAnalysis_Edge.hxx>
#include <ShapeConstruct_ProjectCurveOnSurface.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace spo {

namespace {

using MeshEdge = std::array<std::size_t, 2>;

MeshEdge orderedEdge(const std::size_t first, const std::size_t second) {
    return {std::min(first, second), std::max(first, second)};
}

std::vector<std::vector<std::size_t>> orderedComponents(
    const std::vector<MeshEdge>& edges,
    bool& valid) {
    valid = true;
    std::map<std::size_t, std::vector<std::size_t>> adjacency;
    std::set<MeshEdge> remaining;
    for (const auto& edge : edges) {
        if (edge[0] == edge[1]) continue;
        adjacency[edge[0]].push_back(edge[1]);
        adjacency[edge[1]].push_back(edge[0]);
        remaining.insert(orderedEdge(edge[0], edge[1]));
    }
    for (auto& [vertex, neighbors] : adjacency) {
        (void)vertex;
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
        if (neighbors.size() > 2) {
            valid = false;
            return {};
        }
    }

    std::vector<std::vector<std::size_t>> components;
    while (!remaining.empty()) {
        const auto seed = *remaining.begin();
        std::set<std::size_t> componentVertices;
        std::vector<std::size_t> stack {seed[0], seed[1]};
        while (!stack.empty()) {
            const auto current = stack.back();
            stack.pop_back();
            if (!componentVertices.insert(current).second) continue;
            for (const auto neighbor : adjacency[current]) stack.push_back(neighbor);
        }
        std::vector<std::size_t> endpoints;
        for (const auto vertex : componentVertices) {
            const auto degree = std::count_if(adjacency[vertex].begin(), adjacency[vertex].end(),
                [&](const auto neighbor) { return componentVertices.contains(neighbor); });
            if (degree == 1) endpoints.push_back(vertex);
        }
        if (endpoints.size() != 2) {
            valid = false;
            return {};
        }
        std::sort(endpoints.begin(), endpoints.end());
        std::vector<std::size_t> ordered {endpoints.front()};
        std::optional<std::size_t> previous;
        auto current = endpoints.front();
        while (current != endpoints.back()) {
            std::optional<std::size_t> next;
            for (const auto neighbor : adjacency[current]) {
                const auto edge = orderedEdge(current, neighbor);
                if (remaining.contains(edge) && (!previous.has_value() || neighbor != *previous)) {
                    next = neighbor;
                    break;
                }
            }
            if (!next.has_value()) {
                valid = false;
                return {};
            }
            remaining.erase(orderedEdge(current, *next));
            previous = current;
            current = *next;
            ordered.push_back(current);
        }
        components.push_back(std::move(ordered));
    }
    return components;
}

std::optional<EdgeId> commonBoundaryEdgeId(
    const RegionReferenceVertex& first,
    const RegionReferenceVertex& second) {
    std::optional<EdgeId> result;
    for (const auto edgeId : first.boundaryEdgeIds) {
        if (std::find(second.boundaryEdgeIds.begin(), second.boundaryEdgeIds.end(), edgeId) !=
            second.boundaryEdgeIds.end() && (!result.has_value() || edgeId < *result)) {
            result = edgeId;
        }
    }
    return result;
}

const RegionBoundaryEdgeConstraint* constraintFor(
    const RegionTopologyConstraints& constraints,
    const EdgeId edgeId) {
    const auto found = std::find_if(constraints.boundaryEdges.begin(), constraints.boundaryEdges.end(),
        [&](const auto& constraint) { return constraint.edgeId == edgeId; });
    return found == constraints.boundaryEdges.end() ? nullptr : &*found;
}

bool curveParameter(
    const Handle(Geom_Curve)& curve,
    const gp_Pnt& point,
    const double first,
    const double last,
    double& parameter) {
    try {
        GeomAPI_ProjectPointOnCurve projector(point, curve, first, last);
        if (projector.NbPoints() <= 0) return false;
        parameter = projector.LowerDistanceParameter();
        return std::isfinite(parameter);
    } catch (const Standard_Failure&) {
        return false;
    }
}

bool attachPcurve(
    const TopoDS_Edge& edge,
    const Handle(Geom_BSplineSurface)& surface,
    const double tolerance) {
    double first = 0.0;
    double last = 0.0;
    auto curve = BRep_Tool::Curve(edge, first, last);
    if (curve.IsNull() || std::abs(last - first) <= 1.0e-14) return false;
    Handle(Geom2d_Curve) pcurve;
    try {
        Handle(ShapeConstruct_ProjectCurveOnSurface) projector =
            new ShapeConstruct_ProjectCurveOnSurface;
        projector->Init(surface, tolerance);
        if (!projector->Perform(curve, first, last, pcurve, tolerance, tolerance) || pcurve.IsNull()) {
            return false;
        }
    } catch (const Standard_Failure&) {
        return false;
    }
    BRep_Builder builder;
    const TopLoc_Location location;
    builder.UpdateEdge(
        edge,
        pcurve,
        surface,
        location,
        tolerance,
        pcurve->Value(first),
        pcurve->Value(last));
    builder.Range(edge, first, last, Standard_False);
    builder.Range(edge, surface, location, first, last);
    builder.SameRange(edge, Standard_True);
    builder.UpdateEdge(edge, tolerance);
    return true;
}

TopoDS_Edge orientedForPair(
    TopoDS_Edge edge,
    const gp_Pnt& firstPoint,
    const gp_Pnt& secondPoint) {
    TopoDS_Vertex firstVertex;
    TopoDS_Vertex lastVertex;
    TopExp::Vertices(edge, firstVertex, lastVertex, Standard_True);
    if (firstVertex.IsNull() || lastVertex.IsNull()) return edge;
    const auto forward = BRep_Tool::Pnt(firstVertex).SquareDistance(firstPoint) +
        BRep_Tool::Pnt(lastVertex).SquareDistance(secondPoint);
    const auto reversed = BRep_Tool::Pnt(firstVertex).SquareDistance(secondPoint) +
        BRep_Tool::Pnt(lastVertex).SquareDistance(firstPoint);
    edge.Orientation(reversed < forward ? TopAbs_REVERSED : TopAbs_FORWARD);
    return edge;
}

TopoDS_Edge interpolatedSeamEdge(
    const RegionReferenceMesh& mesh,
    const std::vector<std::size_t>& vertices,
    const TopoDS_Vertex& firstVertex,
    const TopoDS_Vertex& lastVertex,
    const double tolerance) {
    if (vertices.size() < 2) return {};
    Handle(TColgp_HArray1OfPnt) points = new TColgp_HArray1OfPnt(1, static_cast<int>(vertices.size()));
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        points->SetValue(static_cast<int>(index + 1), mesh.vertices[vertices[index]].point);
    }
    try {
        GeomAPI_Interpolate interpolate(points, Standard_False, tolerance);
        interpolate.Perform();
        if (!interpolate.IsDone() || interpolate.Curve().IsNull()) return {};
        BRepBuilderAPI_MakeEdge edgeBuilder(interpolate.Curve(), firstVertex, lastVertex);
        return edgeBuilder.IsDone() ? edgeBuilder.Edge() : TopoDS_Edge {};
    } catch (const Standard_Failure&) {
        return {};
    }
}

}

bool NativeRegionPatchNetworkBuildResult::success() const {
    return status == RegionRefitStatus::Success && !replacementShape.IsNull() &&
        !replacementFaces.empty();
}

NativeRegionPatchNetworkBuildResult NativeRegionPatchNetworkBuilder::build(
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const RegionFittedPatchNetwork& network,
    const double curveOnSurfaceTolerance) const {
    NativeRegionPatchNetworkBuildResult result;
    if (mesh.empty() || network.surfaces.size() != network.layout.patches.size() ||
        network.layout.patchCount != static_cast<int>(network.surfaces.size()) ||
        curveOnSurfaceTolerance <= 0.0) {
        result.status = RegionRefitStatus::InvalidRequest;
        result.message = "Native patch-network construction received inconsistent geometry.";
        return result;
    }

    std::map<MeshEdge, TopoDS_Edge> edgeByMeshEdge;
    std::map<std::size_t, TopoDS_Vertex> sharedVertices;
    const auto rememberOriginalVertex = [&](const TopoDS_Vertex& vertex) {
        if (vertex.IsNull()) return;
        const auto point = BRep_Tool::Pnt(vertex);
        auto closest = mesh.vertices.size();
        auto squaredDistance = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
            if (!mesh.vertices[index].boundary) continue;
            const auto candidate = point.SquareDistance(mesh.vertices[index].point);
            if (candidate < squaredDistance) {
                closest = index;
                squaredDistance = candidate;
            }
        }
        if (closest < mesh.vertices.size() &&
            squaredDistance <= curveOnSurfaceTolerance * curveOnSurfaceTolerance) {
            sharedVertices[closest] = vertex;
        }
    };
    for (const auto& constraint : constraints.boundaryEdges) {
        TopoDS_Vertex firstVertex;
        TopoDS_Vertex lastVertex;
        TopExp::Vertices(constraint.originalEdge, firstVertex, lastVertex, Standard_True);
        rememberOriginalVertex(firstVertex);
        rememberOriginalVertex(lastVertex);
    }
    const auto sharedVertex = [&](const std::size_t meshVertex, const gp_Pnt& point) {
        const auto found = sharedVertices.find(meshVertex);
        if (found != sharedVertices.end()) return found->second;
        auto vertex = BRepBuilderAPI_MakeVertex(point).Vertex();
        BRep_Builder builder;
        builder.UpdateVertex(vertex, curveOnSurfaceTolerance);
        sharedVertices.emplace(meshVertex, vertex);
        return vertex;
    };
    std::map<std::pair<EdgeId, int>, std::vector<MeshEdge>> boundaryEdgesByOwner;
    for (std::size_t patch = 0; patch < network.layout.patches.size(); ++patch) {
        const auto& loop = network.layout.patches[patch].boundaryLoop;
        for (std::size_t index = 0; index < loop.size(); ++index) {
            const auto first = loop[index];
            const auto second = loop[(index + 1) % loop.size()];
            const auto edgeId = commonBoundaryEdgeId(mesh.vertices[first], mesh.vertices[second]);
            if (edgeId.has_value()) {
                boundaryEdgesByOwner[{*edgeId, static_cast<int>(patch)}].push_back(orderedEdge(first, second));
            }
        }
    }

    std::map<EdgeId, int> boundarySegmentCounts;
    for (const auto& [owner, meshEdges] : boundaryEdgesByOwner) {
        bool valid = false;
        const auto components = orderedComponents(meshEdges, valid);
        if (!valid) {
            result.message = "An original boundary edge has a branched patch ownership chain.";
            return result;
        }
        boundarySegmentCounts[owner.first] += static_cast<int>(components.size());
    }

    for (const auto& [owner, meshEdges] : boundaryEdgesByOwner) {
        const auto* constraint = constraintFor(constraints, owner.first);
        if (constraint == nullptr || constraint->originalEdge.IsNull()) {
            result.message = "A patch boundary segment has no original STEP edge constraint.";
            return result;
        }
        double originalFirst = 0.0;
        double originalLast = 0.0;
        const auto originalCurve = BRep_Tool::Curve(
            constraint->originalEdge, originalFirst, originalLast);
        if (originalCurve.IsNull()) {
            result.message = "An original STEP boundary edge has no three-dimensional curve.";
            return result;
        }
        bool valid = false;
        const auto components = orderedComponents(meshEdges, valid);
        if (!valid) {
            result.message = "An original boundary segment could not be ordered.";
            return result;
        }
        for (const auto& vertices : components) {
            double firstParameter = 0.0;
            double lastParameter = 0.0;
            if (!curveParameter(originalCurve, mesh.vertices[vertices.front()].point,
                    originalFirst, originalLast, firstParameter) ||
                !curveParameter(originalCurve, mesh.vertices[vertices.back()].point,
                    originalFirst, originalLast, lastParameter)) {
                result.message = "Could not recover an original boundary segment parameter range.";
                return result;
            }
            const auto minimum = std::min(firstParameter, lastParameter);
            const auto maximum = std::max(firstParameter, lastParameter);
            const auto parameterTolerance = std::max(1.0e-10, std::abs(originalLast - originalFirst) * 1.0e-9);
            TopoDS_Edge edge;
            const auto fullRange = boundarySegmentCounts[owner.first] == 1 &&
                std::abs(minimum - std::min(originalFirst, originalLast)) <= parameterTolerance &&
                std::abs(maximum - std::max(originalFirst, originalLast)) <= parameterTolerance;
            if (fullRange) {
                edge = constraint->originalEdge;
                ++result.reusedOriginalBoundaryEdgeCount;
            } else {
                const auto minimumVertexIndex = firstParameter <= lastParameter
                    ? vertices.front()
                    : vertices.back();
                const auto maximumVertexIndex = firstParameter <= lastParameter
                    ? vertices.back()
                    : vertices.front();
                const auto minimumVertex = sharedVertex(
                    minimumVertexIndex, originalCurve->Value(minimum));
                const auto maximumVertex = sharedVertex(
                    maximumVertexIndex, originalCurve->Value(maximum));
                BRepBuilderAPI_MakeEdge edgeBuilder(
                    originalCurve, minimumVertex, maximumVertex, minimum, maximum);
                if (!edgeBuilder.IsDone()) {
                    result.message = "Could not split an original STEP boundary edge.";
                    return result;
                }
                edge = edgeBuilder.Edge();
                result.splitBoundarySegments.push_back({
                    owner.first, minimum, maximum, edge, owner.second});
                ++result.splitBoundarySegmentCount;
            }
            for (std::size_t index = 0; index + 1 < vertices.size(); ++index) {
                edgeByMeshEdge[orderedEdge(vertices[index], vertices[index + 1])] = edge;
            }
        }
    }

    for (const auto& seam : network.layout.seams) {
        bool valid = false;
        const auto components = orderedComponents(seam.meshEdges, valid);
        if (!valid || components.size() != 1) {
            result.message = "A layout seam is not one unbranched connected chain.";
            return result;
        }
        const auto& vertices = components.front();
        const auto firstVertex = sharedVertex(vertices.front(), mesh.vertices[vertices.front()].point);
        const auto lastVertex = sharedVertex(vertices.back(), mesh.vertices[vertices.back()].point);
        auto edge = interpolatedSeamEdge(
            mesh,
            vertices,
            firstVertex,
            lastVertex,
            curveOnSurfaceTolerance * 0.5);
        if (edge.IsNull()) {
            result.message = "Could not interpolate a shared three-dimensional layout seam.";
            return result;
        }
        result.internalSeamEdges.push_back(edge);
        ++result.internalSeamCount;
        for (std::size_t index = 0; index + 1 < vertices.size(); ++index) {
            edgeByMeshEdge[orderedEdge(vertices[index], vertices[index + 1])] = edge;
        }
    }

    result.replacementFaces.reserve(network.layout.patches.size());
    for (std::size_t patch = 0; patch < network.layout.patches.size(); ++patch) {
        const auto& parameterization = network.layout.patches[patch];
        const auto& loop = parameterization.boundaryLoop;
        if (loop.size() < 3) {
            result.message = "A fitted patch has a degenerate boundary loop.";
            return result;
        }
        std::vector<TopoDS_Edge> loopEdges;
        loopEdges.reserve(loop.size());
        for (std::size_t index = 0; index < loop.size(); ++index) {
            const auto found = edgeByMeshEdge.find(orderedEdge(loop[index], loop[(index + 1) % loop.size()]));
            if (found == edgeByMeshEdge.end()) {
                result.message = "A fitted patch boundary mesh edge has no shared B-rep edge.";
                return result;
            }
            loopEdges.push_back(found->second);
        }
        std::size_t start = 0;
        while (start < loopEdges.size() && loopEdges[start].IsSame(
                loopEdges[(start + loopEdges.size() - 1) % loopEdges.size()])) {
            ++start;
        }
        if (start == loopEdges.size()) {
            result.message = "A fitted patch boundary collapsed to one B-rep edge.";
            return result;
        }
        BRepBuilderAPI_MakeWire wireBuilder;
        std::vector<TopoDS_Edge> distinctEdges;
        for (std::size_t offset = 0; offset < loopEdges.size(); ++offset) {
            const auto index = (start + offset) % loopEdges.size();
            const auto previous = (index + loopEdges.size() - 1) % loopEdges.size();
            if (loopEdges[index].IsSame(loopEdges[previous])) continue;
            auto edge = loopEdges[index];
            if (!attachPcurve(edge, network.surfaces[patch], curveOnSurfaceTolerance)) {
                result.message = "Could not project a shared B-rep edge onto a fitted patch surface.";
                return result;
            }
            ++result.pcurveProjectionCount;
            edge = orientedForPair(
                edge,
                mesh.vertices[loop[index]].point,
                mesh.vertices[loop[(index + 1) % loop.size()]].point);
            wireBuilder.Add(edge);
            distinctEdges.push_back(edge);
        }
        if (!wireBuilder.IsDone()) {
            result.message = "Could not assemble a closed fitted patch wire.";
            return result;
        }
        BRep_Builder faceBuilder;
        TopoDS_Face face;
        faceBuilder.MakeFace(face, network.surfaces[patch], TopLoc_Location {}, curveOnSurfaceTolerance);
        faceBuilder.Add(face, wireBuilder.Wire());
        if (!parameterization.vertexIndices.empty()) {
            const auto local = std::size_t {0};
            const auto uv = parameterization.parameters[local];
            gp_Pnt point;
            gp_Vec du;
            gp_Vec dv;
            network.surfaces[patch]->D1(
                std::clamp((uv[0] + 1.0) * 0.5, 0.0, 1.0),
                std::clamp((uv[1] + 1.0) * 0.5, 0.0, 1.0),
                point,
                du,
                dv);
            du.Cross(dv);
            if (du.SquareMagnitude() > 1.0e-24 &&
                gp_Dir(du).Dot(mesh.vertices[parameterization.vertexIndices[local]].normal) < 0.0) {
                face.Reverse();
            }
        }
        ShapeAnalysis_Edge edgeAnalyzer;
        BRep_Builder topologyBuilder;
        for (const auto& edge : distinctEdges) {
            Standard_Real deviation = 0.0;
            ++result.sameParameterCheckCount;
            edgeAnalyzer.CheckSameParameter(edge, face, deviation, 97);
            result.maximumSameParameterDeviation = std::max(
                result.maximumSameParameterDeviation,
                std::isfinite(deviation) ? static_cast<double>(deviation) :
                    std::numeric_limits<double>::infinity());
            if (!std::isfinite(deviation) || deviation > curveOnSurfaceTolerance) {
                ++result.sameParameterFailureCount;
            } else {
                topologyBuilder.SameParameter(edge, Standard_True);
            }
        }
        if (result.sameParameterFailureCount > 0 || !BRepCheck_Analyzer(face).IsValid()) {
            result.message = "A trimmed fitted patch failed dense SameParameter or BRep validation.";
            return result;
        }
        result.replacementFaces.push_back(face);
    }

    if (static_cast<int>(result.replacementFaces.size()) != network.layout.patchCount) {
        result.message = "The native replacement B-rep face count does not match the selected layout.";
        return result;
    }
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& face : result.replacementFaces) builder.Add(compound, face);
    result.replacementShape = compound;
    result.status = RegionRefitStatus::Success;
    result.message = "Built a native trimmed multi-surface patch network with shared B-rep edges.";
    return result;
}

}
