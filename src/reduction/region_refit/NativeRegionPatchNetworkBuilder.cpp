#include "reduction/region_refit/NativeRegionPatchNetworkBuilder.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom2dAPI_Interpolate.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeAnalysis_Edge.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TColgp_HArray1OfPnt2d.hxx>
#include <TColStd_HArray1OfReal.hxx>
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
#include <unordered_map>
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
    const Handle(Geom2d_Curve)& pcurve,
    const double first,
    const double last,
    const double tolerance) {
    if (edge.IsNull() || surface.IsNull() || pcurve.IsNull() ||
        std::abs(last - first) <= 1.0e-14) return false;
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
    builder.SameRange(edge, Standard_False);
    builder.SameParameter(edge, Standard_False);
    builder.UpdateEdge(edge, tolerance);
    BRepLib::SameRange(edge, tolerance);
    BRepLib::SameParameter(edge, tolerance);
    return true;
}

Handle(Geom2d_Curve) parameterPcurve(
    const TopoDS_Edge& edge,
    const std::vector<std::pair<gp_Pnt, gp_Pnt2d>>& points,
    double& first,
    double& last,
    const double tolerance) {
    const auto curve = BRep_Tool::Curve(edge, first, last);
    if (curve.IsNull() || points.size() < 2 || std::abs(last - first) <= 1.0e-14) return {};
    std::vector<std::pair<double, gp_Pnt2d>> samples;
    samples.reserve(points.size());
    for (const auto& [point, uv] : points) {
        double parameter = 0.0;
        if (!curveParameter(curve, point, first, last, parameter)) return {};
        samples.emplace_back(parameter, uv);
    }
    std::sort(samples.begin(), samples.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    samples.erase(std::unique(samples.begin(), samples.end(), [&](const auto& lhs, const auto& rhs) {
        return std::abs(lhs.first - rhs.first) <= std::max(1.0e-12, std::abs(last - first) * 1.0e-12);
    }), samples.end());
    if (samples.size() < 2) return {};
    first = samples.front().first;
    last = samples.back().first;
    Handle(TColgp_HArray1OfPnt2d) interpolationPoints =
        new TColgp_HArray1OfPnt2d(1, static_cast<int>(samples.size()));
    Handle(TColStd_HArray1OfReal) parameters =
        new TColStd_HArray1OfReal(1, static_cast<int>(samples.size()));
    for (std::size_t index = 0; index < samples.size(); ++index) {
        interpolationPoints->SetValue(static_cast<int>(index + 1), samples[index].second);
        parameters->SetValue(static_cast<int>(index + 1), samples[index].first);
    }
    try {
        Geom2dAPI_Interpolate interpolate(
            interpolationPoints, parameters, Standard_False, tolerance * 0.1);
        interpolate.Perform();
        if (!interpolate.IsDone()) return {};
        Handle(Geom2d_Curve) result = interpolate.Curve();
        return result;
    } catch (const Standard_Failure&) {
        return {};
    }
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
        const auto curve = interpolate.Curve();
        BRepBuilderAPI_MakeEdge edgeBuilder(
            curve,
            firstVertex,
            lastVertex,
            curve->FirstParameter(),
            curve->LastParameter());
        return edgeBuilder.IsDone() ? edgeBuilder.Edge() : TopoDS_Edge {};
    } catch (const Standard_Failure&) {
        return {};
    }
}

