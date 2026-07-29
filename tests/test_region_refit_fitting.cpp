#include "reduction/region_refit/JointBSplineNetworkFitter.h"
#include "reduction/region_refit/RecoveredFittingSpecification.h"
#include "reduction/region_refit/RegionalSurfaceRefitCache.h"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_BSplineSurface.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <cassert>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <tuple>

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
            source.uv = gp_Pnt2d(
                static_cast<double>(column) / resolution,
                static_cast<double>(row) / resolution);
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

void test_face_uv_mapping_uses_same_face_barycentric_surface_parameters() {
    const auto carrier = makePlanarFitMesh(1);
    spo::RegionPatchLayout layout;
    layout.patchCount = 1;
    layout.trianglePatches = {0, 0};
    spo::RegionPatchParameterization patch;
    patch.triangleIndices = {0, 1};
    patch.vertexIndices = {0, 1, 2, 3};
    patch.parameters = {{
        {0.0, 0.0},
        {1.0, 0.0},
        {0.0, 1.0},
        {1.0, 1.0}}};
    patch.surfaceParameters = {{
        {0.0, 0.0},
        {1.0, 0.0},
        {0.0, 1.0},
        {1.0, 1.0}}};
    layout.patches.push_back(std::move(patch));

    spo::RegionReferenceMesh fitting;
    for (const auto& [faceId, u, v] :
         std::array<std::tuple<spo::FaceId, double, double>, 3> {{
             {1, 0.25, 0.75},
             {1, 2.0, 2.0},
             {2, 0.5, 0.5}}}) {
        spo::RegionReferenceVertex vertex;
        spo::RegionReferenceSourceSample source;
        source.faceId = faceId;
        source.uv = gp_Pnt2d(u, v);
        vertex.sources.push_back(source);
        fitting.vertices.push_back(std::move(vertex));
    }

    const auto mapped =
        spo::mapRegionReferenceSamplesToSurfaceParameters(
            fitting, carrier, layout);
    assert(mapped.samples.size() == 2);
    assert(mapped.sameFaceBarycentricCount == 1);
    assert(mapped.sameFaceFallbackCount == 1);
    assert(mapped.crossFaceFailureCount == 1);
    assert(std::abs(mapped.samples.front().surfaceParameter[0] - 0.25) <=
        1.0e-12);
    assert(std::abs(mapped.samples.front().surfaceParameter[1] - 0.75) <=
        1.0e-12);
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
    assert(fitted.network->quality.legality.passed);
    assert(fitted.network->quality.physicalSurfaceArea > 0.0);
    assert(fitted.network->quality.fairnessEnergy <= 1.0e-8);
    assert(std::isfinite(
        fitted.network->quality.controlNetSecondDifferenceEnergy));
}

void test_geomagic_equivalent_refuses_partial_recovered_specification() {
    const auto specification =
        spo::geomagicWrap2021MechanicalSpecification();
    assert(specification.degreeU == 3);
    assert(specification.degreeV == 3);
    assert(specification.nonAdaptiveMode == 1);
    assert(specification.adaptiveMode == 19);
    assert(!specification.behaviorEquivalentReady());
    assert(
        specification.storedFacetNormalHandling.evidence ==
        spo::RecoveredEvidenceLevel::ConfirmedDynamic);
    assert(!specification.normalTerm.recovered());

    const auto mesh = makePlanarFitMesh(4);
    spo::RegionRefitOptions options;
    options.fittingMode =
        spo::RegionalFittingMode::GeomagicEquivalent;
    const auto fitted = spo::JointBSplineNetworkFitter().fit(
        mesh,
        {},
        planarLayouts(mesh, 1),
        options,
        std::chrono::steady_clock::now() +
            std::chrono::seconds(10));
    assert(!fitted.success());
    assert(
        fitted.status ==
        spo::RegionRefitStatus::UnsupportedFittingSpecification);
    assert(fitted.attempts.empty());
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
    assert(iteration.hardConstraintStatus == "BoundaryInfeasibleDiagnostic");
    assert(iteration.minimumFullHardConstraintResidual > options.curveOnSurfaceTolerance);
    assert(iteration.initialControlPriorScale > 0.0);
    assert(iteration.initialSupportBlendAlpha >= 0.0);
    assert(iteration.initialSupportBlendAlpha <= 1.0);
    assert(iteration.constraintCorrectionBlendAlpha >= 0.0);
    assert(iteration.constraintCorrectionBlendAlpha <= 1.0);
    assert(std::isfinite(iteration.affineBaselineTrainingMaximumDistance));
    assert(std::isfinite(iteration.affineBaselineBoundaryMaximumDistance));
    assert(fitted.bestEffortNetwork);
    assert(!fitted.bestEffortNetwork->fitGatePassed);
    assert(!fitted.bestEffortNetwork->boundaryLocked);
    assert(!fitted.bestEffortNetwork->constructibleForBRep);
    assert(fitted.bestEffortNetwork->fitOutcome ==
        "UnconstrainedInspectionOnly");
}

