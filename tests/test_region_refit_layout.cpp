#include "reduction/region_refit/JointBSplineNetworkFitter.h"
#include "reduction/region_refit/NativeRegionPatchNetworkBuilder.h"
#include "reduction/region_refit/RegionPatchLayout.h"
#include "reduction/region_refit/RegionCurveNetwork.h"
#include "reduction/region_refit/RegionalSurfaceRefitCache.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

namespace {

struct PlanarCurveFixture {
    spo::RegionReferenceMesh mesh;
    spo::RegionTopologyConstraints constraints;
};

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

spo::RegionReferenceMesh makeTurningDisk(const int boundaryCount) {
    spo::RegionReferenceMesh mesh;
    spo::RegionReferenceVertex center;
    center.point = gp_Pnt(12.0, -8.0, 3.0);
    center.normal = gp_Dir(0.0, 0.0, 1.0);
    center.sources.push_back({1, {}, center.normal, true, 0.0, 0.0, true});
    mesh.vertices.push_back(center);
    for (int index = 0; index < boundaryCount; ++index) {
        const auto angle = 2.0 * 3.14159265358979323846 *
            static_cast<double>(index) / static_cast<double>(boundaryCount);
        const auto radius = index % 2 == 0 ? 2.0 : 1.35;
        spo::RegionReferenceVertex vertex;
        vertex.point = gp_Pnt(
            center.point.X() + radius * std::cos(angle),
            center.point.Y() + radius * std::sin(angle),
            center.point.Z());
        vertex.normal = center.normal;
        vertex.sources.push_back({1, {}, vertex.normal, true, 0.0, 0.0, true});
        vertex.boundary = true;
        vertex.hardBoundary = true;
        vertex.hardCorner = true;
        vertex.boundaryEdgeIds.push_back(static_cast<spo::EdgeId>(index));
        mesh.vertices.push_back(vertex);
    }
    for (int index = 0; index < boundaryCount; ++index) {
        mesh.triangles.push_back({{
            0,
            static_cast<std::size_t>(index + 1),
            static_cast<std::size_t>((index + 1) % boundaryCount + 1)}, 1});
    }
    return mesh;
}

PlanarCurveFixture makePlanarCurveFixture(const int resolution) {
    PlanarCurveFixture fixture;
    fixture.mesh = makeGrid(resolution, resolution);
    for (int row = 0; row <= resolution; ++row) {
        for (int column = 0; column <= resolution; ++column) {
            auto& vertex = fixture.mesh.vertices[
                static_cast<std::size_t>(
                    row * (resolution + 1) + column)];
            vertex.point.SetZ(0.0);
            vertex.normal = gp_Dir(0.0, 0.0, 1.0);
            vertex.boundaryEdgeIds.clear();
            if (row == 0) vertex.boundaryEdgeIds.push_back(0);
            if (column == resolution) vertex.boundaryEdgeIds.push_back(1);
            if (row == resolution) vertex.boundaryEdgeIds.push_back(2);
            if (column == 0) vertex.boundaryEdgeIds.push_back(3);
            vertex.boundaryEndpoint = vertex.boundaryEdgeIds.size() > 1;
            vertex.hardCorner = vertex.boundaryEndpoint;
        }
    }
    const std::array<TopoDS_Edge, 4> edges {
        BRepBuilderAPI_MakeEdge(
            gp_Pnt(0.0, 0.0, 0.0),
            gp_Pnt(1.0, 0.0, 0.0)).Edge(),
        BRepBuilderAPI_MakeEdge(
            gp_Pnt(1.0, 0.0, 0.0),
            gp_Pnt(1.0, 1.0, 0.0)).Edge(),
        BRepBuilderAPI_MakeEdge(
            gp_Pnt(1.0, 1.0, 0.0),
            gp_Pnt(0.0, 1.0, 0.0)).Edge(),
        BRepBuilderAPI_MakeEdge(
            gp_Pnt(0.0, 1.0, 0.0),
            gp_Pnt(0.0, 0.0, 0.0)).Edge()};
    BRepBuilderAPI_MakeWire wire;
    for (const auto& edge : edges) wire.Add(edge);
    const auto face = BRepBuilderAPI_MakeFace(wire.Wire()).Face();
    fixture.constraints.selectedFaces = {1};
    fixture.constraints.selectedFaceGeometry.push_back({1, face});
    for (std::size_t index = 0; index < edges.size(); ++index) {
        spo::RegionBoundaryEdgeConstraint constraint;
        constraint.edgeId = index;
        constraint.selectedFaceId = 1;
        constraint.originalEdge = edges[index];
        constraint.selectedFace = face;
        constraint.hardPositionConstraint = true;
        fixture.constraints.boundaryEdges.push_back(std::move(constraint));
    }
    fixture.mesh.updateIntegrationWeights();
    return fixture;
}

TopoDS_Face makeUnitSquareFace(const double z) {
    const std::array<TopoDS_Edge, 4> edges {
        BRepBuilderAPI_MakeEdge(
            gp_Pnt(0.0, 0.0, z),
            gp_Pnt(1.0, 0.0, z)).Edge(),
        BRepBuilderAPI_MakeEdge(
            gp_Pnt(1.0, 0.0, z),
            gp_Pnt(1.0, 1.0, z)).Edge(),
        BRepBuilderAPI_MakeEdge(
            gp_Pnt(1.0, 1.0, z),
            gp_Pnt(0.0, 1.0, z)).Edge(),
        BRepBuilderAPI_MakeEdge(
            gp_Pnt(0.0, 1.0, z),
            gp_Pnt(0.0, 0.0, z)).Edge()};
    BRepBuilderAPI_MakeWire wire;
    for (const auto& edge : edges) wire.Add(edge);
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}

void test_fixed_layout_has_exact_patch_count_and_trimmed_disk_parameterizations() {
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
        assert(patch.domainType == "TrimmedDiskConvexBoundary");
        assert(patch.diskTopologyPassed);
        assert(patch.boundaryChainCount >= 1);
        assert(std::all_of(patch.parameters.begin(), patch.parameters.end(), [](const auto& uv) {
            return uv[0] >= 0.0 && uv[0] <= 1.0 && uv[1] >= 0.0 && uv[1] <= 1.0;
        }));
        assert(patch.flippedTriangleCount == 0);
        assert(patch.minimumSignedUvArea > 0.0);
    }
}

