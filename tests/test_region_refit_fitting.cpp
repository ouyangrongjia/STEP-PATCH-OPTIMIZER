#include "reduction/region_refit/JointBSplineNetworkFitter.h"
#include "reduction/region_refit/NativeRegionPatchNetworkBuilder.h"
#include "reduction/region_refit/RegionalSurfaceRefitCache.h"

#include "brep/ShapeDocument.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_Curve.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pln.hxx>

#include <cassert>
#include <chrono>
#include <filesystem>

namespace {

spo::RegionReferenceMesh makePlanarFitMesh(const int resolution) {
    spo::RegionReferenceMesh mesh;
    for (int row = 0; row <= resolution; ++row) {
        for (int column = 0; column <= resolution; ++column) {
            spo::RegionReferenceVertex vertex;
            vertex.point = gp_Pnt(
                static_cast<double>(column) / resolution,
                static_cast<double>(row) / resolution,
                0.0);
            vertex.normal = gp_Dir(0.0, 0.0, 1.0);
            vertex.boundary = row == 0 || row == resolution || column == 0 || column == resolution;
            vertex.hardBoundary = vertex.boundary;
            spo::RegionReferenceSourceSample source;
            source.faceId = 1;
            source.normal = vertex.normal;
            source.normalDefined = true;
            source.curvatureDefined = true;
            vertex.sources.push_back(source);
            mesh.vertices.push_back(vertex);
        }
    }
    const auto index = [resolution](const int column, const int row) {
        return static_cast<std::size_t>(row * (resolution + 1) + column);
    };
    for (int row = 0; row < resolution; ++row) {
        for (int column = 0; column < resolution; ++column) {
            const auto lowerLeft = index(column, row);
            const auto lowerRight = index(column + 1, row);
            const auto upperLeft = index(column, row + 1);
            const auto upperRight = index(column + 1, row + 1);
            mesh.triangles.push_back({{lowerLeft, lowerRight, upperRight}, 1});
            mesh.triangles.push_back({{lowerLeft, upperRight, upperLeft}, 1});
        }
    }
    return mesh;
}

std::vector<spo::RegionPatchLayout> planarLayouts(const spo::RegionReferenceMesh& mesh) {
    spo::RegionRefitOptions options;
    options.fixedPatchCount = 2;
    options.maximumParameterDistortion = 1000.0;
    const auto generated = spo::RegionPatchLayoutGenerator().generate(
        mesh, spo::RegionRefitLayoutMode::FixedPatchCount, options);
    assert(generated.success());
    return generated.selectedCandidates;
}

void test_joint_fitter_passes_planar_two_patch_network() {
    const auto mesh = makePlanarFitMesh(10);
    const auto layouts = planarLayouts(mesh);
    spo::RegionRefitOptions options;
    options.tolerance3d = 0.05;
    options.curveOnSurfaceTolerance = 0.02;
    options.normalToleranceDegrees = 1.0;
    options.maximumFitIterations = 8;
    const auto fitted = spo::JointBSplineNetworkFitter().fit(
        mesh,
        {},
        layouts,
        options,
        std::chrono::steady_clock::now() + std::chrono::seconds(10));

    assert(fitted.success());
    assert(fitted.network);
    assert(fitted.network->layout.patchCount == 2);
    assert(fitted.network->surfaces.size() == 2);
    assert(fitted.network->holdoutMaximumDistance <= options.tolerance3d);
    assert(fitted.network->boundaryMaximumDistance <= options.curveOnSurfaceTolerance);
    assert(fitted.network->seamMaximumDistance <= options.curveOnSurfaceTolerance);
}

void test_joint_fitter_honors_preflight_memory_budget() {
    const auto mesh = makePlanarFitMesh(6);
    const auto layouts = planarLayouts(mesh);
    spo::RegionRefitOptions options;
    options.solverMemoryPreflightMiB = 0;
    const auto fitted = spo::JointBSplineNetworkFitter().fit(
        mesh,
        {},
        layouts,
        options,
        std::chrono::steady_clock::now() + std::chrono::seconds(10));

    assert(!fitted.success());
    assert(fitted.status == spo::RegionRefitStatus::MemoryBudgetExceeded);
    assert(!fitted.network);
}

void test_joint_fitter_honors_expired_deadline() {
    const auto mesh = makePlanarFitMesh(4);
    const auto layouts = planarLayouts(mesh);
    const auto fitted = spo::JointBSplineNetworkFitter().fit(
        mesh,
        {},
        layouts,
        {},
        std::chrono::steady_clock::now() - std::chrono::milliseconds(1));

    assert(!fitted.success());
    assert(fitted.status == spo::RegionRefitStatus::TimeBudgetExceeded);
    assert(fitted.attempts.empty());
}

void test_native_patch_network_reuses_shared_edges_and_builds_two_faces() {
    auto mesh = makePlanarFitMesh(10);
    const auto sourceFace = BRepBuilderAPI_MakeFace(
        gp_Pln(gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0))),
        0.0,
        1.0,
        0.0,
        1.0).Face();
    const spo::ShapeDocument document(sourceFace, "synthetic-region-refit.step");
    spo::RegionTopologyConstraints constraints;
    constraints.selectedFaces.push_back(0);
    for (spo::EdgeId edgeId = 0; edgeId < document.topology().edgeCount(); ++edgeId) {
        const auto edge = document.topology().edge(edgeId);
        double first = 0.0;
        double last = 0.0;
        const auto curve = BRep_Tool::Curve(edge, first, last);
        assert(!curve.IsNull());
        spo::RegionBoundaryEdgeConstraint constraint;
        constraint.edgeId = edgeId;
        constraint.selectedFaceId = 0;
        constraint.originalEdge = edge;
        constraint.selectedPcurve = BRep_Tool::CurveOnSurface(edge, sourceFace, first, last);
        constraint.selectedFirstParameter = first;
        constraint.selectedLastParameter = last;
        constraints.boundaryEdges.push_back(constraint);
        constraints.orderedBoundaryEdges.push_back(edgeId);
        for (auto& vertex : mesh.vertices) {
            if (!vertex.boundary) continue;
            GeomAPI_ProjectPointOnCurve projector(vertex.point, curve, first, last);
            if (projector.NbPoints() > 0 && projector.LowerDistance() <= 1.0e-8) {
                vertex.boundaryEdgeIds.push_back(edgeId);
            }
        }
    }

    const auto layouts = planarLayouts(mesh);
    spo::RegionRefitOptions options;
    options.tolerance3d = 0.05;
    options.curveOnSurfaceTolerance = 0.05;
    options.normalToleranceDegrees = 1.0;
    options.maximumFitIterations = 8;
    const auto fitted = spo::JointBSplineNetworkFitter().fit(
        mesh,
        constraints,
        layouts,
        options,
        std::chrono::steady_clock::now() + std::chrono::seconds(10));
    assert(fitted.success());

    const auto built = spo::NativeRegionPatchNetworkBuilder().build(
        mesh,
        constraints,
        *fitted.network,
        options.curveOnSurfaceTolerance);
    assert(built.success());
    assert(built.replacementFaces.size() == 2);
    assert(built.internalSeamCount == 1);
    assert(built.sameParameterFailureCount == 0);

    int sharedUseCount = 0;
    for (const auto& face : built.replacementFaces) {
        bool found = false;
        for (TopExp_Explorer explorer(face, TopAbs_EDGE); explorer.More(); explorer.Next()) {
            if (TopoDS::Edge(explorer.Current()).IsSame(built.internalSeamEdges.front())) {
                found = true;
                break;
            }
        }
        if (found) ++sharedUseCount;
    }
    int commonFaceEdgeCount = 0;
    for (TopExp_Explorer first(built.replacementFaces[0], TopAbs_EDGE); first.More(); first.Next()) {
        for (TopExp_Explorer second(built.replacementFaces[1], TopAbs_EDGE); second.More(); second.Next()) {
            if (TopoDS::Edge(first.Current()).IsSame(TopoDS::Edge(second.Current()))) ++commonFaceEdgeCount;
        }
    }
    assert(sharedUseCount == 2);
    assert(commonFaceEdgeCount == 1);
}

