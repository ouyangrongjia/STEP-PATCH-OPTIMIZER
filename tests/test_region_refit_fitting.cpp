#include "reduction/region_refit/JointBSplineNetworkFitter.h"

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <chrono>

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

}

void run_region_refit_fitting_tests() {
    test_joint_fitter_passes_planar_two_patch_network();
    test_joint_fitter_honors_preflight_memory_budget();
    test_joint_fitter_honors_expired_deadline();
}