double percentile95(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const auto index = std::min(
        values.size() - 1,
        static_cast<std::size_t>(std::ceil(static_cast<double>(values.size()) * 0.95)) - 1);
    std::nth_element(
        values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

double pointSegmentDistance(
    const gp_Pnt& point,
    const gp_Pnt& first,
    const gp_Pnt& second) {
    const gp_Vec segment(first, second);
    if (segment.SquareMagnitude() <= 1.0e-24) return point.Distance(first);
    const auto parameter = std::clamp(
        gp_Vec(first, point).Dot(segment) / segment.SquareMagnitude(), 0.0, 1.0);
    return point.Distance(first.Translated(segment * parameter));
}

double polylineSeparation(
    const std::vector<gp_Pnt>& first,
    const std::vector<gp_Pnt>& second) {
    if (first.size() < 2 || second.size() < 2) {
        return std::numeric_limits<double>::infinity();
    }
    const auto directed = [](const std::vector<gp_Pnt>& points, const std::vector<gp_Pnt>& target) {
        auto maximum = 0.0;
        for (const auto& point : points) {
            auto minimum = std::numeric_limits<double>::infinity();
            for (std::size_t index = 0; index + 1 < target.size(); ++index) {
                minimum = std::min(minimum, pointSegmentDistance(point, target[index], target[index + 1]));
            }
            maximum = std::max(maximum, minimum);
        }
        return maximum;
    };
    return std::max(directed(first, second), directed(second, first));
}

std::vector<gp_Pnt> edgePolyline(
    const TopoDS_Edge& edge,
    const TopoDS_Face& face) {
    TopLoc_Location location;
    const auto triangulation = BRep_Tool::Triangulation(face, location);
    if (triangulation.IsNull()) return {};
    const auto polygon = BRep_Tool::PolygonOnTriangulation(edge, triangulation, location);
    if (polygon.IsNull()) return {};
    std::vector<gp_Pnt> points;
    const auto& nodes = polygon->Nodes();
    points.reserve(static_cast<std::size_t>(nodes.Length()));
    for (auto index = nodes.Lower(); index <= nodes.Upper(); ++index) {
        points.push_back(triangulation->Node(nodes.Value(index)).Transformed(location.Transformation()));
    }
    return points;
}

std::vector<RegionEdgeConsistencyReport> measureEdgeConsistency(
    const TopoDS_Shape& replacementShape,
    const std::vector<TopoDS_Face>& faces,
    const std::vector<TopoDS_Edge>& internalSeams,
    const double tolerance) {
    struct EdgeGroup {
        TopoDS_Edge edge;
        bool internal = false;
        std::vector<TopoDS_Face> faces;
    };
    std::vector<EdgeGroup> groups;
    for (const auto& face : faces) {
        for (TopExp_Explorer explorer(face, TopAbs_EDGE); explorer.More(); explorer.Next()) {
            const auto edge = TopoDS::Edge(explorer.Current());
            auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
                return group.edge.IsSame(edge);
            });
            if (found == groups.end()) {
                EdgeGroup group;
                group.edge = edge;
                group.internal = std::any_of(internalSeams.begin(), internalSeams.end(), [&](const auto& seam) {
                    return seam.IsSame(edge);
                });
                group.faces.push_back(face);
                groups.push_back(std::move(group));
            } else if (std::none_of(found->faces.begin(), found->faces.end(), [&](const auto& existing) {
                    return existing.IsSame(face);
                })) {
                found->faces.push_back(face);
            }
        }
    }

    std::vector<RegionEdgeConsistencyReport> reports(groups.size());
    for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const auto& group = groups[groupIndex];
        auto& report = reports[groupIndex];
        report.edgeIndex = static_cast<int>(groupIndex);
        report.internalSeam = group.internal;
        report.expectedFaceUseCount = group.internal ? 2 : 1;
        report.actualFaceUseCount = static_cast<int>(group.faces.size());
        report.sharedSameEdge = report.actualFaceUseCount == report.expectedFaceUseCount;
        report.freeEdge = group.internal && report.actualFaceUseCount < 2;
        report.multipleEdge = report.actualFaceUseCount > report.expectedFaceUseCount;
        report.sameRange = BRep_Tool::SameRange(group.edge);
        report.sameParameter = BRep_Tool::SameParameter(group.edge);

        double curveFirst = 0.0;
        double curveLast = 0.0;
        const auto curve = BRep_Tool::Curve(group.edge, curveFirst, curveLast);
        std::vector<double> deviations;
        if (!curve.IsNull()) {
            for (const auto& face : group.faces) {
                double pcurveFirst = 0.0;
                double pcurveLast = 0.0;
                const auto pcurve = BRep_Tool::CurveOnSurface(
                    group.edge, face, pcurveFirst, pcurveLast);
                const auto surface = BRep_Tool::Surface(face);
                if (pcurve.IsNull() || surface.IsNull()) {
                    deviations.push_back(std::numeric_limits<double>::infinity());
                    continue;
                }
                const auto first = std::max(std::min(curveFirst, curveLast), std::min(pcurveFirst, pcurveLast));
                const auto last = std::min(std::max(curveFirst, curveLast), std::max(pcurveFirst, pcurveLast));
                for (int sample = 0; sample < 97 && last >= first; ++sample) {
                    const auto parameter = first + (last - first) * static_cast<double>(sample) / 96.0;
                    const auto uv = pcurve->Value(parameter);
                    const auto deviation = curve->Value(parameter).Distance(surface->Value(uv.X(), uv.Y()));
                    deviations.push_back(deviation);
                    if (deviation > report.curveOnSurfaceMaximumDeviation) {
                        report.curveOnSurfaceMaximumDeviation = deviation;
                        report.worstParameter = parameter;
                    }
                }
            }
        }
        report.curveOnSurfaceP95Deviation = percentile95(deviations);
        if (!deviations.empty()) {
            auto squared = 0.0;
            for (const auto deviation : deviations) squared += deviation * deviation;
            report.curveOnSurfaceRmsDeviation = std::sqrt(squared / static_cast<double>(deviations.size()));
        }

        if (group.faces.size() == 2) {
            double firstParameters[2] {};
            double lastParameters[2] {};
            const auto firstPcurve = BRep_Tool::CurveOnSurface(
                group.edge, group.faces[0], firstParameters[0], lastParameters[0]);
            const auto secondPcurve = BRep_Tool::CurveOnSurface(
                group.edge, group.faces[1], firstParameters[1], lastParameters[1]);
            const auto firstSurface = BRep_Tool::Surface(group.faces[0]);
            const auto secondSurface = BRep_Tool::Surface(group.faces[1]);
            if (!firstPcurve.IsNull() && !secondPcurve.IsNull() &&
                !firstSurface.IsNull() && !secondSurface.IsNull()) {
                const auto first = std::max(
                    std::min(firstParameters[0], lastParameters[0]),
                    std::min(firstParameters[1], lastParameters[1]));
                const auto last = std::min(
                    std::max(firstParameters[0], lastParameters[0]),
                    std::max(firstParameters[1], lastParameters[1]));
                for (int sample = 0; sample < 97 && last >= first; ++sample) {
                    const auto parameter = first + (last - first) * static_cast<double>(sample) / 96.0;
                    const auto firstUv = firstPcurve->Value(parameter);
                    const auto secondUv = secondPcurve->Value(parameter);
                    gp_Pnt firstPoint;
                    gp_Pnt secondPoint;
                    gp_Vec firstDu;
                    gp_Vec firstDv;
                    gp_Vec secondDu;
                    gp_Vec secondDv;
                    firstSurface->D1(firstUv.X(), firstUv.Y(), firstPoint, firstDu, firstDv);
                    secondSurface->D1(secondUv.X(), secondUv.Y(), secondPoint, secondDu, secondDv);
                    report.adjacentSurfaceMaximumSeparation = std::max(
                        report.adjacentSurfaceMaximumSeparation, firstPoint.Distance(secondPoint));
                    const auto firstNormal = firstDu.Crossed(firstDv);
                    const auto secondNormal = secondDu.Crossed(secondDv);
                    if (firstNormal.SquareMagnitude() > 1.0e-24 && secondNormal.SquareMagnitude() > 1.0e-24) {
                        const auto dot = std::clamp(
                            std::abs(gp_Dir(firstNormal).Dot(gp_Dir(secondNormal))), 0.0, 1.0);
                        report.adjacentSurfaceMaximumNormalDegrees = std::max(
                            report.adjacentSurfaceMaximumNormalDegrees,
                            std::acos(dot) * 180.0 / 3.14159265358979323846);
                    }
                }
            }
        }
        report.triangulationDeflections = {0.03, 0.01, 0.003};
        report.triangulationPolylineSeparations.assign(3, 0.0);
    }

    for (std::size_t deflectionIndex = 0; deflectionIndex < 3; ++deflectionIndex) {
        BRepTools::Clean(replacementShape);
        BRepMesh_IncrementalMesh mesh(
            replacementShape,
            reports.front().triangulationDeflections[deflectionIndex],
            Standard_False,
            0.5,
            Standard_False);
        for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            if (groups[groupIndex].faces.size() != 2) continue;
            reports[groupIndex].triangulationPolylineSeparations[deflectionIndex] = polylineSeparation(
                edgePolyline(groups[groupIndex].edge, groups[groupIndex].faces[0]),
                edgePolyline(groups[groupIndex].edge, groups[groupIndex].faces[1]));
        }
    }
    for (auto& report : reports) {
        report.classification = classifyRegionEdgeConsistency(report, tolerance);
    }
    return reports;
}

}

