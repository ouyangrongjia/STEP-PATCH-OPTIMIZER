#include "reduction/region_refit/JointBSplineNetworkFitter.h"
#include "reduction/region_refit/RegionalSurfaceRefitCache.h"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

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
    mesh.updateIntegrationWeights();
    return mesh;
}

std::vector<spo::RegionPatchLayout> planarLayouts(
    const spo::RegionReferenceMesh& mesh,
    const int patchCount = 2) {
    spo::RegionRefitOptions options;
    options.fixedPatchCount = patchCount;
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
    options.tolerance3d = 0.1;
    options.curveOnSurfaceTolerance = 0.03;
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
    assert(!fitted.attempts.empty());
    assert(!fitted.attempts.front().iterations.empty());
    const auto& iteration = fitted.attempts.front().iterations.back();
    assert(iteration.seamG0ConstraintCount > 0);
    assert(iteration.seamG1SoftConstraintCount > 0);
}

void test_constrained_kkt_writes_nonzero_boundary_targets_to_rhs() {
    auto mesh = makePlanarFitMesh(1);
    const gp_Vec translation(120.0, -75.0, 42.0);
    for (auto& vertex : mesh.vertices) {
        vertex.point.Translate(translation);
    }
    mesh.updateIntegrationWeights();
    const auto layouts = planarLayouts(mesh, 1);
    spo::RegionTopologyConstraints constraints;
    const std::array<std::pair<gp_Pnt, gp_Pnt>, 2> edges {{
        {gp_Pnt(120.0, -75.0, 42.0), gp_Pnt(121.0, -75.0, 42.0)},
        {gp_Pnt(120.0, -75.0, 42.0), gp_Pnt(121.0, -75.0, 42.0)}}};
    for (std::size_t index = 0; index < edges.size(); ++index) {
        spo::RegionBoundaryEdgeConstraint constraint;
        constraint.edgeId = index;
        constraint.originalEdge = BRepBuilderAPI_MakeEdge(
            edges[index].first, edges[index].second).Edge();
        constraint.hardPositionConstraint = true;
        constraints.boundaryEdges.push_back(constraint);
        constraints.orderedBoundaryEdges.push_back(index);
    }
    for (auto& vertex : mesh.vertices) {
        if (!vertex.boundary) continue;
        const auto y = vertex.point.Y() + 75.0;
        if (std::abs(y) <= 1.0e-12) vertex.boundaryEdgeIds = {1, 0};
    }

    spo::RegionRefitOptions options;
    options.tolerance3d = 0.05;
    options.curveOnSurfaceTolerance = 0.01;
    options.normalToleranceDegrees = 1.0;
    options.maximumFitIterations = 4;
    const auto fitted = spo::JointBSplineNetworkFitter().fit(
        mesh,
        constraints,
        layouts,
        options,
        std::chrono::steady_clock::now() + std::chrono::seconds(10));

    assert(!fitted.attempts.empty());
    assert(!fitted.attempts.front().iterations.empty());
    const auto& iteration = fitted.attempts.front().iterations.back();
    assert(iteration.nonZeroBoundaryRhsCount > 0);
    assert(iteration.boundaryTargetMinimum[0] >= 119.9);
    assert(iteration.boundaryTargetMaximum[2] >= 41.9);
    assert(iteration.constraintRowsBefore >= iteration.constraintRowsAfter);
    assert(iteration.constraintRowsAfter > 0);
    assert(iteration.constraintEstimatedRank == iteration.constraintRowsAfter);
    assert(iteration.minimumFullHardConstraintResidual <= 1.0e-4);
    assert(iteration.hardConstraintFeasible);
    assert(iteration.hardConstraintStatus == "Feasible");
    assert(iteration.maximumHardConstraintResidual <= 1.0e-4);
}