void test_trimmed_disk_accepts_more_than_four_boundary_turns_without_uv_flips() {
    const auto mesh = makeTurningDisk(8);
    spo::RegionRefitOptions options;
    options.fixedPatchCount = 1;
    options.maximumParameterDistortion = 1000.0;
    const auto generated = spo::RegionPatchLayoutGenerator().generate(
        mesh, spo::RegionRefitLayoutMode::FixedPatchCount, options);

    assert(generated.success());
    const auto& patch = generated.selectedCandidates.front().patches.front();
    assert(patch.domainType == "TrimmedDiskConvexBoundary");
    assert(patch.trimCornerCount > 4);
    assert(patch.diskTopologyPassed);
    assert(patch.flippedTriangleCount == 0);
    assert(patch.minimumSignedUvArea > 0.0);
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
    assert(first.candidates.size() == 6);
    assert(second.candidates.size() == 6);
    assert(!first.selectedCandidates.empty());
    assert(first.selectedCandidates.size() <= 3);
    assert(first.selectedCandidates.size() == second.selectedCandidates.size());
    for (std::size_t index = 0; index < first.selectedCandidates.size(); ++index) {
        assert(first.selectedCandidates[index].hash == second.selectedCandidates[index].hash);
        assert(first.selectedCandidates[index].patchCount == second.selectedCandidates[index].patchCount);
    }
}