bool NativeRegionPatchNetworkBuildResult::success() const {
    return status == RegionRefitStatus::Success && !replacementShape.IsNull() &&
        !replacementFaces.empty();
}

bool NativeRegionPatchNetworkBuildResult::constructible() const {
    return !replacementShape.IsNull() && !replacementFaces.empty();
}

NativeRegionPatchNetworkBuildResult NativeRegionPatchNetworkBuilder::build(
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const RegionFittedPatchNetwork& network,
    const double curveOnSurfaceTolerance,
    const bool allowUnvalidatedFaces) const {
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
        std::unordered_map<std::size_t, std::size_t> localByGlobal;
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            localByGlobal.emplace(parameterization.vertexIndices[local], local);
        }
        std::size_t offset = 0;
        while (offset < loopEdges.size()) {
            const auto index = (start + offset) % loopEdges.size();
            auto runLength = std::size_t {1};
            while (offset + runLength < loopEdges.size() &&
                loopEdges[(start + offset + runLength) % loopEdges.size()].IsSame(loopEdges[index])) {
                ++runLength;
            }
            auto edge = loopEdges[index];
            std::vector<std::pair<gp_Pnt, gp_Pnt2d>> pcurvePoints;
            pcurvePoints.reserve(runLength + 1);
            for (std::size_t run = 0; run <= runLength; ++run) {
                const auto vertex = loop[(start + offset + run) % loop.size()];
                const auto local = localByGlobal.at(vertex);
                const auto uv = parameterization.parameters[local];
                pcurvePoints.emplace_back(
                    mesh.vertices[vertex].point,
                    gp_Pnt2d(uv[0], uv[1]));
            }
            double pcurveFirst = 0.0;
            double pcurveLast = 0.0;
            const auto pcurve = parameterPcurve(
                edge, pcurvePoints, pcurveFirst, pcurveLast, curveOnSurfaceTolerance);
            if (!attachPcurve(
                    edge,
                    network.surfaces[patch],
                    pcurve,
                    pcurveFirst,
                    pcurveLast,
                    curveOnSurfaceTolerance)) {
                result.message = "Could not construct a parameter-aligned pcurve for a shared B-rep edge.";
                return result;
            }
            ++result.pcurveProjectionCount;
            edge = orientedForPair(
                edge,
                mesh.vertices[loop[index]].point,
                mesh.vertices[loop[(start + offset + runLength) % loop.size()]].point);
            wireBuilder.Add(edge);
            distinctEdges.push_back(edge);
            offset += runLength;
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
                std::clamp(uv[0], 0.0, 1.0),
                std::clamp(uv[1], 0.0, 1.0),
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
        const auto faceValid = BRepCheck_Analyzer(face).IsValid();
        if (!faceValid) ++result.brepValidationFailureCount;
        if ((result.sameParameterFailureCount > 0 || !faceValid) && !allowUnvalidatedFaces) {
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
    result.edgeConsistency = measureEdgeConsistency(
        result.replacementShape,
        result.replacementFaces,
        result.internalSeamEdges,
        curveOnSurfaceTolerance);
    const auto validated = result.sameParameterFailureCount == 0 &&
        result.brepValidationFailureCount == 0;
    result.status = validated ? RegionRefitStatus::Success : RegionRefitStatus::Failed;
    result.message = validated
        ? "Built a native trimmed multi-surface patch network with shared B-rep edges."
        : "Built a diagnostic patch network that failed SameParameter or BRep validation.";
    return result;
}

}
