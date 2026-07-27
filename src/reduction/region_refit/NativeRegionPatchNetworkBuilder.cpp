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
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeAnalysis_Edge.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TColgp_HArray1OfPnt2d.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_HArray1OfReal.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <unordered_map>
#include <vector>

namespace spo {

namespace {

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

Handle(Geom2d_Curve) diagnosticOrderedPcurve(
    const TopoDS_Edge& edge,
    const std::vector<std::pair<gp_Pnt, gp_Pnt2d>>& points,
    double& first,
    double& last,
    const double /*tolerance*/) {
    const auto curve = BRep_Tool::Curve(edge, first, last);
    if (curve.IsNull() || points.size() < 2 || std::abs(last - first) <= 1.0e-14) {
        return {};
    }
    const auto rangeFirst = std::min(first, last);
    const auto rangeLast = std::max(first, last);
    const auto forward =
        curve->Value(rangeFirst).SquareDistance(points.front().first) +
        curve->Value(rangeLast).SquareDistance(points.back().first);
    const auto reversed =
        curve->Value(rangeFirst).SquareDistance(points.back().first) +
        curve->Value(rangeLast).SquareDistance(points.front().first);
    const auto reversePoints = reversed < forward;
    TColgp_Array1OfPnt2d poles(1, static_cast<int>(points.size()));
    TColStd_Array1OfReal knots(1, static_cast<int>(points.size()));
    TColStd_Array1OfInteger multiplicities(1, static_cast<int>(points.size()));
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto pointIndex = reversePoints ? points.size() - 1 - index : index;
        const auto parameter = rangeFirst +
            (rangeLast - rangeFirst) * static_cast<double>(index) /
                static_cast<double>(points.size() - 1);
        const auto arrayIndex = static_cast<int>(index + 1);
        poles.SetValue(arrayIndex, points[pointIndex].second);
        knots.SetValue(arrayIndex, parameter);
        multiplicities.SetValue(
            arrayIndex,
            index == 0 || index + 1 == points.size() ? 2 : 1);
    }
    try {
        first = rangeFirst;
        last = rangeLast;
        return new Geom2d_BSplineCurve(
            poles,
            knots,
            multiplicities,
            1,
            Standard_False);
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
    const RegionCurveNetwork& curveNetwork,
    const double curveOnSurfaceTolerance,
    const bool allowUnvalidatedFaces) const {
    NativeRegionPatchNetworkBuildResult result;
    if (mesh.empty() || network.surfaces.size() != network.layout.patches.size() ||
        network.layout.patchCount != static_cast<int>(network.surfaces.size()) ||
        !curveNetwork.valid() ||
        curveNetwork.hash != network.layout.curveNetworkHash ||
        curveNetwork.patchCount != network.layout.patchCount ||
        curveOnSurfaceTolerance <= 0.0) {
        result.status = RegionRefitStatus::InvalidRequest;
        result.message = "Native patch-network construction received inconsistent geometry.";
        return result;
    }

    std::vector<TopoDS_Vertex> vertices(curveNetwork.nodes.size());
    for (const auto& node : curveNetwork.nodes) {
        if (node.id >= vertices.size()) {
            result.message = "A curve-network node id is out of range.";
            return result;
        }
        if (node.originalBoundary) {
            for (const auto& curveEdge : curveNetwork.edges) {
                if (!curveEdge.originalBoundary ||
                    curveEdge.originalEdge.IsNull() ||
                    (curveEdge.startNode != node.id &&
                        curveEdge.endNode != node.id)) {
                    continue;
                }
                TopoDS_Vertex first;
                TopoDS_Vertex last;
                TopExp::Vertices(
                    curveEdge.originalEdge, first, last, Standard_True);
                if (!first.IsNull() &&
                    BRep_Tool::Pnt(first).Distance(node.point) <=
                        curveOnSurfaceTolerance) {
                    vertices[node.id] = first;
                    break;
                }
                if (!last.IsNull() &&
                    BRep_Tool::Pnt(last).Distance(node.point) <=
                        curveOnSurfaceTolerance) {
                    vertices[node.id] = last;
                    break;
                }
            }
        }
        if (vertices[node.id].IsNull()) {
            vertices[node.id] = BRepBuilderAPI_MakeVertex(node.point).Vertex();
            BRep_Builder vertexBuilder;
            vertexBuilder.UpdateVertex(
                vertices[node.id], curveOnSurfaceTolerance);
        }
    }

    std::vector<TopoDS_Edge> edges(curveNetwork.edges.size());
    for (const auto& curveEdge : curveNetwork.edges) {
        if (curveEdge.id >= edges.size() ||
            curveEdge.startNode >= vertices.size() ||
            curveEdge.endNode >= vertices.size() ||
            curveEdge.curve.IsNull()) {
            result.message = "A frozen curve-network edge is incomplete.";
            return result;
        }
        if (curveEdge.originalBoundary && curveEdge.reusesOriginalEdge &&
            !curveEdge.originalEdge.IsNull()) {
            edges[curveEdge.id] = curveEdge.originalEdge;
            ++result.reusedOriginalBoundaryEdgeCount;
        } else {
            BRepBuilderAPI_MakeEdge edgeBuilder(
                curveEdge.curve,
                vertices[curveEdge.startNode],
                vertices[curveEdge.endNode],
                curveEdge.firstParameter,
                curveEdge.lastParameter);
            if (!edgeBuilder.IsDone()) {
                result.message =
                    "Could not construct one frozen curve-network edge.";
                return result;
            }
            edges[curveEdge.id] = edgeBuilder.Edge();
            if (curveEdge.originalBoundary) {
                result.splitBoundarySegments.push_back({
                    curveEdge.originalEdgeId,
                    curveEdge.firstParameter,
                    curveEdge.lastParameter,
                    edges[curveEdge.id],
                    curveEdge.firstPatch});
                ++result.splitBoundarySegmentCount;
            }
        }
        if (!curveEdge.originalBoundary) {
            result.internalSeamEdges.push_back(edges[curveEdge.id]);
            ++result.internalSeamCount;
        }
    }

    struct PatchEdges {
        std::vector<TopoDS_Edge> oriented;
        std::vector<std::size_t> indices;
    };
    std::vector<PatchEdges> patchEdges(
        static_cast<std::size_t>(curveNetwork.patchCount));
    for (const auto& loop : curveNetwork.patchLoops) {
        if (loop.patch < 0 || loop.patch >= curveNetwork.patchCount ||
            loop.edges.size() < 2) {
            result.message = "A curve-network patch loop is invalid.";
            return result;
        }
        const auto patch = static_cast<std::size_t>(loop.patch);
        const auto& parameterization = network.layout.patches[patch];
        std::unordered_map<std::size_t, std::size_t> localByGlobal;
        for (std::size_t local = 0;
             local < parameterization.vertexIndices.size(); ++local) {
            localByGlobal.emplace(
                parameterization.vertexIndices[local], local);
        }
        for (const auto& use : loop.edges) {
            if (use.edgeIndex >= curveNetwork.edges.size()) {
                result.message = "A patch loop references a missing curve edge.";
                return result;
            }
            const auto& curveEdge = curveNetwork.edges[use.edgeIndex];
            auto edge = edges[use.edgeIndex];
            std::vector<std::pair<gp_Pnt, gp_Pnt2d>> pcurvePoints;
            pcurvePoints.reserve(curveEdge.meshVertices.size());
            for (std::size_t sample = 0;
                 sample < curveEdge.meshVertices.size(); ++sample) {
                const auto found = localByGlobal.find(
                    curveEdge.meshVertices[sample]);
                if (found == localByGlobal.end()) {
                    result.message =
                        "A curve edge sample is absent from its patch parameterization.";
                    return result;
                }
                const auto& uv = parameterization.parameters[found->second];
                pcurvePoints.emplace_back(
                    curveEdge.curve->Value(
                        curveEdge.meshParameters[sample]),
                    gp_Pnt2d(uv[0], uv[1]));
            }
            double pcurveFirst = 0.0;
            double pcurveLast = 0.0;
            auto pcurve = parameterPcurve(
                edge,
                pcurvePoints,
                pcurveFirst,
                pcurveLast,
                curveOnSurfaceTolerance);
            if (pcurve.IsNull() && allowUnvalidatedFaces) {
                pcurve = diagnosticOrderedPcurve(
                    edge,
                    pcurvePoints,
                    pcurveFirst,
                    pcurveLast,
                    curveOnSurfaceTolerance);
                if (!pcurve.IsNull()) {
                    ++result.diagnosticPcurveFallbackCount;
                }
            }
            if (!attachPcurve(
                    edge,
                    network.surfaces[patch],
                    pcurve,
                    pcurveFirst,
                    pcurveLast,
                    curveOnSurfaceTolerance)) {
                result.message =
                    "Could not attach a parameter-aligned pcurve to both sides of the frozen edge network.";
                return result;
            }
            ++result.pcurveProjectionCount;
            const auto firstNode = use.forward
                ? curveEdge.startNode : curveEdge.endNode;
            const auto secondNode = use.forward
                ? curveEdge.endNode : curveEdge.startNode;
            patchEdges[patch].oriented.push_back(orientedForPair(
                edge,
                curveNetwork.nodes[firstNode].point,
                curveNetwork.nodes[secondNode].point));
            patchEdges[patch].indices.push_back(use.edgeIndex);
        }
    }

    for (auto& edge : edges) {
        BRepLib::SameRange(edge, curveOnSurfaceTolerance);
        BRepLib::SameParameter(edge, curveOnSurfaceTolerance);
    }

    result.replacementFaces.reserve(network.layout.patches.size());
    for (std::size_t patch = 0;
         patch < network.layout.patches.size(); ++patch) {
        BRepBuilderAPI_MakeWire wireBuilder;
        for (const auto& edge : patchEdges[patch].oriented) {
            wireBuilder.Add(edge);
        }
        if (!wireBuilder.IsDone()) {
            result.message =
                "Could not assemble a closed fitted patch wire from the frozen curve network.";
            return result;
        }
        BRep_Builder faceBuilder;
        TopoDS_Face face;
        faceBuilder.MakeFace(
            face,
            network.surfaces[patch],
            TopLoc_Location {},
            curveOnSurfaceTolerance);
        faceBuilder.Add(face, wireBuilder.Wire());
        const auto& parameterization = network.layout.patches[patch];
        if (!parameterization.vertexIndices.empty()) {
            const auto uv = parameterization.parameters.front();
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
                gp_Dir(du).Dot(
                    mesh.vertices[
                        parameterization.vertexIndices.front()].normal) < 0.0) {
                face.Reverse();
            }
        }
        ShapeAnalysis_Edge edgeAnalyzer;
        BRep_Builder topologyBuilder;
        for (const auto edgeIndex : patchEdges[patch].indices) {
            Standard_Real deviation = 0.0;
            ++result.sameParameterCheckCount;
            edgeAnalyzer.CheckSameParameter(
                edges[edgeIndex], face, deviation, 97);
            result.maximumSameParameterDeviation = std::max(
                result.maximumSameParameterDeviation,
                std::isfinite(deviation)
                    ? static_cast<double>(deviation)
                    : std::numeric_limits<double>::infinity());
            if (!std::isfinite(deviation) ||
                deviation > curveOnSurfaceTolerance) {
                ++result.sameParameterFailureCount;
            } else {
                topologyBuilder.SameParameter(
                    edges[edgeIndex], Standard_True);
            }
        }
        const auto faceValid = BRepCheck_Analyzer(face).IsValid();
        if (!faceValid) ++result.brepValidationFailureCount;
        if ((result.sameParameterFailureCount > 0 || !faceValid) &&
            !allowUnvalidatedFaces) {
            result.message =
                "A trimmed fitted patch failed dense SameParameter or BRep validation.";
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