void test_forced_diagnostic_layout_selection_is_bounded_and_deterministic() {
    const auto mesh = makeGrid(12, 10);
    spo::RegionRefitOptions targetOptions;
    targetOptions.requestedPatchCount = 4;
    targetOptions.patchCountSearchRadius = 0;
    targetOptions.maximumParameterDistortion = 1.0e-12;
    const auto target = spo::RegionPatchLayoutGenerator().generate(
        mesh,
        spo::RegionRefitLayoutMode::TargetPatchCount,
        targetOptions,
        true);

    assert(target.success());
    assert(target.selectedCandidates.size() == 1);
    assert(target.selectedCandidates.front().patchCount == 4);
    assert(!target.candidates.front().parameterizationPassed);
    assert(target.candidates.front().constructibleParameterization);

    spo::RegionRefitOptions adaptiveOptions = targetOptions;
    adaptiveOptions.maximumAdaptivePatchCount = 6;
    adaptiveOptions.maximumFullFitCount = 3;
    const auto first = spo::RegionPatchLayoutGenerator().generate(
        mesh,
        spo::RegionRefitLayoutMode::Adaptive,
        adaptiveOptions,
        true);
    const auto second = spo::RegionPatchLayoutGenerator().generate(
        mesh,
        spo::RegionRefitLayoutMode::Adaptive,
        adaptiveOptions,
        true);

    assert(first.success());
    assert(first.selectedCandidates.size() <= 3);
    assert(first.selectedCandidates.size() == second.selectedCandidates.size());
    for (std::size_t index = 0; index < first.selectedCandidates.size(); ++index) {
        assert(first.selectedCandidates[index].hash == second.selectedCandidates[index].hash);
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
    const auto forced = spo::RegionPatchLayoutGenerator().generate(
        mesh, spo::RegionRefitLayoutMode::FixedPatchCount, options, true);

    assert(!generated.success());
    assert(generated.status == spo::RegionRefitStatus::NoValidatedLayoutWithinBudget);
    assert(!forced.success());
    assert(forced.constructibleCandidates.empty());
}

void test_curve_network_uses_global_nearest_selected_face_projection() {
    auto fixture = makePlanarCurveFixture(8);
    constexpr double farZ = 25.0;
    fixture.constraints.selectedFaces = {2, 1};
    fixture.constraints.selectedFaceGeometry.insert(
        fixture.constraints.selectedFaceGeometry.begin(),
        {2, makeUnitSquareFace(farZ)});
    for (auto& vertex : fixture.mesh.vertices) {
        vertex.sources.front().faceId = 2;
    }

    spo::RegionRefitOptions options;
    options.fixedPatchCount = 2;
    options.maximumParameterDistortion = 1000.0;
    options.curveProjectionTolerance = 0.01;
    const auto generated = spo::RegionCurveNetworkGenerator().generate(
        fixture.mesh,
        fixture.constraints,
        spo::RegionRefitLayoutMode::FixedPatchCount,
        options);

    assert(generated.success());
    const auto& network = generated.selectedCandidates.front().network;
    const auto internal = std::find_if(
        network.edges.begin(),
        network.edges.end(),
        [](const auto& edge) { return !edge.originalBoundary; });
    assert(internal != network.edges.end());
    const auto middle = internal->curve->Value(
        (internal->firstParameter + internal->lastParameter) * 0.5);
    assert(std::abs(middle.Z()) <= options.curveProjectionTolerance);
    assert(internal->maximumProjectionError <=
        options.curveProjectionTolerance);
}

void test_curve_network_accepts_near_preferred_face_projection() {
    auto fixture = makePlanarCurveFixture(8);
    constexpr double preferredZ = 0.005;
    fixture.constraints.selectedFaces = {2, 1};
    fixture.constraints.selectedFaceGeometry.insert(
        fixture.constraints.selectedFaceGeometry.begin(),
        {2, makeUnitSquareFace(preferredZ)});
    for (auto& vertex : fixture.mesh.vertices) {
        vertex.sources.front().faceId = 2;
    }

    spo::RegionRefitOptions options;
    options.fixedPatchCount = 2;
    options.maximumParameterDistortion = 1000.0;
    options.curveProjectionTolerance = 0.01;
    const auto generated = spo::RegionCurveNetworkGenerator().generate(
        fixture.mesh,
        fixture.constraints,
        spo::RegionRefitLayoutMode::FixedPatchCount,
        options);

    assert(generated.success());
    const auto& network = generated.selectedCandidates.front().network;
    const auto internal = std::find_if(
        network.edges.begin(),
        network.edges.end(),
        [](const auto& edge) { return !edge.originalBoundary; });
    assert(internal != network.edges.end());
    const auto middle = internal->curve->Value(
        (internal->firstParameter + internal->lastParameter) * 0.5);
    assert(middle.Z() > preferredZ * 0.5);
    assert(internal->maximumProjectionError <=
        options.curveProjectionTolerance);
}

void test_curve_network_classifies_internal_continuity_from_evidence() {
    spo::RegionRefitOptions options;
    options.fixedPatchCount = 2;
    options.maximumParameterDistortion = 1000.0;
    options.curveProjectionTolerance = 0.01;
    options.g2CurvatureTolerance = 0.01;
    const auto classify = [&](const PlanarCurveFixture& fixture,
                              const spo::RegionRefitOptions& configured) {
        const auto generated = spo::RegionCurveNetworkGenerator().generate(
            fixture.mesh,
            fixture.constraints,
            spo::RegionRefitLayoutMode::FixedPatchCount,
            configured);
        assert(generated.success());
        const auto& edges =
            generated.selectedCandidates.front().network.edges;
        const auto internal = std::find_if(
            edges.begin(),
            edges.end(),
            [](const auto& edge) { return !edge.originalBoundary; });
        assert(internal != edges.end());
        return internal->continuity;
    };

    const auto ordinary = makePlanarCurveFixture(8);
    assert(classify(ordinary, options) ==
        spo::RegionCurveContinuity::G1);

    auto hardBreak = makePlanarCurveFixture(8);
    for (auto& vertex : hardBreak.mesh.vertices) {
        if (!vertex.boundary) vertex.normalDiscontinuity = true;
    }
    assert(classify(hardBreak, options) ==
        spo::RegionCurveContinuity::G0);

    auto g2Options = options;
    g2Options.enableInternalSeamG2 = true;
    assert(classify(ordinary, g2Options) ==
        spo::RegionCurveContinuity::G2);

    auto incompleteCurvature = makePlanarCurveFixture(8);
    for (auto& vertex : incompleteCurvature.mesh.vertices) {
        vertex.sources.front().curvatureDefined = false;
    }
    assert(classify(incompleteCurvature, g2Options) ==
        spo::RegionCurveContinuity::G1);

    auto incompatibleCurvature = makePlanarCurveFixture(8);
    for (auto& vertex : incompatibleCurvature.mesh.vertices) {
        auto second = vertex.sources.front();
        second.faceId = 2;
        second.minimumCurvature += 1.0;
        second.maximumCurvature += 1.0;
        vertex.sources.push_back(second);
    }
    assert(classify(incompatibleCurvature, g2Options) ==
        spo::RegionCurveContinuity::G1);
}

void test_curve_network_freezes_one_shared_internal_curve_before_layout_binding() {
    auto fixture = makePlanarCurveFixture(10);
    spo::RegionRefitOptions options;
    options.fixedPatchCount = 2;
    options.maximumParameterDistortion = 1000.0;
    options.curveProjectionTolerance = 0.01;
    const auto generated = spo::RegionCurveNetworkGenerator().generate(
        fixture.mesh,
        fixture.constraints,
        spo::RegionRefitLayoutMode::FixedPatchCount,
        options);

    assert(generated.success());
    assert(generated.selectedCandidates.size() == 1);
    const auto network = generated.selectedCandidates.front().network;
    assert(network.valid());
    assert(network.report.internalEdgeCount == 1);
    assert(network.report.boundaryEdgeCount >= 4);
    assert(network.patchLoops.size() == 2);
    assert(network.report.ownershipPassed);
    assert(network.report.closurePassed);
    assert(network.report.projectionPassed);
    const auto internal = std::find_if(
        network.edges.begin(),
        network.edges.end(),
        [](const auto& edge) { return !edge.originalBoundary; });
    assert(internal != network.edges.end());
    const auto parameter =
        (internal->firstParameter + internal->lastParameter) * 0.5;
    const auto frozenPoint = internal->curve->Value(parameter);
    for (auto& vertex : fixture.mesh.vertices) {
        if (!vertex.boundary) vertex.point.SetZ(0.5);
    }
    assert(internal->curve->Value(parameter).Distance(frozenPoint) <= 1.0e-12);

    const auto layouts = spo::RegionPatchLayoutGenerator().generate(
        fixture.mesh, generated, options);
    assert(layouts.success());
    assert(layouts.selectedCandidates.front().curveNetworkHash ==
        network.hash);
    assert(layouts.selectedCandidates.front().hash !=
        network.partitionLayoutHash);
}

void test_curve_network_rejects_open_misoriented_and_multiple_use_loops() {
    const auto fixture = makePlanarCurveFixture(8);
    spo::RegionRefitOptions options;
    options.fixedPatchCount = 2;
    options.maximumParameterDistortion = 1000.0;
    options.curveProjectionTolerance = 0.01;
    const auto generated = spo::RegionCurveNetworkGenerator().generate(
        fixture.mesh,
        fixture.constraints,
        spo::RegionRefitLayoutMode::FixedPatchCount,
        options);
    assert(generated.success());

    auto open = generated.selectedCandidates.front().network;
    open.patchLoops.front().edges.pop_back();
    std::string message;
    assert(!spo::validateRegionCurveNetwork(open, 0.01, 97, message));
    assert(!open.report.closurePassed);

    auto misoriented = generated.selectedCandidates.front().network;
    misoriented.patchLoops.front().edges.front().forward =
        !misoriented.patchLoops.front().edges.front().forward;
    message.clear();
    assert(!spo::validateRegionCurveNetwork(
        misoriented, 0.01, 97, message));
    assert(!misoriented.report.closurePassed);

    auto multipleUse = generated.selectedCandidates.front().network;
    multipleUse.patchLoops.front().edges.push_back(
        multipleUse.patchLoops.front().edges.front());
    message.clear();
    assert(!spo::validateRegionCurveNetwork(
        multipleUse, 0.01, 97, message));
    assert(!multipleUse.report.ownershipPassed ||
        !multipleUse.report.closurePassed);
}

void test_four_patch_curve_network_keeps_closed_oriented_patch_loops() {
    const auto fixture = makePlanarCurveFixture(12);
    spo::RegionRefitOptions options;
    options.fixedPatchCount = 4;
    options.maximumParameterDistortion = 1000.0;
    options.curveProjectionTolerance = 0.01;
    const auto generated = spo::RegionCurveNetworkGenerator().generate(
        fixture.mesh,
        fixture.constraints,
        spo::RegionRefitLayoutMode::FixedPatchCount,
        options);

    assert(generated.success());
    const auto& network = generated.selectedCandidates.front().network;
    assert(network.patchLoops.size() == 4);
    assert(network.report.closurePassed);
    assert(network.report.ownershipPassed);
    assert(std::all_of(
        network.patchLoops.begin(),
        network.patchLoops.end(),
        [](const auto& loop) { return loop.edges.size() >= 2; }));
    assert(std::any_of(
        network.nodes.begin(),
        network.nodes.end(),
        [](const auto& node) { return node.incidentEdges.size() >= 3; }));
}

void test_curve_network_drives_hard_g0_fit_and_one_shared_brep_edge() {
    const auto fixture = makePlanarCurveFixture(10);
    spo::RegionRefitOptions options;
    options.fixedPatchCount = 2;
    options.maximumParameterDistortion = 1000.0;
    options.curveProjectionTolerance = 0.01;
    options.curveOnSurfaceTolerance = 0.05;
    options.tolerance3d = 0.05;
    options.normalToleranceDegrees = 1.0;
    options.g1ToleranceDegrees = 1.0;
    options.g2CurvatureTolerance = 0.01;
    options.enableInternalSeamG2 = true;
    options.maximumFitIterations = 6;
    const auto generated = spo::RegionCurveNetworkGenerator().generate(
        fixture.mesh,
        fixture.constraints,
        spo::RegionRefitLayoutMode::FixedPatchCount,
        options);
    assert(generated.success());
    const auto& curveNetwork =
        generated.selectedCandidates.front().network;
    const auto layouts = spo::RegionPatchLayoutGenerator().generate(
        fixture.mesh, generated, options);
    assert(layouts.success());
    const auto fitted = spo::JointBSplineNetworkFitter().fit(
        fixture.mesh,
        fixture.mesh,
        fixture.constraints,
        layouts.selectedCandidates,
        std::vector<spo::RegionCurveNetwork> {curveNetwork},
        options,
        std::chrono::steady_clock::now() + std::chrono::seconds(20));

    assert(fitted.success());
    assert(fitted.network);
    assert(fitted.network->layout.curveNetworkHash == curveNetwork.hash);
    assert(fitted.network->seamMaximumCurveDeviation <=
        options.curveOnSurfaceTolerance);
    assert(fitted.network->seamMaximumDistance <=
        options.curveOnSurfaceTolerance);
    assert(fitted.network->seamContinuityGatePassed);
    assert(!fitted.attempts.empty());
    assert(!fitted.attempts.front().iterations.empty());
    const auto& iteration = fitted.attempts.front().iterations.back();
    assert(iteration.seamG0ConstraintCount > 0);
    assert(iteration.seamG1SoftConstraintCount > 0);
    assert(iteration.seamG2SoftConstraintCount > 0);
    assert(iteration.seamContinuityGatePassed);

    const auto built = spo::NativeRegionPatchNetworkBuilder().build(
        fixture.mesh,
        fixture.constraints,
        *fitted.network,
        curveNetwork,
        options.curveOnSurfaceTolerance);
    assert(built.success());
    assert(built.internalSeamCount == 1);
    assert(built.internalSeamEdges.size() == 1);
    assert(built.sameParameterFailureCount == 0);
    const auto internal = std::find_if(
        built.edgeConsistency.begin(),
        built.edgeConsistency.end(),
        [](const auto& edge) { return edge.internalSeam; });
    assert(internal != built.edgeConsistency.end());
    assert(internal->sharedSameEdge);
    assert(internal->actualFaceUseCount == 2);
    assert(internal->sameRange);
    assert(internal->sameParameter);
    assert(internal->curveOnSurfaceMaximumDeviation <=
        options.curveOnSurfaceTolerance);
}

void test_curve_network_cache_roundtrip_is_deterministic() {
    const auto fixture = makePlanarCurveFixture(8);
    const auto token = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto cacheDirectory = std::filesystem::temp_directory_path() /
        ("spo-region-curve-network-cache-" + token);
    spo::RegionRefitRequest request;
    request.selectedFaces = {1};
    request.layoutMode = spo::RegionRefitLayoutMode::FixedPatchCount;
    request.options.fixedPatchCount = 2;
    request.options.maximumParameterDistortion = 1000.0;
    request.options.curveProjectionTolerance = 0.01;
    request.options.inputStepSha256 = std::string(64, 'b');
    request.options.algorithmFingerprint =
        "curve-network-cache-determinism-test";
    request.options.cachePolicy = spo::RegionRefitCachePolicy::ReadWrite;
    request.options.cacheDirectory = cacheDirectory;
    const auto first = spo::RegionCurveNetworkGenerator().generate(
        fixture.mesh,
        fixture.constraints,
        request.layoutMode,
        request.options);
    const auto second = spo::RegionCurveNetworkGenerator().generate(
        fixture.mesh,
        fixture.constraints,
        request.layoutMode,
        request.options);
    assert(first.success());
    assert(second.success());
    assert(first.selectedCandidates.front().network.hash ==
        second.selectedCandidates.front().network.hash);

    const spo::RegionalSurfaceRefitCache cache(request);
    const auto written = cache.saveCurveNetworks(first);
    assert(written.writeMilliseconds >= 0.0);
    spo::RegionCurveNetworkGenerationResult loaded;
    spo::RegionRefitCacheEvent event;
    assert(cache.loadCurveNetworks(fixture.constraints, loaded, event));
    assert(event.hit);
    assert(loaded.success());
    assert(loaded.selectedCandidates.front().network.hash ==
        first.selectedCandidates.front().network.hash);
    assert(loaded.selectedCandidates.front().network.valid());
    std::filesystem::remove_all(cacheDirectory);
}

}

void run_region_refit_layout_tests() {
    test_fixed_layout_has_exact_patch_count_and_trimmed_disk_parameterizations();
    test_trimmed_disk_accepts_more_than_four_boundary_turns_without_uv_flips();
    test_target_layout_uses_requested_first_then_alternating_deviation_order();
    test_adaptive_layout_is_deterministic_and_screens_at_most_three();
    test_forced_diagnostic_layout_selection_is_bounded_and_deterministic();
    test_layout_rejects_disconnected_reference_mesh();
    test_curve_network_uses_global_nearest_selected_face_projection();
    test_curve_network_accepts_near_preferred_face_projection();
    test_curve_network_classifies_internal_continuity_from_evidence();
    test_curve_network_freezes_one_shared_internal_curve_before_layout_binding();
    test_curve_network_rejects_open_misoriented_and_multiple_use_loops();
    test_four_patch_curve_network_keeps_closed_oriented_patch_loops();
    test_curve_network_drives_hard_g0_fit_and_one_shared_brep_edge();
    test_curve_network_cache_roundtrip_is_deterministic();
}
