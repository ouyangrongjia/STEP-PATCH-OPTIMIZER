#include "brep/ShapeDocument.h"
#include "reduction/region_refit/RegionConstraintExtractor.h"
#include "reduction/region_refit/StepAdaptiveRegionSampler.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <Geom_SphericalSurface.hxx>
#include <TopoDS.hxx>
#include <gp.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cassert>
#include <chrono>

namespace {

spo::ShapeDocument make_sampling_two_face_fixture() {
    const gp_Pnt p00(0.0, 0.0, 0.0);
    const gp_Pnt p10(1.0, 0.0, 0.0);
    const gp_Pnt p20(2.0, 0.0, 0.0);
    const gp_Pnt p01(0.0, 1.0, 0.0);
    const gp_Pnt p11(1.0, 1.0, 0.0);
    const gp_Pnt p21(2.0, 1.0, 0.0);

    const auto bottomLeft = BRepBuilderAPI_MakeEdge(p00, p10).Edge();
    const auto shared = BRepBuilderAPI_MakeEdge(p10, p11).Edge();
    const auto topLeft = BRepBuilderAPI_MakeEdge(p11, p01).Edge();
    const auto left = BRepBuilderAPI_MakeEdge(p01, p00).Edge();
    const auto bottomRight = BRepBuilderAPI_MakeEdge(p10, p20).Edge();
    const auto right = BRepBuilderAPI_MakeEdge(p20, p21).Edge();
    const auto topRight = BRepBuilderAPI_MakeEdge(p21, p11).Edge();

    BRepBuilderAPI_MakeWire leftWire;
    leftWire.Add(bottomLeft);
    leftWire.Add(shared);
    leftWire.Add(topLeft);
    leftWire.Add(left);
    BRepBuilderAPI_MakeWire rightWire;
    rightWire.Add(bottomRight);
    rightWire.Add(right);
    rightWire.Add(topRight);
    rightWire.Add(TopoDS::Edge(shared.Reversed()));

    BRepBuilderAPI_Sewing sewing;
    sewing.Add(BRepBuilderAPI_MakeFace(leftWire.Wire()).Face());
    sewing.Add(BRepBuilderAPI_MakeFace(rightWire.Wire()).Face());
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

spo::ShapeDocument make_spherical_patch_fixture() {
    const Handle(Geom_SphericalSurface) sphere = new Geom_SphericalSurface(
        gp_Ax3(gp::Origin(), gp::DZ()),
        10.0);
    const auto face = BRepBuilderAPI_MakeFace(
        sphere,
        0.0,
        0.8,
        -0.4,
        0.4,
        1.0e-7).Face();
    return spo::ShapeDocument(face, {});
}

spo::RegionTopologyConstraints constraintsFor(
    const spo::ShapeDocument& document,
    std::vector<spo::FaceId> faces) {
    spo::RegionRefitRequest request;
    request.selectedFaces = std::move(faces);
    const auto extracted = spo::RegionConstraintExtractor().extract(document, request);
    assert(extracted.success());
    return extracted.constraints;
}

void test_adaptive_sampler_welds_internal_step_seam() {
    const auto document = make_sampling_two_face_fixture();
    const auto constraints = constraintsFor(document, {0, 1});
    spo::RegionRefitOptions options;

    const auto sampled = spo::StepAdaptiveRegionSampler().sample(document, constraints, options);

    assert(sampled.report.success());
    assert(!sampled.mesh.empty());
    assert(sampled.report.internalSeamVertexCount > 0);
    assert(sampled.report.weldedInternalSeamVertexCount > 0);
    assert(sampled.report.weldedInternalSeamVertexCount == sampled.report.internalSeamVertexCount);
    assert(sampled.mesh.toStlMesh().triangleCount() == sampled.mesh.triangles.size());
}

void test_adaptive_sampler_forces_exact_outer_boundary_samples() {
    const auto document = make_spherical_patch_fixture();
    const auto constraints = constraintsFor(document, {0});
    spo::RegionRefitOptions options;

    const auto sampled = spo::StepAdaptiveRegionSampler().sample(document, constraints, options);

    assert(sampled.report.success());
    assert(sampled.report.boundarySampleCount >= constraints.boundaryEdges.size() * 2);
    assert(sampled.report.boundaryEndpointCount >= constraints.boundaryEdges.size() * 2);
    for (const auto& sample : sampled.mesh.boundarySamples) {
        const auto found = std::find_if(
            constraints.boundaryEdges.begin(),
            constraints.boundaryEdges.end(),
            [&](const auto& edge) { return edge.edgeId == sample.edgeId; });
        assert(found != constraints.boundaryEdges.end());
        const auto exact = BRepAdaptor_Curve(found->originalEdge).Value(sample.parameter);
        assert(exact.Distance(sampled.mesh.vertices[sample.vertexIndex].point) < 1.0e-8);
        assert(sampled.mesh.vertices[sample.vertexIndex].hardBoundary);
    }
    assert(sampled.report.maximumBoundaryChordError <= sampled.report.boundaryChordTolerance * 1.01);
    assert(sampled.report.maximumInteriorChordError <= sampled.report.linearDeflection * 1.5);
    assert(sampled.report.maximumInteriorNormalErrorDegrees <=
        sampled.report.angularDeflectionDegrees * 1.5);
}

void test_adaptive_sampler_is_deterministic() {
    const auto document = make_spherical_patch_fixture();
    const auto constraints = constraintsFor(document, {0});
    spo::RegionRefitOptions options;

    const auto first = spo::StepAdaptiveRegionSampler().sample(document, constraints, options);
    const auto second = spo::StepAdaptiveRegionSampler().sample(document, constraints, options);

    assert(first.report.success());
    assert(second.report.success());
    assert(first.report.meshFingerprint == second.report.meshFingerprint);
    assert(first.report.vertexCount == second.report.vertexCount);
    assert(first.report.triangleCount == second.report.triangleCount);
}

void test_adaptive_sampler_honors_triangle_budget() {
    const auto document = make_spherical_patch_fixture();
    const auto constraints = constraintsFor(document, {0});
    spo::RegionRefitOptions options;
    options.maximumSampleTriangles = 1;

    const auto sampled = spo::StepAdaptiveRegionSampler().sample(document, constraints, options);

    assert(!sampled.report.success());
    assert(sampled.report.status == spo::RegionRefitStatus::SamplingBudgetExceeded);
    assert(sampled.mesh.empty());
}

void test_quality_balanced_sampler_is_deterministic_and_structurally_valid() {
    const auto document = make_spherical_patch_fixture();
    const auto constraints = constraintsFor(document, {0});
    spo::RegionRefitOptions options;
    options.samplingMode = spo::RegionRefitSamplingMode::QualityBalanced;
    options.targetSampleTriangles = 800;
    options.maximumSampleTriangles = 5000;
    options.maximumRemeshSweeps = 3;

    const auto first = spo::StepAdaptiveRegionSampler().sample(document, constraints, options);
    const auto second = spo::StepAdaptiveRegionSampler().sample(document, constraints, options);

    assert(first.report.success());
    assert(second.report.success());
    assert(first.report.meshFingerprint == second.report.meshFingerprint);
    assert(first.report.zeroAreaTriangleCount == 0);
    assert(first.report.nonManifoldEdgeCount == 0);
    assert(first.report.edgeLengthP50 > 0.0);
    assert(first.report.minimumAngleP05Degrees > 0.0);
    assert(first.report.aspectRatioP95 >= 1.0);
    assert(first.report.normalizedQualityP05 > 0.0);
    assert(std::any_of(
        first.mesh.vertices.begin(),
        first.mesh.vertices.end(),
        [](const auto& vertex) { return vertex.areaWeight > 0.0; }));
    for (const auto& sample : first.mesh.boundarySamples) {
        const auto found = std::find_if(
            constraints.boundaryEdges.begin(),
            constraints.boundaryEdges.end(),
            [&](const auto& edge) { return edge.edgeId == sample.edgeId; });
        assert(found != constraints.boundaryEdges.end());
        const auto exact = BRepAdaptor_Curve(found->originalEdge).Value(sample.parameter);
        assert(exact.Distance(first.mesh.vertices[sample.vertexIndex].point) < 1.0e-8);
    }
}

void test_quality_balanced_sampler_honors_expired_deadline() {
    const auto document = make_spherical_patch_fixture();
    const auto constraints = constraintsFor(document, {0});
    spo::RegionRefitOptions options;
    options.samplingMode = spo::RegionRefitSamplingMode::QualityBalanced;

    const auto sampled = spo::StepAdaptiveRegionSampler().sample(
        document,
        constraints,
        options,
        std::chrono::steady_clock::now() - std::chrono::milliseconds(1));

    assert(!sampled.report.success());
    assert(sampled.report.status == spo::RegionRefitStatus::TimeBudgetExceeded);
    assert(sampled.mesh.empty());
}

void test_uniform_uv_sampler_is_deterministic_and_preserves_step_edges() {
    const auto document = make_sampling_two_face_fixture();
    const auto constraints = constraintsFor(document, {0, 1});
    spo::RegionRefitOptions options;
    options.samplingMode = spo::RegionRefitSamplingMode::UniformUv;
    options.targetSampleTriangles = 800;
    options.maximumSampleTriangles = 10000;

    const auto first = spo::StepAdaptiveRegionSampler().sample(document, constraints, options);
    const auto second = spo::StepAdaptiveRegionSampler().sample(document, constraints, options);

    assert(first.report.success());
    assert(second.report.success());
    assert(first.report.meshFingerprint == second.report.meshFingerprint);
    assert(first.report.remeshSplitCount > 0);
    assert(first.report.weldedInternalSeamVertexCount == first.report.internalSeamVertexCount);
    assert(first.report.zeroAreaTriangleCount == 0);
    assert(first.report.nonManifoldEdgeCount == 0);
    assert(first.report.edgeLengthP50 > 0.0);
    assert(std::any_of(
        first.mesh.vertices.begin(),
        first.mesh.vertices.end(),
        [](const auto& vertex) { return vertex.areaWeight > 0.0; }));
    for (const auto& sample : first.mesh.boundarySamples) {
        const auto found = std::find_if(
            constraints.boundaryEdges.begin(),
            constraints.boundaryEdges.end(),
            [&](const auto& edge) { return edge.edgeId == sample.edgeId; });
        assert(found != constraints.boundaryEdges.end());
        const auto exact = BRepAdaptor_Curve(found->originalEdge).Value(sample.parameter);
        assert(exact.Distance(first.mesh.vertices[sample.vertexIndex].point) < 1.0e-8);
    }
}

}

void run_region_refit_sampling_tests() {
    test_adaptive_sampler_welds_internal_step_seam();
    test_adaptive_sampler_forces_exact_outer_boundary_samples();
    test_adaptive_sampler_is_deterministic();
    test_adaptive_sampler_honors_triangle_budget();
    test_quality_balanced_sampler_is_deterministic_and_structurally_valid();
    test_quality_balanced_sampler_honors_expired_deadline();
    test_uniform_uv_sampler_is_deterministic_and_preserves_step_edges();
}
