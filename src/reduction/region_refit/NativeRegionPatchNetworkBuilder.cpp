#include "reduction/region_refit/NativeRegionPatchNetworkBuilder.h"

#include <BRepAlgoAPI_Section.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepCheck_Status.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
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
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>
#include <gp_Vec2d.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace spo {

namespace {

const char* brepCheckStatusName(const BRepCheck_Status status) {
    switch (status) {
    case BRepCheck_NoError: return "NoError";
    case BRepCheck_InvalidPointOnCurve: return "InvalidPointOnCurve";
    case BRepCheck_InvalidPointOnCurveOnSurface:
        return "InvalidPointOnCurveOnSurface";
    case BRepCheck_InvalidPointOnSurface: return "InvalidPointOnSurface";
    case BRepCheck_No3DCurve: return "No3DCurve";
    case BRepCheck_Multiple3DCurve: return "Multiple3DCurve";
    case BRepCheck_Invalid3DCurve: return "Invalid3DCurve";
    case BRepCheck_NoCurveOnSurface: return "NoCurveOnSurface";
    case BRepCheck_InvalidCurveOnSurface: return "InvalidCurveOnSurface";
    case BRepCheck_InvalidCurveOnClosedSurface:
        return "InvalidCurveOnClosedSurface";
    case BRepCheck_InvalidSameRangeFlag: return "InvalidSameRangeFlag";
    case BRepCheck_InvalidSameParameterFlag:
        return "InvalidSameParameterFlag";
    case BRepCheck_InvalidDegeneratedFlag: return "InvalidDegeneratedFlag";
    case BRepCheck_FreeEdge: return "FreeEdge";
    case BRepCheck_InvalidMultiConnexity: return "InvalidMultiConnexity";
    case BRepCheck_InvalidRange: return "InvalidRange";
    case BRepCheck_EmptyWire: return "EmptyWire";
    case BRepCheck_RedundantEdge: return "RedundantEdge";
    case BRepCheck_SelfIntersectingWire: return "SelfIntersectingWire";
    case BRepCheck_NoSurface: return "NoSurface";
    case BRepCheck_InvalidWire: return "InvalidWire";
    case BRepCheck_RedundantWire: return "RedundantWire";
    case BRepCheck_IntersectingWires: return "IntersectingWires";
    case BRepCheck_InvalidImbricationOfWires:
        return "InvalidImbricationOfWires";
    case BRepCheck_EmptyShell: return "EmptyShell";
    case BRepCheck_RedundantFace: return "RedundantFace";
    case BRepCheck_InvalidImbricationOfShells:
        return "InvalidImbricationOfShells";
    case BRepCheck_UnorientableShape: return "UnorientableShape";
    case BRepCheck_NotClosed: return "NotClosed";
    case BRepCheck_NotConnected: return "NotConnected";
    case BRepCheck_SubshapeNotInShape: return "SubshapeNotInShape";
    case BRepCheck_BadOrientation: return "BadOrientation";
    case BRepCheck_BadOrientationOfSubshape:
        return "BadOrientationOfSubshape";
    case BRepCheck_InvalidPolygonOnTriangulation:
        return "InvalidPolygonOnTriangulation";
    case BRepCheck_InvalidToleranceValue: return "InvalidToleranceValue";
    case BRepCheck_EnclosedRegion: return "EnclosedRegion";
    case BRepCheck_CheckFail: return "CheckFail";
    }
    return "Unknown";
}

void recordBRepCheckStatuses(
    std::vector<RegionBRepCheckStatusReport>& reports,
    const BRepCheck_Analyzer& analyzer,
    const TopoDS_Shape& shape,
    std::string shapeKind,
    const int faceIndex,
    const int subshapeIndex) {
    const auto valid = analyzer.IsValid(shape) == Standard_True;
    const auto result = analyzer.Result(shape);
    if (result.IsNull()) {
        reports.push_back({
            std::move(shapeKind),
            faceIndex,
            subshapeIndex,
            static_cast<int>(BRepCheck_CheckFail),
            brepCheckStatusName(BRepCheck_CheckFail),
            false});
        return;
    }
    const auto& statuses = result->Status();
    if (statuses.IsEmpty()) {
        reports.push_back({
            std::move(shapeKind),
            faceIndex,
            subshapeIndex,
            static_cast<int>(BRepCheck_CheckFail),
            brepCheckStatusName(BRepCheck_CheckFail),
            false});
        return;
    }
    for (BRepCheck_ListIteratorOfListOfStatus iterator(statuses);
         iterator.More();
         iterator.Next()) {
        const auto status = iterator.Value();
        reports.push_back({
            shapeKind,
            faceIndex,
            subshapeIndex,
            static_cast<int>(status),
            brepCheckStatusName(status),
            valid});
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

Handle(Geom2d_Curve) storedPcurve(
    const RegionPatchEdgePcurve& source,
    double& first,
    double& last) {
    if (source.parameters.size() < 2 ||
        source.parameters.size() != source.uvPoints.size() ||
        source.lastParameter <= source.firstParameter ||
        !std::is_sorted(
            source.parameters.begin(),
            source.parameters.end()) ||
        std::adjacent_find(
            source.parameters.begin(),
            source.parameters.end(),
            std::greater_equal<double> {}) !=
            source.parameters.end()) {
        return {};
    }
    TColgp_Array1OfPnt2d poles(
        1, static_cast<int>(source.uvPoints.size()));
    TColStd_Array1OfReal knots(
        1, static_cast<int>(source.parameters.size()));
    TColStd_Array1OfInteger multiplicities(
        1, static_cast<int>(source.parameters.size()));
    for (std::size_t index = 0; index < source.parameters.size(); ++index) {
        const auto arrayIndex = static_cast<int>(index + 1);
        poles.SetValue(
            arrayIndex,
            gp_Pnt2d(
                source.uvPoints[index][0],
                source.uvPoints[index][1]));
        knots.SetValue(arrayIndex, source.parameters[index]);
        multiplicities.SetValue(
            arrayIndex,
            index == 0 || index + 1 == source.parameters.size()
                ? 2 : 1);
    }
    try {
        first = source.firstParameter;
        last = source.lastParameter;
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

bool isValidStoredPcurve(
    const RegionPatchEdgePcurve& source) {
    if (source.parameters.size() < 2 ||
        source.parameters.size() !=
            source.uvPoints.size() ||
        !(source.lastParameter >
            source.firstParameter) ||
        std::any_of(
            source.uvPoints.begin(),
            source.uvPoints.end(),
            [](const auto& uv) {
                return !std::isfinite(uv[0]) ||
                    !std::isfinite(uv[1]);
            })) {
        return false;
    }
    return std::equal(
        source.parameters.begin() + 1,
        source.parameters.end(),
        source.parameters.begin(),
        [](const auto current, const auto previous) {
            return current > previous;
        });
}

double sectionCurveLength(const TopoDS_Edge& edge) {
    double first = 0.0;
    double last = 0.0;
    const auto curve = BRep_Tool::Curve(edge, first, last);
    if (curve.IsNull() || last <= first) return 0.0;
    auto previous = curve->Value(first);
    auto length = 0.0;
    for (int sample = 1; sample < 65; ++sample) {
        const auto point = curve->Value(
            first + (last - first) * sample / 64.0);
        length += previous.Distance(point);
        previous = point;
    }
    return length;
}

bool sectionOnExpectedEdge(
    const TopoDS_Edge& sectionEdge,
    const std::vector<const RegionCurveNetworkEdge*>& expected,
    const double tolerance) {
    double first = 0.0;
    double last = 0.0;
    const auto curve = BRep_Tool::Curve(sectionEdge, first, last);
    if (curve.IsNull() || last <= first) return false;
    for (const auto* expectedEdge : expected) {
        if (expectedEdge == nullptr || expectedEdge->curve.IsNull()) continue;
        auto matches = true;
        for (int sample = 0; sample < 17; ++sample) {
            const auto parameter =
                first + (last - first) * sample / 16.0;
            try {
                GeomAPI_ProjectPointOnCurve projection(
                    curve->Value(parameter),
                    expectedEdge->curve,
                    expectedEdge->firstParameter,
                    expectedEdge->lastParameter);
                if (projection.NbPoints() == 0 ||
                    projection.LowerDistance() > tolerance) {
                    matches = false;
                    break;
                }
            } catch (const Standard_Failure&) {
                matches = false;
                break;
            }
        }
        if (matches) return true;
    }
    return false;
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
        report.parameterSemantic = group.internal
            ? "SharedEdgeParameter"
            : "EdgeParameter";

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
                for (int sample = 0; sample < 257 && last >= first; ++sample) {
                    const auto parameter = first + (last - first) * static_cast<double>(sample) / 256.0;
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
            if (!curve.IsNull() &&
                !firstPcurve.IsNull() && !secondPcurve.IsNull() &&
                !firstSurface.IsNull() && !secondSurface.IsNull()) {
                const auto first = std::max({
                    std::min(curveFirst, curveLast),
                    std::min(firstParameters[0], lastParameters[0]),
                    std::min(firstParameters[1], lastParameters[1])});
                const auto last = std::min({
                    std::max(curveFirst, curveLast),
                    std::max(firstParameters[0], lastParameters[0]),
                    std::max(firstParameters[1], lastParameters[1])});
                report.commonFirstParameter = first;
                report.commonLastParameter = last;
                report.minimumFirstDerivativeScaleRatio =
                    std::numeric_limits<double>::infinity();
                report.minimumSecondDerivativeScaleRatio =
                    std::numeric_limits<double>::infinity();
                report.minimumAdjacentDerivativeScaleRatio =
                    std::numeric_limits<double>::infinity();
                for (int sample = 0; sample < 257 && last >= first; ++sample) {
                    const auto parameter = first + (last - first) * static_cast<double>(sample) / 256.0;
                    gp_Pnt curvePoint;
                    gp_Vec curveDerivative;
                    curve->D1(parameter, curvePoint, curveDerivative);
                    gp_Pnt2d firstUv;
                    gp_Pnt2d secondUv;
                    gp_Vec2d firstPcurveDerivative;
                    gp_Vec2d secondPcurveDerivative;
                    firstPcurve->D1(
                        parameter, firstUv, firstPcurveDerivative);
                    secondPcurve->D1(
                        parameter, secondUv, secondPcurveDerivative);
                    gp_Pnt firstPoint;
                    gp_Pnt secondPoint;
                    gp_Vec firstDu;
                    gp_Vec firstDv;
                    gp_Vec secondDu;
                    gp_Vec secondDv;
                    firstSurface->D1(firstUv.X(), firstUv.Y(), firstPoint, firstDu, firstDv);
                    secondSurface->D1(secondUv.X(), secondUv.Y(), secondPoint, secondDu, secondDv);
                    ++report.commonParameterSampleCount;
                    const auto firstDeviation =
                        curvePoint.Distance(firstPoint);
                    const auto secondDeviation =
                        curvePoint.Distance(secondPoint);
                    const auto separation =
                        firstPoint.Distance(secondPoint);
                    if (std::max(firstDeviation, secondDeviation) >
                        report.curveOnSurfaceMaximumDeviation) {
                        report.curveOnSurfaceMaximumDeviation =
                            std::max(firstDeviation, secondDeviation);
                        report.worstParameter = parameter;
                    }
                    if (separation >
                        report.adjacentSurfaceMaximumSeparation) {
                        report.adjacentSurfaceMaximumSeparation =
                            separation;
                        report.worstSeparationParameter = parameter;
                    }
                    const auto firstNormal = firstDu.Crossed(firstDv);
                    const auto secondNormal = secondDu.Crossed(secondDv);
                    if (firstNormal.SquareMagnitude() > 1.0e-24 && secondNormal.SquareMagnitude() > 1.0e-24) {
                        const auto dot = std::clamp(
                            std::abs(gp_Dir(firstNormal).Dot(gp_Dir(secondNormal))), 0.0, 1.0);
                        const auto normalDegrees =
                            std::acos(dot) * 180.0 /
                            3.14159265358979323846;
                        if (normalDegrees >
                            report.adjacentSurfaceMaximumNormalDegrees) {
                            report.adjacentSurfaceMaximumNormalDegrees =
                                normalDegrees;
                            report.worstNormalParameter = parameter;
                        }
                    }
                    const auto firstTangent =
                        firstDu.Multiplied(firstPcurveDerivative.X()) +
                        firstDv.Multiplied(firstPcurveDerivative.Y());
                    const auto secondTangent =
                        secondDu.Multiplied(secondPcurveDerivative.X()) +
                        secondDv.Multiplied(secondPcurveDerivative.Y());
                    const auto curveMagnitude = curveDerivative.Magnitude();
                    const auto firstMagnitude = firstTangent.Magnitude();
                    const auto secondMagnitude = secondTangent.Magnitude();
                    if (curveMagnitude > 1.0e-12 &&
                        firstMagnitude > 1.0e-12 &&
                        secondMagnitude > 1.0e-12) {
                        const auto firstScale =
                            firstMagnitude / curveMagnitude;
                        const auto secondScale =
                            secondMagnitude / curveMagnitude;
                        const auto adjacentScale = std::max(
                            firstMagnitude / secondMagnitude,
                            secondMagnitude / firstMagnitude);
                        report.minimumFirstDerivativeScaleRatio =
                            std::min(
                                report.minimumFirstDerivativeScaleRatio,
                                firstScale);
                        report.maximumFirstDerivativeScaleRatio =
                            std::max(
                                report.maximumFirstDerivativeScaleRatio,
                                firstScale);
                        report.minimumSecondDerivativeScaleRatio =
                            std::min(
                                report.minimumSecondDerivativeScaleRatio,
                                secondScale);
                        report.maximumSecondDerivativeScaleRatio =
                            std::max(
                                report.maximumSecondDerivativeScaleRatio,
                                secondScale);
                        report.minimumAdjacentDerivativeScaleRatio =
                            std::min(
                                report.minimumAdjacentDerivativeScaleRatio,
                                adjacentScale);
                        report.maximumAdjacentDerivativeScaleRatio =
                            std::max(
                                report.maximumAdjacentDerivativeScaleRatio,
                                adjacentScale);
                    }
                }
                if (report.commonParameterSampleCount == 0) {
                    report.minimumFirstDerivativeScaleRatio = 0.0;
                    report.minimumSecondDerivativeScaleRatio = 0.0;
                    report.minimumAdjacentDerivativeScaleRatio = 0.0;
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
        !network.boundaryLocked ||
        !network.constructibleForBRep ||
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
    std::vector<int> edgeUseCounts(edges.size(), 0);
    for (const auto& loop : curveNetwork.patchLoops) {
        if (loop.patch < 0 || loop.patch >= curveNetwork.patchCount ||
            loop.edges.size() < 2) {
            result.message = "A curve-network patch loop is invalid.";
            return result;
        }
        const auto patch = static_cast<std::size_t>(loop.patch);
        const auto& parameterization = network.layout.patches[patch];
        if (!parameterization.pcurveLoopClosed ||
            !parameterization.pcurveOrientationConsistent ||
            parameterization.pcurveSelfIntersectionCount != 0 ||
            parameterization.pcurveNonAdjacentIntersectionCount != 0 ||
            parameterization.edgePcurves.size() != loop.edges.size()) {
            result.message =
                "A patch has no simple closed frozen pcurve loop.";
            return result;
        }
        for (const auto& use : loop.edges) {
            if (use.edgeIndex >= curveNetwork.edges.size()) {
                result.message = "A patch loop references a missing curve edge.";
                return result;
            }
            const auto& curveEdge = curveNetwork.edges[use.edgeIndex];
            auto edge = edges[use.edgeIndex];
            const auto stored = std::find_if(
                parameterization.edgePcurves.begin(),
                parameterization.edgePcurves.end(),
                [&](const auto& pcurve) {
                    return pcurve.edgeIndex == use.edgeIndex &&
                        pcurve.forward == use.forward;
                });
            if (stored == parameterization.edgePcurves.end()) {
                result.message =
                    "A patch loop has no matching frozen parameter-domain pcurve.";
                return result;
            }
            if (!isValidStoredPcurve(*stored)) {
                result.message =
                    "A patch loop has no finite, strictly parameterized stored pcurve.";
                return result;
            }
            double pcurveFirst = 0.0;
            double pcurveLast = 0.0;
            const auto pcurve = storedPcurve(
                *stored, pcurveFirst, pcurveLast);
            if (!attachPcurve(
                    edge,
                    network.surfaces[patch],
                    pcurve,
                    pcurveFirst,
                    pcurveLast,
                    curveOnSurfaceTolerance)) {
                result.message =
                    "Could not attach the frozen parameter-domain pcurve to the shared edge.";
                return result;
            }
            ++result.storedPcurveAttachmentCount;
            ++edgeUseCounts[use.edgeIndex];
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
    std::vector<bool> denseSameParameterPassed(edges.size(), true);
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
            const auto& parameters =
                parameterization.surfaceParameters.size() ==
                    parameterization.vertexIndices.size()
                ? parameterization.surfaceParameters
                : parameterization.parameters;
            const auto uv = parameters.front();
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
        for (const auto edgeIndex : patchEdges[patch].indices) {
            Standard_Real deviation = 0.0;
            ++result.sameParameterCheckCount;
            edgeAnalyzer.CheckSameParameter(
                edges[edgeIndex], face, deviation, 257);
            result.maximumSameParameterDeviation = std::max(
                result.maximumSameParameterDeviation,
                std::isfinite(deviation)
                    ? static_cast<double>(deviation)
                    : std::numeric_limits<double>::infinity());
            if (!std::isfinite(deviation) ||
                deviation > curveOnSurfaceTolerance) {
                ++result.sameParameterFailureCount;
                denseSameParameterPassed[edgeIndex] = false;
            }
        }
        result.replacementFaces.push_back(face);
    }
    BRep_Builder topologyBuilder;
    for (std::size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
        const auto expectedUseCount =
            curveNetwork.edges[edgeIndex].originalBoundary ? 1 : 2;
        const auto passed = denseSameParameterPassed[edgeIndex] &&
            edgeUseCounts[edgeIndex] == expectedUseCount;
        topologyBuilder.SameParameter(
            edges[edgeIndex], passed ? Standard_True : Standard_False);
        if (!passed && denseSameParameterPassed[edgeIndex]) {
            ++result.sameParameterFailureCount;
        }
    }
    if (static_cast<int>(result.replacementFaces.size()) != network.layout.patchCount) {
        result.message = "The native replacement B-rep face count does not match the selected layout.";
        return result;
    }
    for (std::size_t firstPatch = 0;
         firstPatch < result.replacementFaces.size(); ++firstPatch) {
        for (std::size_t secondPatch = firstPatch + 1;
             secondPatch < result.replacementFaces.size(); ++secondPatch) {
            std::vector<const RegionCurveNetworkEdge*> expected;
            for (const auto& edge : curveNetwork.edges) {
                if (edge.originalBoundary) continue;
                const auto first = std::min(edge.firstPatch, edge.secondPatch);
                const auto second = std::max(edge.firstPatch, edge.secondPatch);
                if (first == static_cast<int>(firstPatch) &&
                    second == static_cast<int>(secondPatch)) {
                    expected.push_back(&edge);
                }
            }
            try {
                BRepAlgoAPI_Section section(
                    result.replacementFaces[firstPatch],
                    result.replacementFaces[secondPatch],
                    Standard_False);
                section.Build();
                if (!section.IsDone()) {
                    ++result.unexpectedFaceIntersectionCount;
                    result.unexpectedFaceIntersectionTotalLength =
                        std::numeric_limits<double>::infinity();
                    result.unexpectedFaceIntersectionPatches.push_back({
                        static_cast<int>(firstPatch),
                        static_cast<int>(secondPatch)});
                    continue;
                }
                for (TopExp_Explorer explorer(
                         section.Shape(), TopAbs_EDGE);
                     explorer.More(); explorer.Next()) {
                    const auto sectionEdge =
                        TopoDS::Edge(explorer.Current());
                    if (sectionOnExpectedEdge(
                            sectionEdge,
                            expected,
                            curveOnSurfaceTolerance)) {
                        continue;
                    }
                    ++result.unexpectedFaceIntersectionCount;
                    result.unexpectedFaceIntersectionTotalLength +=
                        sectionCurveLength(sectionEdge);
                    result.unexpectedFaceIntersectionPatches.push_back({
                        static_cast<int>(firstPatch),
                        static_cast<int>(secondPatch)});
                }
            } catch (const Standard_Failure&) {
                ++result.unexpectedFaceIntersectionCount;
                result.unexpectedFaceIntersectionTotalLength =
                    std::numeric_limits<double>::infinity();
                result.unexpectedFaceIntersectionPatches.push_back({
                    static_cast<int>(firstPatch),
                    static_cast<int>(secondPatch)});
            }
        }
    }
    BRep_Builder builder;
    TopoDS_Shell shell;
    builder.MakeShell(shell);
    for (const auto& face : result.replacementFaces) {
        builder.Add(shell, face);
    }
    result.replacementShape = shell;
    BRepCheck_Analyzer shellAnalyzer(shell);
    if (!shellAnalyzer.IsValid(shell)) {
        ++result.brepValidationFailureCount;
    }
    recordBRepCheckStatuses(
        result.brepCheckStatuses,
        shellAnalyzer,
        shell,
        "Shell",
        -1,
        0);
    for (std::size_t faceIndex = 0;
         faceIndex < result.replacementFaces.size();
         ++faceIndex) {
        const auto& face = result.replacementFaces[faceIndex];
        if (!shellAnalyzer.IsValid(face)) {
            ++result.brepValidationFailureCount;
        }
        recordBRepCheckStatuses(
            result.brepCheckStatuses,
            shellAnalyzer,
            face,
            "Face",
            static_cast<int>(faceIndex),
            0);
        auto wireIndex = 0;
        for (TopExp_Explorer wireExplorer(face, TopAbs_WIRE);
             wireExplorer.More();
             wireExplorer.Next(), ++wireIndex) {
            recordBRepCheckStatuses(
                result.brepCheckStatuses,
                shellAnalyzer,
                wireExplorer.Current(),
                "Wire",
                static_cast<int>(faceIndex),
                wireIndex);
        }
        auto edgeIndex = 0;
        for (TopExp_Explorer edgeExplorer(face, TopAbs_EDGE);
             edgeExplorer.More();
             edgeExplorer.Next(), ++edgeIndex) {
            recordBRepCheckStatuses(
                result.brepCheckStatuses,
                shellAnalyzer,
                edgeExplorer.Current(),
                "Edge",
                static_cast<int>(faceIndex),
                edgeIndex);
        }
    }
    if ((result.sameParameterFailureCount > 0 ||
         result.brepValidationFailureCount > 0) &&
        !allowUnvalidatedFaces) {
        result.message =
            "A trimmed fitted patch shell failed dense SameParameter or BRep validation.";
        return result;
    }
    result.edgeConsistency = measureEdgeConsistency(
        result.replacementShape,
        result.replacementFaces,
        result.internalSeamEdges,
        curveOnSurfaceTolerance);
    result.replacementQuiltInternalFreeEdgeCount =
        static_cast<int>(std::count_if(
            result.edgeConsistency.begin(),
            result.edgeConsistency.end(),
            [](const auto& report) {
                return report.internalSeam && report.freeEdge;
            }));
    const auto validated = result.sameParameterFailureCount == 0 &&
        result.brepValidationFailureCount == 0 &&
        result.unexpectedFaceIntersectionCount == 0 &&
        result.replacementQuiltInternalFreeEdgeCount == 0 &&
        std::all_of(
            result.edgeConsistency.begin(),
            result.edgeConsistency.end(),
            [](const auto& report) {
                return !report.internalSeam ||
                    (report.sharedSameEdge &&
                        report.actualFaceUseCount == 2);
            });
    result.status = validated ? RegionRefitStatus::Success : RegionRefitStatus::Failed;
    result.message = validated
        ? "Built a native trimmed multi-surface patch network with shared B-rep edges."
        : "Built a diagnostic patch network that failed SameParameter, BRep, shared-edge, or face-intersection validation.";
    return result;
}

}
