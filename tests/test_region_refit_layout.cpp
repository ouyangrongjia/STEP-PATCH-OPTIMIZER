#include "reduction/region_refit/RegionPatchLayout.h"

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <map>
#include <set>

namespace {

spo::RegionReferenceMesh makeGrid(const int columns, const int rows) {
    spo::RegionReferenceMesh mesh;
    for (int row = 0; row <= rows; ++row) {
        for (int column = 0; column <= columns; ++column) {
            const auto x = static_cast<double>(column) / columns;
            const auto y = static_cast<double>(row) / rows;
            const auto z = 0.08 * std::sin(x * 2.0) * std::sin(y * 1.5);
            spo::RegionReferenceVertex vertex;
            vertex.point = gp_Pnt(x, y, z);
            vertex.normal = gp_Dir(-0.16 * std::cos(x * 2.0) * std::sin(y * 1.5),
                -0.12 * std::sin(x * 2.0) * std::cos(y * 1.5), 1.0);
            spo::RegionReferenceSourceSample source;
            source.faceId = 1;
            source.normal = vertex.normal;
            source.normalDefined = true;
            source.curvatureDefined = true;
            source.minimumCurvature = -0.1 * x;
            source.maximumCurvature = 0.15 * y;
            vertex.sources.push_back(source);
            vertex.boundary = row == 0 || row == rows || column == 0 || column == columns;
            vertex.hardBoundary = vertex.boundary;
            mesh.vertices.push_back(std::move(vertex));
        }
    }
    const auto index = [columns](const int column, const int row) {
        return static_cast<std::size_t>(row * (columns + 1) + column);
    };
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
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

void test_fixed_layout_has_exact_patch_count_and_disk_parameterizations() {
    const auto mesh = makeGrid(16, 12);
    spo::RegionRefitOptions options;
    options.fixedPatchCount = 4;
    options.maximumParameterDistortion = 1000.0;
    const auto generated = spo::RegionPatchLayoutGenerator().generate(
        mesh, spo::RegionRefitLayoutMode::FixedPatchCount, options);

    assert(generated.success());
    assert(generated.selectedCandidates.size() == 1);
    const auto& layout = generated.selectedCandidates.front();
    assert(layout.patchCount == 4);
    assert(layout.patches.size() == 4);
    assert(layout.trianglePatches.size() == mesh.triangles.size());
    std::set<int> patches(layout.trianglePatches.begin(), layout.trianglePatches.end());
    assert(patches == std::set<int>({0, 1, 2, 3}));
    assert(std::all_of(layout.patches.begin(), layout.patches.end(), [](const auto& patch) {
        return !patch.triangleIndices.empty() && !patch.boundaryLoop.empty() &&
            patch.flippedTriangleCount == 0;
    }));
    for (const auto& patch : layout.patches) {
        assert(std::all_of(patch.parameters.begin(), patch.parameters.end(), [](const auto& uv) {
            return uv[0] >= 0.0 && uv[0] <= 1.0 && uv[1] >= 0.0 && uv[1] <= 1.0;
        }));
        std::map<std::size_t, std::array<double, 2>> parameters;
        for (std::size_t index = 0; index < patch.vertexIndices.size(); ++index) {
            parameters.emplace(patch.vertexIndices[index], patch.parameters[index]);
        }
        for (std::size_t side = 0; side < patch.boundarySides.size(); ++side) {
            assert(patch.boundarySides[side].size() >= 2);
            for (const auto vertex : patch.boundarySides[side]) {
                const auto uv = parameters.at(vertex);
                if (side == 0) assert(std::abs(uv[1]) <= 1.0e-12);
                if (side == 1) assert(std::abs(uv[0] - 1.0) <= 1.0e-12);
                if (side == 2) assert(std::abs(uv[1] - 1.0) <= 1.0e-12);
                if (side == 3) assert(std::abs(uv[0]) <= 1.0e-12);
            }
        }
    }
}

void test_target_layout_uses_requested_first_then_alternating_deviation_order() {
    const auto mesh = makeGrid(18, 14);
    spo::RegionRefitOptions options;
    options.requestedPatchCount = 4;
    options.patchCountSearchRadius = 2;
    options.maximumParameterDistortion = 1000.0;
    const auto generated = spo::RegionPatchLayoutGenerator().generate(
        mesh, spo::RegionRefitLayoutMode::TargetPatchCount, options);

    assert(generated.success());
    assert(generated.selectedCandidates.size() == 1);
    assert(generated.selectedCandidates.front().patchCount == 4);
    std::vector<int> counts;
    for (const auto& candidate : generated.candidates) counts.push_back(candidate.patchCount);
    assert(counts == std::vector<int>({4, 3, 5, 2, 6}));
}

void test_adaptive_layout_is_deterministic_and_screens_at_most_three() {
    const auto mesh = makeGrid(12, 10);
    spo::RegionRefitOptions options;
    options.maximumAdaptivePatchCount = 6;
    options.maximumParameterDistortion = 1000.0;
    const auto first = spo::RegionPatchLayoutGenerator().generate(
        mesh, spo::RegionRefitLayoutMode::Adaptive, options);
    const auto second = spo::RegionPatchLayoutGenerator().generate(
        mesh, spo::RegionRefitLayoutMode::Adaptive, options);

    assert(first.success());
    assert(second.success());
    assert(!first.selectedCandidates.empty());
    assert(first.selectedCandidates.size() <= 3);
    assert(first.selectedCandidates.size() == second.selectedCandidates.size());
    for (std::size_t index = 0; index < first.selectedCandidates.size(); ++index) {
        assert(first.selectedCandidates[index].hash == second.selectedCandidates[index].hash);
        assert(first.selectedCandidates[index].patchCount == second.selectedCandidates[index].patchCount);
    }
}

void test_layout_rejects_disconnected_reference_mesh() {
    auto mesh = makeGrid(1, 1);
    const auto offset = mesh.vertices.size();
    for (auto vertex : mesh.vertices) {
        vertex.point.SetX(vertex.point.X() + 3.0);
        mesh.vertices.push_back(vertex);
    }
    const auto originalTriangleCount = mesh.triangles.size();
    for (std::size_t index = 0; index < originalTriangleCount; ++index) {
        auto triangle = mesh.triangles[index];
        for (auto& vertex : triangle.vertices) {
            vertex += offset;
        }
        mesh.triangles.push_back(triangle);
    }
    spo::RegionRefitOptions options;
    options.fixedPatchCount = 1;
    const auto generated = spo::RegionPatchLayoutGenerator().generate(
        mesh, spo::RegionRefitLayoutMode::FixedPatchCount, options);

    assert(!generated.success());
    assert(generated.status == spo::RegionRefitStatus::NoValidatedLayoutWithinBudget);
}

}

void run_region_refit_layout_tests() {
    test_fixed_layout_has_exact_patch_count_and_disk_parameterizations();
    test_target_layout_uses_requested_first_then_alternating_deviation_order();
    test_adaptive_layout_is_deterministic_and_screens_at_most_three();
    test_layout_rejects_disconnected_reference_mesh();
}
