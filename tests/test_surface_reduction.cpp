#include "brep/ShapeDocument.h"
#include "command/CommandContext.h"
#include "command/ReduceSelectedFacesCommand.h"
#include "reduction/SurfaceReductionEngine.h"
#include "reduction/RestrictedBSplinePatchFitter.h"
#include "reduction/StlHybridParameterization.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Geom_BSplineSurface.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Pln.hxx>

#include <cassert>
#include <cmath>
#include <iostream>
#include <set>
#include <vector>

namespace {

spo::ShapeDocument makePlanarStrip(int faceCount) {
    BRepBuilderAPI_Sewing sewing(1.0e-7);
    const gp_Pln plane(gp::XOY());
    for (int index = 0; index < faceCount; ++index) {
        const double first = static_cast<double>(index);
        const double last = static_cast<double>(index + 1);
        sewing.Add(BRepBuilderAPI_MakeFace(plane, first, last, 0.0, 1.0).Face());
    }
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

spo::ShapeDocument makeSplitBSpline(int faceCount) {
    TColgp_Array2OfPnt poles(1, 4, 1, 4);
    for (int u = 1; u <= 4; ++u) {
        for (int v = 1; v <= 4; ++v) {
            const double x = static_cast<double>(u - 1);
            const double y = static_cast<double>(v - 1);
            poles.SetValue(u, v, gp_Pnt(x, y, 0.08 * x * y));
        }
    }
    TColStd_Array1OfReal knots(1, 2);
    knots.SetValue(1, 0.0);
    knots.SetValue(2, 1.0);
    TColStd_Array1OfInteger multiplicities(1, 2);
    multiplicities.SetValue(1, 4);
    multiplicities.SetValue(2, 4);
    const Handle(Geom_BSplineSurface) surface = new Geom_BSplineSurface(
        poles,
        knots,
        knots,
        multiplicities,
        multiplicities,
        3,
        3,
        Standard_False,
        Standard_False);
    BRepBuilderAPI_Sewing sewing(1.0e-7);
    for (int index = 0; index < faceCount; ++index) {
        const double first = static_cast<double>(index) / static_cast<double>(faceCount);
        const double last = static_cast<double>(index + 1) / static_cast<double>(faceCount);
        sewing.Add(BRepBuilderAPI_MakeFace(surface, first, last, 0.0, 1.0, 1.0e-7).Face());
    }
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

spo::ShapeDocument makePlanarRing() {
    BRepBuilderAPI_Sewing sewing(1.0e-7);
    const gp_Pln plane(gp::XOY());
    for (int x = 0; x < 3; ++x) {
        for (int y = 0; y < 3; ++y) {
            if (x == 1 && y == 1) {
                continue;
            }
            sewing.Add(BRepBuilderAPI_MakeFace(
                plane,
                static_cast<double>(x),
                static_cast<double>(x + 1),
                static_cast<double>(y),
                static_cast<double>(y + 1)).Face());
        }
    }
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

TopoDS_Face makePolygonFace(const std::vector<gp_Pnt>& points) {
    BRepBuilderAPI_MakePolygon polygon;
    for (const auto& point : points) {
        polygon.Add(point);
    }
    polygon.Close();
    return BRepBuilderAPI_MakeFace(polygon.Wire()).Face();
}

spo::ShapeDocument makePartiallyReducibleCorner() {
    BRepBuilderAPI_Sewing sewing(1.0e-7);
    sewing.Add(makePolygonFace({
        gp_Pnt(0.0, 0.0, 0.0),
        gp_Pnt(1.0, 0.0, 0.0),
        gp_Pnt(1.0, 1.0, 0.0),
        gp_Pnt(0.0, 1.0, 0.0)}));
    sewing.Add(makePolygonFace({
        gp_Pnt(1.0, 0.0, 0.0),
        gp_Pnt(2.0, 0.0, 0.0),
        gp_Pnt(2.0, 1.0, 0.0),
        gp_Pnt(1.0, 1.0, 0.0)}));
    sewing.Add(makePolygonFace({
        gp_Pnt(2.0, 0.0, 0.0),
        gp_Pnt(2.0, 0.0, 1.0),
        gp_Pnt(2.0, 1.0, 1.0),
        gp_Pnt(2.0, 1.0, 0.0)}));
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

std::vector<spo::FaceId> allFaces(const spo::ShapeDocument& document) {
    std::vector<spo::FaceId> faces;
    for (spo::FaceId face = 0; face < document.topology().faceCount(); ++face) {
        faces.push_back(face);
    }
    return faces;
}

std::set<spo::EdgeId> internalEdges(const spo::ShapeDocument& document) {
    std::set<spo::EdgeId> edges;
    for (spo::EdgeId edge = 0; edge < document.topology().edgeCount(); ++edge) {
        const auto* adjacency = document.topology().adjacencyForEdge(edge);
        if (adjacency != nullptr && adjacency->faces.size() == 2) {
            edges.insert(edge);
        }
    }
    return edges;
}

spo::StlMesh makeCurvedReferencePatch() {
    const spo::StlVec3 first {0.0, 0.0, 0.0};
    const spo::StlVec3 second {1.0, 0.0, 0.0};
    const spo::StlVec3 third {1.0, 1.0, 0.0};
    const spo::StlVec3 fourth {0.0, 1.0, 0.0};
    const spo::StlVec3 center {0.5, 0.5, 0.1};
    spo::StlMesh mesh;
    mesh.addTriangle({{}, first, second, center});
    mesh.addTriangle({{}, second, third, center});
    mesh.addTriangle({{}, third, fourth, center});
    mesh.addTriangle({{}, fourth, first, center});
    return mesh;
}

spo::StlMesh makeEarTriangulatedReferencePatch() {
    constexpr int boundaryCount = 12;
    constexpr double pi = 3.14159265358979323846;
    std::vector<spo::StlVec3> boundary;
    boundary.reserve(boundaryCount);
    for (int index = 0; index < boundaryCount; ++index) {
        const double angle = 2.0 * pi * static_cast<double>(index) /
            static_cast<double>(boundaryCount);
        boundary.push_back({std::cos(angle), std::sin(angle), 0.0});
    }
    spo::StlMesh mesh;
    for (int index = 1; index + 1 < boundaryCount; ++index) {
        mesh.addTriangle({{}, boundary[0], boundary[index], boundary[index + 1]});
    }
    return mesh;
}

} // namespace

void run_surface_reduction_tests() {
    {
        spo::StlHybridParameterizationReport report;
        const auto parameterization =
            spo::StlHybridParameterization::buildFromPatchMesh(makeCurvedReferencePatch(), report);
        assert(parameterization);
        assert(report.success);
        assert(report.flippedTriangleCount == 0);
        const auto first = parameterization->project(gp_Pnt(0.25, 0.25, 0.05));
        const auto second = parameterization->project(gp_Pnt(0.75, 0.75, 0.05));
        assert(first.valid);
        assert(second.valid);
        assert(first.distance < 1.0e-12);
        assert(second.distance < 1.0e-12);
        assert(std::hypot(first.u - second.u, first.v - second.v) > 0.25);
    }

    {
        spo::StlHybridParameterizationReport report;
        const auto parameterization =
            spo::StlHybridParameterization::buildFromPatchMesh(
                makeEarTriangulatedReferencePatch(), report);
        assert(parameterization);
        assert(report.success);
        assert(report.flippedTriangleCount == 0);
        assert(report.minimumNormalizedJacobian > 1.0e-6);
    }

    {
        const auto document = makePlanarStrip(3);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = {0};
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        assert(result.report.outcome == spo::SurfaceReductionOutcome::InvalidSelection);
        assert(!result.canApply());
    }

    {
        const auto document = makePlanarStrip(2);
        const auto fit = spo::RestrictedBSplinePatchFitter().fit(document, allFaces(document));
        assert(fit.success());
        assert(fit.report.selectionPassed);
        assert(fit.report.holdoutPassed);
        assert(fit.report.boundaryHoldoutSampleCount > 0);
        assert(fit.report.maxBoundaryDistance <= 0.03);
        assert(fit.report.maxHoldoutDistance <= 0.03);
        assert(fit.report.maxHoldoutNormalAngleDegrees <= 15.0);
        assert(fit.report.parameterizationValid);
    }

    {
        const auto document = makePlanarStrip(3);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        request.options.maximumControlPoints = 8;
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        assert(result.report.outcome == spo::SurfaceReductionOutcome::InvalidSelection);
        assert(!result.canApply());
    }

    {
        const spo::ShapeDocument document(BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape(), {});
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        assert(result.report.inputFaceCount == 6);
        assert(result.report.outputFaceCount == 6);
        assert(result.report.outcome == spo::SurfaceReductionOutcome::NoAcceptableReduction);
        assert(!result.canApply());
    }

    {
        const auto document = makePlanarStrip(3);
        assert(document.topology().faceCount() == 3);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        request.lockedEdges = internalEdges(document);
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        assert(result.report.inputFaceCount == 3);
        assert(result.report.outputFaceCount == 3);
        assert(result.report.outcome == spo::SurfaceReductionOutcome::NoAcceptableReduction);
        assert(!result.canApply());
    }

    {
        const auto document = makePlanarStrip(3);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        if (!result.canApply()) {
            std::cerr << "3-face reduction failed: " << result.report.message << '\n';
        }
        assert(result.report.inputFaceCount == 3);
        assert(result.report.outputFaceCount < 3);
        assert(result.report.reductionRatio > 0.0);
        assert(result.report.maxDistance <= request.options.tolerance3d);
        assert(result.report.maxNormalAngleDegrees <= request.options.normalToleranceDegrees);
        assert(result.report.parameterizationValid);
        assert(!result.report.commercialCadReports.empty());
        assert(result.report.commercialCadReports.front().passed);
        assert(result.report.topologyReport.afterBRepCheckValid);
        assert(result.report.topologyReport.stepRoundtripOk);
        assert(result.canApply());

        spo::CommandContext staleContext;
        staleContext.document = makePlanarStrip(2);
        spo::ReduceSelectedFacesCommand staleCommand(result);
        assert(!staleCommand.execute(staleContext).success());

        spo::CommandContext context;
        context.document = document;
        spo::ReduceSelectedFacesCommand command(result);
        assert(command.execute(context).success());
        const auto reducedFaceCount = context.document.topology().faceCount();
        assert(reducedFaceCount < document.topology().faceCount());
        assert(command.undo(context).success());
        assert(context.document.topology().faceCount() == document.topology().faceCount());
        assert(command.redo(context).success());
        assert(context.document.topology().faceCount() == reducedFaceCount);
    }

    {
        const auto document = makePlanarStrip(7);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        if (!result.canApply()) {
            std::cerr << "7-face reduction failed: " << result.report.message << '\n';
        }
        assert(result.report.inputFaceCount == 7);
        assert(result.report.outputFaceCount <= 3);
        assert(result.report.targetAchieved);
        assert(result.canApply());
    }

    {
        const auto document = makePlanarStrip(20);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        request.options.targetOutputRatio = 0.95;
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        assert(result.report.inputFaceCount == 20);
        assert(result.report.outputFaceCount == 19);
        assert(result.report.targetAchieved);
        assert(result.canApply());
    }

    {
        const auto document = makeSplitBSpline(7);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        assert(result.report.inputFaceCount == 7);
        assert(result.report.outputFaceCount <= 3);
        assert(result.report.maxDistance <= 1.0e-10);
        assert(result.report.maxNormalAngleDegrees <= 1.0e-10);
        assert(result.canApply());
    }

    {
        const auto document = makePlanarRing();
        assert(document.topology().faceCount() == 8);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        request.options.targetOutputRatio = 0.2;
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        if (!result.canApply()) {
            std::cerr << "planar ring reduction failed: " << result.report.message << '\n';
        }
        assert(result.report.outputFaceCount == 1);
        assert(result.canApply());
        int wireCount = 0;
        for (TopExp_Explorer explorer(result.reducedShape, TopAbs_WIRE); explorer.More(); explorer.Next()) {
            ++wireCount;
        }
        assert(wireCount == 2);
    }

    {
        const auto document = makePartiallyReducibleCorner();
        assert(document.topology().faceCount() == 3);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        assert(result.report.outputFaceCount == 2);
        assert(result.report.outcome == spo::SurfaceReductionOutcome::Reduced);
        assert(!result.report.targetAchieved);
        assert(result.canApply());
    }
}