void test_inconsistent_dense_hard_constraints_are_reported_explicitly() {
    auto mesh = makePlanarFitMesh(1);
    const gp_Vec translation(120.0, -75.0, 42.0);
    for (auto& vertex : mesh.vertices) vertex.point.Translate(translation);
    mesh.updateIntegrationWeights();
    const auto layouts = planarLayouts(mesh, 1);
    spo::RegionTopologyConstraints constraints;
    for (std::size_t index = 0; index < 2; ++index) {
        spo::RegionBoundaryEdgeConstraint constraint;
        constraint.edgeId = index;
        constraint.originalEdge = BRepBuilderAPI_MakeEdge(
            gp_Pnt(120.0, -75.0, 42.0 + static_cast<double>(index)),
            gp_Pnt(121.0, -75.0, 42.0 + static_cast<double>(index))).Edge();
        constraint.hardPositionConstraint = true;
        constraints.boundaryEdges.push_back(constraint);
        constraints.orderedBoundaryEdges.push_back(index);
    }
    for (auto& vertex : mesh.vertices) {
        if (vertex.boundary && std::abs(vertex.point.Y() + 75.0) <= 1.0e-12) {
            vertex.boundaryEdgeIds = {0, 1};
        }
    }

    spo::RegionRefitOptions options;
    options.tolerance3d = 0.05;
    options.curveOnSurfaceTolerance = 0.01;
    options.maximumFitIterations = 2;
    const auto fitted = spo::JointBSplineNetworkFitter().fit(
        mesh,
        constraints,
        layouts,
        options,
        std::chrono::steady_clock::now() + std::chrono::seconds(10));

    assert(!fitted.attempts.empty());
    assert(!fitted.attempts.front().iterations.empty());
    const auto& iteration = fitted.attempts.front().iterations.back();
    assert(!iteration.hardConstraintFeasible);
    assert(iteration.hardConstraintStatus == "InfeasibleHardBoundaryConstraints");
    assert(iteration.minimumFullHardConstraintResidual > options.curveOnSurfaceTolerance);
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

void test_joint_fitter_preserves_best_effort_network_when_normal_gate_fails() {
    auto mesh = makePlanarFitMesh(8);
    for (auto& vertex : mesh.vertices) {
        vertex.normal = gp_Dir(1.0, 0.0, 0.0);
        for (auto& source : vertex.sources) source.normal = vertex.normal;
    }
    const auto layouts = planarLayouts(mesh, 1);
    spo::RegionRefitOptions options;
    options.tolerance3d = 0.05;
    options.curveOnSurfaceTolerance = 0.05;
    options.normalToleranceDegrees = 0.01;
    options.maximumFitIterations = 3;
    const auto fitted = spo::JointBSplineNetworkFitter().fit(
        mesh,
        {},
        layouts,
        options,
        std::chrono::steady_clock::now() + std::chrono::seconds(10));

    assert(!fitted.success());
    assert(!fitted.network);
    assert(fitted.bestEffortNetwork);
    assert(!fitted.bestEffortNetwork->fitGatePassed);
    assert(fitted.bestEffortNetwork->normalizedViolationScore > 1.0);
}

void test_edge_consistency_classifies_geometry_and_display_cracks() {
    spo::RegionEdgeConsistencyReport report;
    assert(spo::classifyRegionEdgeConsistency(report, 0.001) == "TopologicalOpening");

    report.sharedSameEdge = true;
    report.sameRange = true;
    report.sameParameter = true;
    report.curveOnSurfaceMaximumDeviation = 0.002;
    assert(spo::classifyRegionEdgeConsistency(report, 0.001) == "CurveOnSurfaceMismatch");

    report.curveOnSurfaceMaximumDeviation = 0.0;
    report.adjacentSurfaceMaximumSeparation = 0.002;
    assert(spo::classifyRegionEdgeConsistency(report, 0.001) == "AdjacentSurfaceSeparation");

    report.adjacentSurfaceMaximumSeparation = 0.0;
    report.triangulationPolylineSeparations = {0.0, 0.002, 0.0};
    assert(spo::classifyRegionEdgeConsistency(report, 0.001) == "TriangulationArtifact");

    report.triangulationPolylineSeparations.assign(3, 0.0);
    assert(spo::classifyRegionEdgeConsistency(report, 0.001) == "Passed");
}

void test_area_weighting_is_invariant_to_planar_sampling_density() {
    const auto coarse = makePlanarFitMesh(6);
    const auto dense = makePlanarFitMesh(18);
    spo::RegionRefitOptions options;
    options.tolerance3d = 0.05;
    options.curveOnSurfaceTolerance = 0.05;
    options.normalToleranceDegrees = 1.0;
    options.maximumFitIterations = 4;
    const auto coarseFit = spo::JointBSplineNetworkFitter().fit(
        coarse,
        {},
        planarLayouts(coarse, 1),
        options,
        std::chrono::steady_clock::now() + std::chrono::seconds(10));
    const auto denseFit = spo::JointBSplineNetworkFitter().fit(
        dense,
        {},
        planarLayouts(dense, 1),
        options,
        std::chrono::steady_clock::now() + std::chrono::seconds(10));

    assert(coarseFit.success());
    assert(denseFit.success());
    const auto coarsePoint = coarseFit.network->surfaces.front()->Value(0.5, 0.5);
    const auto densePoint = denseFit.network->surfaces.front()->Value(0.5, 0.5);
    const GeomAPI_ProjectPointOnSurf coarseOnDense(
        coarsePoint,
        denseFit.network->surfaces.front());
    const GeomAPI_ProjectPointOnSurf denseOnCoarse(
        densePoint,
        coarseFit.network->surfaces.front());
    assert(coarseOnDense.NbPoints() > 0);
    assert(denseOnCoarse.NbPoints() > 0);
    assert(coarseOnDense.LowerDistance() <= 1.0e-6);
    assert(denseOnCoarse.LowerDistance() <= 1.0e-6);
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
    fitOptions.tolerance3d = 0.1;
    fitOptions.curveOnSurfaceTolerance = 0.03;
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
    test_constrained_kkt_writes_nonzero_boundary_targets_to_rhs();
    test_inconsistent_dense_hard_constraints_are_reported_explicitly();
    test_joint_fitter_passes_planar_two_patch_network();
    test_joint_fitter_honors_preflight_memory_budget();
    test_joint_fitter_honors_expired_deadline();
    test_joint_fitter_preserves_best_effort_network_when_normal_gate_fails();
    test_edge_consistency_classifies_geometry_and_display_cracks();
    test_area_weighting_is_invariant_to_planar_sampling_density();
    test_regional_refit_cache_roundtrips_sampling_layout_and_positive_fit();
}