void test_trimmed_disk_jacobian_ignores_unused_rectangular_corners() {
    auto mesh = makePlanarFitMesh(8);
    auto layouts = planarLayouts(mesh, 1);
    auto& patch = layouts.front().patches.front();
    for (std::size_t local = 0; local < patch.parameters.size(); ++local) {
        auto& parameter = patch.parameters[local];
        parameter[0] = 0.5 + 0.5 * (parameter[0] - 0.5);
        parameter[1] = 0.5 + 0.5 * (parameter[1] - 0.5);
        const auto u = parameter[0];
        const auto x = 0.09 * u - std::pow(u - 0.5, 3.0) / 3.0;
        mesh.vertices[patch.vertexIndices[local]].point =
            gp_Pnt(x, parameter[1], 0.0);
    }
    mesh.updateIntegrationWeights();

    const std::array<double, 4> xControls {
        1.0 / 24.0,
        -7.0 / 600.0,
        61.0 / 600.0,
        29.0 / 600.0};
    TColgp_Array2OfPnt poles(1, 4, 1, 4);
    for (int u = 1; u <= 4; ++u) {
        for (int v = 1; v <= 4; ++v) {
            poles.SetValue(
                u, v, gp_Pnt(xControls[static_cast<std::size_t>(u - 1)],
                             static_cast<double>(v - 1) / 3.0,
                             0.0));
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
        false,
        false);

    spo::RegionRefitOptions options;
    options.tolerance3d = 0.1;
    std::string reason;
    assert(spo::regionLocalSupportDomainPassed(
        surface, mesh, patch, options, reason));
    gp_Pnt unusedPoint;
    gp_Vec unusedU;
    gp_Vec unusedV;
    surface->D1(0.0, 0.5, unusedPoint, unusedU, unusedV);
    assert(unusedU.Crossed(unusedV).Dot(gp_Vec(0.0, 0.0, 1.0)) < 0.0);
    for (const auto& parameter : patch.parameters) {
        gp_Pnt trimPoint;
        gp_Vec trimU;
        gp_Vec trimV;
        surface->D1(parameter[0], parameter[1], trimPoint, trimU, trimV);
        assert(trimU.Crossed(trimV).Dot(gp_Vec(0.0, 0.0, 1.0)) > 0.0);
    }
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
    assert(fitted.bestEffortNetwork->boundaryLocked);
    assert(!fitted.bestEffortNetwork->constructibleForBRep);
    assert(fitted.bestEffortNetwork->fitOutcome ==
        "BoundaryLockedDiagnostic");
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

    report.internalSeam = true;
    report.parameterSemantic = "SharedEdgeParameter";
    report.commonParameterSampleCount = 257;
    assert(spo::classifyRegionValidatorConsistency(
        4.96138,
        std::vector<spo::RegionEdgeConsistencyReport> {report},
        0.001) == "ValidatorInconsistent");
    assert(spo::classifyRegionValidatorConsistency(
        1.0e-14,
        std::vector<spo::RegionEdgeConsistencyReport> {report},
        0.001) == "Consistent");
    report.parameterSemantic = "LegacyMeshVertexPair";
    assert(spo::classifyRegionValidatorConsistency(
        1.0e-14,
        std::vector<spo::RegionEdgeConsistencyReport> {report},
        0.001) == "ValidatorInconsistent");
}

void test_surface_legality_states_are_independent() {
    spo::SurfaceLegalityReport legality;
    assert(spo::classifySurfaceUvTopology(legality) == "Passed");
    assert(spo::classifySurfaceJacobianLegality(legality) == "Passed");

    legality.uvFlippedTriangleCount = 7;
    assert(spo::classifySurfaceUvTopology(legality) ==
        "FlippedUvTriangle");
    assert(spo::classifySurfaceJacobianLegality(legality) == "Passed");

    legality.jacobianOrientationFlipCount = 3;
    assert(spo::classifySurfaceUvTopology(legality) ==
        "FlippedUvTriangle");
    assert(spo::classifySurfaceJacobianLegality(legality) ==
        "SurfaceOrientationFlip");
}

void test_parameter_quality_uses_topological_triangle_orientation() {
    auto mesh = makePlanarFitMesh(1);
    std::swap(
        mesh.triangles[1].vertices[1],
        mesh.triangles[1].vertices[2]);
    spo::RegionPatchParameterization patch;
    patch.triangleIndices = {0, 1};
    std::vector<int> orientations;
    std::string message;
    assert(spo::orientPatchTriangles(
        mesh, patch, orientations, message));
    assert(orientations[0] == 1);
    assert(orientations[1] == -1);

    const std::array<std::array<double, 2>, 4> uv {{
        {0.0, 0.0},
        {1.0, 0.0},
        {0.0, 1.0},
        {1.0, 1.0}}};
    for (const auto triangleIndex : patch.triangleIndices) {
        const auto& triangle = mesh.triangles[triangleIndex];
        const auto& a = uv[triangle.vertices[0]];
        const auto& b = uv[triangle.vertices[1]];
        const auto& c = uv[triangle.vertices[2]];
        const auto orientedArea =
            static_cast<double>(
                orientations[triangleIndex]) *
            ((b[0] - a[0]) * (c[1] - a[1]) -
                (b[1] - a[1]) * (c[0] - a[0]));
        assert(orientedArea > 0.0);
    }
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
    spo::RegionPatchEdgePcurve cachedPcurve;
    cachedPcurve.edgeIndex = 17;
    cachedPcurve.forward = false;
    cachedPcurve.firstParameter = 2.0;
    cachedPcurve.lastParameter = 5.0;
    cachedPcurve.startNode = 3;
    cachedPcurve.endNode = 9;
    cachedPcurve.loopOrder = 1;
    cachedPcurve.parameters = {2.0, 3.5, 5.0};
    cachedPcurve.uvPoints = {{
        {0.1, 0.2}, {0.5, 0.2}, {0.9, 0.2}}};
    cachedPcurve.hash = "pcurve-cache-v8";
    layouts.selectedCandidates.front().patches.front().edgePcurves = {
        cachedPcurve};
    layouts.selectedCandidates.front().patches.front().pcurveLoopClosed = true;
    layouts.selectedCandidates.front().patches.front()
        .pcurveOrientationConsistent = true;
    layouts.selectedCandidates.front().patches.front()
        .minimumToMedianUvAreaRatio = 0.125;
    layouts.selectedCandidates.front().patches.front()
        .distortionP995 = 3.25;
    layouts.selectedCandidates.front().patches.front()
        .logicalSideCandidateCount = 7;
    layouts.selectedCandidates.front().patches.front()
        .selectedLogicalSideHash = "logical-side-cache-v11";
    layouts.selectedCandidates.front().patches.front()
        .boundaryRepresentabilityResidual = 1.0e-9;
    layouts.selectedCandidates.front().patches.front()
        .surfaceParameterCarrierBlend = 0.375;
    layouts.selectedCandidates.front().patches.front()
        .carrierParameterWeightMode = "InverseEdgeLength";
    layouts.candidates.front().distortionP995 = 4.5;
    layouts.candidates.front().maximumSurfaceParameterCarrierBlend = 0.375;
    layouts.candidates.front().logicalSideCandidateCount = 11;
    layouts.candidates.front().selectedLogicalSideHash =
        "candidate-logical-side-cache-v13";
    cache.saveLayouts(layouts);
    spo::RegionPatchLayoutGenerationResult loadedLayouts;
    spo::RegionRefitCacheEvent layoutRead;
    assert(cache.loadLayouts(loadedLayouts, layoutRead));
    assert(layoutRead.hit);
    assert(std::abs(
        loadedLayouts.candidates.front().distortionP995 - 4.5) <=
        1.0e-15);
    assert(std::abs(
        loadedLayouts.candidates.front()
                .maximumSurfaceParameterCarrierBlend -
            0.375) <=
        1.0e-15);
    assert(
        loadedLayouts.candidates.front().logicalSideCandidateCount ==
        11);
    assert(
        loadedLayouts.candidates.front().selectedLogicalSideHash ==
        "candidate-logical-side-cache-v13");
    assert(loadedLayouts.selectedCandidates.front().hash == layouts.selectedCandidates.front().hash);
    const auto& loadedPatch =
        loadedLayouts.selectedCandidates.front().patches.front();
    assert(loadedPatch.edgePcurves.size() == 1);
    assert(loadedPatch.edgePcurves.front().hash == cachedPcurve.hash);
    assert(loadedPatch.edgePcurves.front().parameters ==
        cachedPcurve.parameters);
    assert(loadedPatch.edgePcurves.front().uvPoints ==
        cachedPcurve.uvPoints);
    assert(loadedPatch.pcurveLoopClosed);
    assert(loadedPatch.pcurveOrientationConsistent);
    assert(std::abs(
        loadedPatch.minimumToMedianUvAreaRatio - 0.125) <= 1.0e-15);
    assert(std::abs(loadedPatch.distortionP995 - 3.25) <= 1.0e-15);
    assert(loadedPatch.logicalSideCandidateCount == 7);
    assert(loadedPatch.selectedLogicalSideHash ==
        "logical-side-cache-v11");
    assert(std::abs(
        loadedPatch.surfaceParameterCarrierBlend - 0.375) <=
        1.0e-15);
    assert(
        loadedPatch.carrierParameterWeightMode ==
        "InverseEdgeLength");

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
    assert(loadedFit.network->quality.totalPoleCount ==
        fitted.network->quality.totalPoleCount);
    assert(loadedFit.network->quality.maximumPoleCountPerSide ==
        fitted.network->quality.maximumPoleCountPerSide);
    assert(loadedFit.network->quality.surfaceComplexities.size() ==
        loadedFit.network->surfaces.size());
    for (std::size_t patch = 0;
         patch < loadedFit.network->surfaces.size();
         ++patch) {
        const auto& surface = loadedFit.network->surfaces[patch];
        const auto& complexity =
            loadedFit.network->quality.surfaceComplexities[patch];
        assert(complexity.patchIndex == static_cast<int>(patch));
        assert(complexity.uPoleCount == surface->NbUPoles());
        assert(complexity.vPoleCount == surface->NbVPoles());
        assert(complexity.uKnotCount == surface->NbUKnots());
        assert(complexity.vKnotCount == surface->NbVKnots());
    }
    assert(std::abs(
        loadedFit.network->quality.fairnessEnergy -
        fitted.network->quality.fairnessEnergy) <= 1.0e-15);

    const auto overwriteWithVersion10 =
        [](const std::filesystem::path& path) {
            std::fstream stream(
                path,
                std::ios::in | std::ios::out | std::ios::binary);
            assert(stream);
            const std::array<unsigned char, 4> version10 {
                0x00, 0x00, 0x00, 0x0a};
            stream.seekp(4);
            stream.write(
                reinterpret_cast<const char*>(version10.data()),
                static_cast<std::streamsize>(version10.size()));
            assert(stream.good());
        };
    overwriteWithVersion10(
        cacheDirectory / cache.key() / "layouts.bin");
    spo::RegionPatchLayoutGenerationResult rejectedLayouts;
    spo::RegionRefitCacheEvent rejectedLayoutEvent;
    assert(!cache.loadLayouts(
        rejectedLayouts, rejectedLayoutEvent));
    overwriteWithVersion10(
        cacheDirectory / cache.key() / "fit.bin");
    spo::JointBSplineNetworkFitResult rejectedFit;
    spo::RegionRefitCacheEvent rejectedFitEvent;
    assert(!cache.loadFit(
        layouts.selectedCandidates,
        rejectedFit,
        rejectedFitEvent));

    std::filesystem::remove_all(cacheDirectory);
}

}

void run_region_refit_fitting_tests() {
    test_face_uv_mapping_uses_same_face_barycentric_surface_parameters();
    test_constrained_kkt_writes_nonzero_boundary_targets_to_rhs();
    test_inconsistent_dense_hard_constraints_are_reported_explicitly();
    test_trimmed_disk_jacobian_ignores_unused_rectangular_corners();
    test_joint_fitter_passes_planar_two_patch_network();
    test_geomagic_equivalent_refuses_partial_recovered_specification();
    test_joint_fitter_honors_preflight_memory_budget();
    test_joint_fitter_honors_expired_deadline();
    test_joint_fitter_preserves_best_effort_network_when_normal_gate_fails();
    test_edge_consistency_classifies_geometry_and_display_cracks();
    test_surface_legality_states_are_independent();
    test_parameter_quality_uses_topological_triangle_orientation();
    test_area_weighting_is_invariant_to_planar_sampling_density();
    test_regional_refit_cache_roundtrips_sampling_layout_and_positive_fit();
}