void test_regional_refit_cache_roundtrips_sampling_layout_and_positive_fit() {
    const auto token = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto cacheDirectory = std::filesystem::temp_directory_path() /
        ("spo-region-refit-cache-" + token);
    spo::RegionRefitRequest request;
    request.selectedFaces = {4, 2};
    request.layoutMode = spo::RegionRefitLayoutMode::FixedPatchCount;
    request.options.fixedPatchCount = 2;
    request.options.maximumParameterDistortion = 1000.0;
    request.options.inputStepSha256 = std::string(64, 'a');
    request.options.algorithmFingerprint = "cache-unit-test";
    request.options.cachePolicy = spo::RegionRefitCachePolicy::ReadWrite;
    request.options.cacheDirectory = cacheDirectory;
    const spo::RegionalSurfaceRefitCache cache(request);
    assert(cache.enabled());
    assert(!cache.key().empty());

    spo::StepAdaptiveRegionSamplingResult sampling;
    sampling.mesh = makePlanarFitMesh(10);
    sampling.report.status = spo::RegionRefitStatus::Success;
    sampling.report.message = "Synthetic sampling.";
    sampling.report.vertexCount = sampling.mesh.vertices.size();
    sampling.report.triangleCount = sampling.mesh.triangles.size();
    sampling.report.meshFingerprint = sampling.mesh.deterministicFingerprint();
    const auto samplingWrite = cache.saveSampling(sampling);
    assert(samplingWrite.writeMilliseconds >= 0.0);
    spo::StepAdaptiveRegionSamplingResult loadedSampling;
    spo::RegionRefitCacheEvent samplingRead;
    assert(cache.loadSampling(loadedSampling, samplingRead));
    assert(samplingRead.hit);
    assert(loadedSampling.report.meshFingerprint == sampling.report.meshFingerprint);

    auto layouts = spo::RegionPatchLayoutGenerator().generate(
        sampling.mesh,
        request.layoutMode,
        request.options);
    assert(layouts.success());
    cache.saveLayouts(layouts);
    spo::RegionPatchLayoutGenerationResult loadedLayouts;
    spo::RegionRefitCacheEvent layoutRead;
    assert(cache.loadLayouts(loadedLayouts, layoutRead));
    assert(layoutRead.hit);
    assert(loadedLayouts.selectedCandidates.front().hash == layouts.selectedCandidates.front().hash);

    spo::RegionRefitOptions fitOptions = request.options;
    fitOptions.tolerance3d = 0.05;
    fitOptions.curveOnSurfaceTolerance = 0.02;
    fitOptions.normalToleranceDegrees = 1.0;
    fitOptions.maximumFitIterations = 8;
    const auto fitted = spo::JointBSplineNetworkFitter().fit(
        sampling.mesh,
        {},
        layouts.selectedCandidates,
        fitOptions,
        std::chrono::steady_clock::now() + std::chrono::seconds(10));
    assert(fitted.success());
    cache.saveFit(fitted);
    spo::JointBSplineNetworkFitResult loadedFit;
    spo::RegionRefitCacheEvent fitRead;
    assert(cache.loadFit(layouts.selectedCandidates, loadedFit, fitRead));
    assert(fitRead.hit);
    assert(!fitRead.negativeEntry);
    assert(loadedFit.success());
    assert(loadedFit.network->surfaces.size() == fitted.network->surfaces.size());
    assert(loadedFit.network->layout.hash == fitted.network->layout.hash);

    std::filesystem::remove_all(cacheDirectory);
}

}

void run_region_refit_fitting_tests() {
    test_joint_fitter_passes_planar_two_patch_network();
    test_joint_fitter_honors_preflight_memory_budget();
    test_joint_fitter_honors_expired_deadline();
    test_native_patch_network_reuses_shared_edges_and_builds_two_faces();
    test_regional_refit_cache_roundtrips_sampling_layout_and_positive_fit();
}
