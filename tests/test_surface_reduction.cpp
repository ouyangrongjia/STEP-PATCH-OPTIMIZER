#include "brep/ShapeDocument.h"
#include "command/CommandContext.h"
#include "command/ReduceSelectedFacesCommand.h"
#include "reduction/SurfaceReductionEngine.h"
#include "reduction/RestrictedBSplinePatchFitter.h"
#include "reduction/SurfaceReductionFitCache.h"
#include "reduction/StlHybridParameterization.h"

#include <QFile>
#include <QTemporaryDir>

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
#include <future>
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

spo::ShapeDocument makeSmoothFold() {
    constexpr double rise = 0.02;
    BRepBuilderAPI_Sewing sewing(1.0e-7);
    sewing.Add(makePolygonFace({
        gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(1.0, 0.0, 0.0),
        gp_Pnt(1.0, 1.0, 0.0), gp_Pnt(0.0, 1.0, 0.0)}));
    sewing.Add(makePolygonFace({
        gp_Pnt(1.0, 0.0, 0.0), gp_Pnt(2.0, 0.0, rise),
        gp_Pnt(2.0, 1.0, rise), gp_Pnt(1.0, 1.0, 0.0)}));
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

spo::StlMesh makeSplitBSplineScanPatch() {
    spo::StlMesh mesh;
    constexpr int segments = 6;
    auto point = [](int x, int y) {
        const double px = 3.0 * static_cast<double>(x) / static_cast<double>(segments);
        const double py = 3.0 * static_cast<double>(y) / static_cast<double>(segments);
        return spo::StlVec3 {px, py, 0.08 * px * py};
    };
    for (int x = 0; x < segments; ++x) {
        for (int y = 0; y < segments; ++y) {
            const auto first = point(x, y);
            const auto second = point(x + 1, y);
            const auto third = point(x + 1, y + 1);
            const auto fourth = point(x, y + 1);
            mesh.addTriangle({{}, first, second, third});
            mesh.addTriangle({{}, first, third, fourth});
        }
    }
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
        for (int x = -2; x <= 12; ++x) {
            for (int y = -2; y <= 12; ++y) {
                const gp_Pnt point(
                    static_cast<double>(x) / 10.0,
                    static_cast<double>(y) / 10.0,
                    0.1);
                const auto indexed = parameterization->project(point);
                const auto brute = parameterization->projectBruteForceForTesting(point);
                assert(indexed.valid == brute.valid);
                assert(std::abs(indexed.distance - brute.distance) < 1.0e-12);
                assert(std::abs(indexed.u - brute.u) < 1.0e-12);
                assert(std::abs(indexed.v - brute.v) < 1.0e-12);
                const double u = static_cast<double>(x) / 10.0;
                const double v = static_cast<double>(y) / 10.0;
                assert(parameterization->contains(u, v, 1.0e-10) ==
                    parameterization->containsBruteForceForTesting(u, v, 1.0e-10));
            }
        }
    }

    {
        QTemporaryDir temporary;
        assert(temporary.isValid());
        spo::SurfaceReductionExecutionOptions execution;
        execution.cacheMode = spo::SurfaceReductionCacheMode::ReadWrite;
        execution.cacheDirectory = temporary.path().toStdWString();
        execution.sourceFingerprint = "source-a";
        execution.scanFingerprint = "none";
        execution.algorithmFingerprint = "algorithm-a";
        spo::SurfaceReductionFitCache cache(execution);
        const auto document = makePlanarStrip(2);
        const auto fit = spo::RestrictedBSplinePatchFitter().fit(document, allFaces(document));
        assert(fit.success());
        spo::RestrictedBSplinePatchFitOptions options;
        const auto key = cache.key({0, 1}, options);
        spo::SurfaceReductionFitCacheEntry entry;
        entry.success = true;
        entry.face = fit.face;
        entry.surface = fit.surface;
        entry.report = fit.report;
        assert(cache.store(key, entry));
        const auto loaded = cache.load(key);
        assert(loaded.has_value());
        assert(loaded->success);
        assert(!loaded->face.IsNull());
        assert(loaded->report.kktSolveCount == fit.report.kktSolveCount);

        options.tolerance3d = 0.031;
        const auto negativeKey = cache.key({0, 1}, options);
        spo::SurfaceReductionFitCacheEntry negative;
        negative.report.failureStage = spo::RestrictedBSplinePatchFitFailureStage::ModelSelection;
        negative.report.message = "negative";
        assert(cache.store(negativeKey, negative));
        const auto loadedNegative = cache.load(negativeKey);
        assert(loadedNegative.has_value());
        assert(!loadedNegative->success);
        assert(loadedNegative->report.message == "negative");

        const auto cacheFile = temporary.path() + "/" +
            QString::fromStdString(key).left(2) + "/" + QString::fromStdString(key) + ".json";
        QFile corrupted(cacheFile);
        assert(corrupted.open(QIODevice::WriteOnly | QIODevice::Truncate));
        corrupted.write("broken");
        corrupted.close();
        assert(!cache.load(key).has_value());

        auto changedExecution = execution;
        changedExecution.sourceFingerprint = "source-b";
        const spo::SurfaceReductionFitCache changedCache(changedExecution);
        assert(changedCache.key({0, 1}, options) != negativeKey);

        std::vector<std::future<bool>> writes;
        std::vector<std::string> concurrentKeys;
        for (int index = 0; index < 4; ++index) {
            auto concurrentOptions = options;
            concurrentOptions.tolerance3d += static_cast<double>(index + 1) * 0.001;
            concurrentKeys.push_back(cache.key({0, 1}, concurrentOptions));
            writes.push_back(std::async(std::launch::async, [&cache, negative, key = concurrentKeys.back()] {
                return cache.store(key, negative);
            }));
        }
        for (auto& write : writes) assert(write.get());
        for (const auto& concurrentKey : concurrentKeys) {
            assert(cache.load(concurrentKey).has_value());
        }
    }

    {
        QTemporaryDir temporary;
        assert(temporary.isValid());
        const auto document = makeSmoothFold();
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        spo::SurfaceReductionExecutionOptions replay;
        replay.cacheMode = spo::SurfaceReductionCacheMode::ReadOnlyRequired;
        replay.cacheDirectory = temporary.path().toStdWString();
        replay.sourceFingerprint = "smooth-fold";
        replay.algorithmFingerprint = "test";
        const auto missing = spo::SurfaceReductionEngine().reduce(document, request, replay);
        assert(missing.report.persistentFitCacheRequiredMiss);
        assert(missing.report.fitCacheMissCount == 0);

        auto refresh = replay;
        refresh.cacheMode = spo::SurfaceReductionCacheMode::Refresh;
        const auto cold = spo::SurfaceReductionEngine().reduce(document, request, refresh);
        assert(cold.report.persistentFitCacheWriteCount > 0);
        const auto warm = spo::SurfaceReductionEngine().reduce(document, request, replay);
        assert(!warm.report.persistentFitCacheRequiredMiss);
        assert(warm.report.persistentFitCacheHitCount > 0);
        assert(warm.report.fitCacheMissCount == 0);
        assert(cold.report.outputGroups == warm.report.outputGroups);
        assert(cold.report.outcome == warm.report.outcome);
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
        if (!fit.success()) {
            std::cerr << "direct restricted fit failed: " << fit.report.message
                      << ", constraints=" << fit.report.boundaryConstraintCount
                      << ", residual=" << fit.report.maxBoundaryConstraintResidual
                      << ", parameterization=" << spo::toString(fit.report.parameterizationSource)
                      << ", min_jacobian=" << fit.report.minimumNormalizedJacobian
                      << '\n';
        }
        assert(fit.success());
        assert(fit.report.selectionPassed);
        assert(fit.report.holdoutPassed);
        assert(fit.report.boundaryHoldoutSampleCount >= 97);
        assert(fit.report.maxBoundaryDistance <= 0.03);
        assert(fit.report.maxHoldoutDistance <= 0.03);
        assert(fit.report.maxHoldoutNormalAngleDegrees <= 15.0);
        assert(fit.report.parameterizationValid);
        assert(fit.report.supportSurfaceExtentRatio <= 2.0);
        assert(fit.report.supportSurfaceMaxPoleOverflowRatio <= 0.5);
    }

    {
        const auto document = makeSplitBSpline(2);
        spo::StlHybridParameterizationReport chartReport;
        const auto chart = spo::StlHybridParameterization::buildFromStepSelection(
            document,
            allFaces(document),
            0.03,
            chartReport);
        assert(chart);
        assert(chartReport.success);
        assert(chartReport.source == spo::SurfaceParameterizationSource::StepHarmonic);
        const auto fit = spo::RestrictedBSplinePatchFitter().fit(document, allFaces(document));
        if (!fit.success()) {
            std::cerr << "curved hard-boundary fit failed: " << fit.report.message
                      << ", constraints=" << fit.report.boundaryConstraintCount
                      << ", controls=" << fit.report.controlPointCount
                      << ", residual=" << fit.report.maxBoundaryConstraintResidual
                      << ", boundary=" << fit.report.maxBoundaryDistance
                      << '\n';
        }
        assert(fit.success());
        assert(fit.report.parameterizationSource ==
            spo::SurfaceParameterizationSource::SupportSurfaceProjection);
        assert(fit.report.boundaryConstraintCount > 0);
        assert(fit.report.maxBoundaryConstraintResidual <= 3.0e-8);
        assert(fit.report.maxBoundaryUvShift == 0.0);
        assert(fit.report.maxBoundaryDistance <= 0.03);
        assert(!fit.report.uvIterations.empty());
        assert(fit.report.iterationCount <= 5);
        assert(fit.report.supportSurfaceExtentRatio <= 2.0);
        assert(fit.report.supportSurfaceMaxPoleOverflowRatio <= 0.5);
    }

    {
        const auto document = makeSplitBSpline(2);
        const auto scan = makeSplitBSplineScanPatch();
        spo::RestrictedBSplinePatchFitOptions options;
        options.scanReference = &scan;
        options.scanReferenceCoverage = spo::ScanReferenceCoverage::SelectionCrop;
        const auto fit = spo::RestrictedBSplinePatchFitter().fit(
            document,
            allFaces(document),
            options);
        if (!fit.success()) {
            std::cerr << "curved scan-reference fit failed: " << fit.report.message
                      << ", constraints=" << fit.report.boundaryConstraintCount
                      << ", residual=" << fit.report.maxBoundaryConstraintResidual
                      << ", boundary=" << fit.report.maxBoundaryDistance
                      << ", scan=" << fit.report.scanReferenceSampleCount
                      << '\n';
        }
        assert(fit.success());
        assert(fit.report.scanReferenceCoverage == spo::ScanReferenceCoverage::SelectionCrop);
        assert(fit.report.scanReferenceSampleCount > 0);
        assert(fit.report.maxStepToScanDistance <= options.maximumScanReferenceDistance);
    }

    {
        const auto document = makePlanarStrip(2);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        request.scanReferenceStl = std::make_shared<spo::StlMesh>(makeCurvedReferencePatch());
        const auto result = spo::SurfaceReductionEngine().reduce(document, request);
        assert(result.report.outcome == spo::SurfaceReductionOutcome::InvalidSelection);
        assert(!result.canApply());
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
        const spo::ShapeDocument preview(result.previewShape, {});
        assert(preview.topology().faceCount() ==
            static_cast<std::size_t>(result.report.outputFaceCount));

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
        const auto document = makePlanarStrip(5);
        spo::SurfaceReductionRequest request;
        request.selectedFaces = allFaces(document);
        request.options.targetOutputRatio = 0.4;
        const auto first = spo::SurfaceReductionEngine().reduce(document, request);
        const auto second = spo::SurfaceReductionEngine().reduce(document, request);
        spo::SurfaceReductionExecutionOptions parallelExecution;
        parallelExecution.fitWorkerCount = 4;
        const auto parallel = spo::SurfaceReductionEngine().reduce(
            document, request, parallelExecution);
        assert(first.canApply());
        assert(second.canApply());
        assert(parallel.canApply());
        assert(first.report.outputGroups == second.report.outputGroups);
        assert(first.report.outputGroups == parallel.report.outputGroups);
        assert(first.report.outputFaceCount == second.report.outputFaceCount);
        assert(first.report.outputFaceCount == parallel.report.outputFaceCount);
        assert(first.report.expandedStateCount == second.report.expandedStateCount);
        assert(first.report.generatedStateCount == second.report.generatedStateCount);
        assert(first.report.generatedStateCount > first.report.acceptedMergeCount);
        assert(!first.report.searchBudgetExhausted);
        assert(parallel.report.fitWorkerCount == 4);
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
        const spo::ShapeDocument preview(result.previewShape, {});
        assert(preview.topology().faceCount() == 2);
    }
}
