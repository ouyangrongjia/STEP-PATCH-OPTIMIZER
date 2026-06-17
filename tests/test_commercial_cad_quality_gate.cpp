#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "validate/CommercialCadQualityGate.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <string>

namespace {

struct SquareFixture {
    spo::ShapeDocument document;
    spo::MergeCandidate candidate;
    spo::RegionBoundaryAnalysis boundary;
    TopoDS_Face sourceFace;
};

TopoDS_Face make_square_face(double halfSize, double z = 0.0) {
    const gp_Pnt p0(-halfSize, -halfSize, z);
    const gp_Pnt p1(halfSize, -halfSize, z);
    const gp_Pnt p2(halfSize, halfSize, z);
    const gp_Pnt p3(-halfSize, halfSize, z);

    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p0, p1).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(p3, p0).Edge());
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}

SquareFixture make_square_fixture() {
    SquareFixture fixture;
    fixture.sourceFace = make_square_face(5.0);
    fixture.document = spo::ShapeDocument(fixture.sourceFace, {});

    fixture.candidate.candidate_id = 7;
    fixture.candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    fixture.candidate.face_count = static_cast<int>(fixture.document.topology().faceCount());
    for (std::size_t face = 0; face < fixture.document.topology().faceCount(); ++face) {
        fixture.candidate.faces.push_back(face);
    }
    for (std::size_t edge = 0; edge < fixture.document.topology().edgeCount(); ++edge) {
        fixture.candidate.boundary_edges.push_back(edge);
    }

    fixture.boundary = spo::RegionBoundaryAnalyzer().analyze(fixture.document, fixture.candidate);
    assert(fixture.boundary.valid);
    return fixture;
}

spo::CommercialCadQualityGateReport evaluate(
    const SquareFixture& fixture,
    const TopoDS_Shape& patchShape,
    double tolerance = 0.05) {
    spo::CommercialCadQualityGateOptions options;
    options.boundarySamplesPerEdge = 12;
    options.featureEdgeSamplesPerEdge = 12;
    options.maxBoundaryDistance = tolerance;
    options.maxCornerAnchorDistance = tolerance;
    options.maxFeatureEdgeDistance = tolerance;

    spo::CommercialCadQualityGateInput input;
    input.document = &fixture.document;
    input.candidate = &fixture.candidate;
    input.boundary = &fixture.boundary;
    input.patchShape = &patchShape;
    input.options = options;
    return spo::CommercialCadQualityGate().evaluate(input);
}

void test_matching_patch_passes_dense_boundary_gate() {
    const auto fixture = make_square_fixture();
    const auto report = evaluate(fixture, fixture.sourceFace);

    assert(report.evaluated);
    assert(report.passed);
    assert(report.boundary.samples > 0);
    assert(report.boundary.maxDistance <= 1.0e-7);
    assert(report.seamContinuity.evaluated);
    assert(report.seamContinuity.samples == 0);
    assert(report.cornerAnchors.samples == 4);
    assert(report.cornerAnchors.maxDistance <= 1.0e-7);
    assert(report.sharpCornerPreservationPassed);
}

void test_inset_patch_fails_boundary_and_corner_drift() {
    const auto fixture = make_square_fixture();
    const auto insetPatch = make_square_face(4.0);
    const auto report = evaluate(fixture, insetPatch, 0.05);

    assert(report.evaluated);
    assert(!report.passed);
    assert(report.boundary.maxDistance > 0.5);
    assert(report.cornerAnchors.maxDistance > 1.0);
    assert(!report.sharpCornerPreservationPassed);
    assert(report.message.find("CommercialCadLikeQualityGate failed") != std::string::npos);
}

void test_report_json_contains_machine_readable_metrics() {
    const auto fixture = make_square_fixture();
    const auto report = evaluate(fixture, fixture.sourceFace);
    const auto json = spo::toJson(report);

    assert(json.find("\"commercial_cad_like_quality_gate\"") != std::string::npos);
    assert(json.find("\"sampling_report\"") != std::string::npos);
    assert(json.find("\"corner_anchor_source\"") != std::string::npos);
    assert(json.find("\"original_boundary_edge_endpoints\"") != std::string::npos);
    assert(json.find("\"boundary_samples_per_edge\"") != std::string::npos);
    assert(json.find("\"feature_edge_samples_per_edge\"") != std::string::npos);
    assert(json.find("\"boundary\"") != std::string::npos);
    assert(json.find("\"seam_continuity\"") != std::string::npos);
    assert(json.find("\"corner_anchors\"") != std::string::npos);
    assert(json.find("\"sharp_corner_preservation_passed\"") != std::string::npos);
}

}

void run_commercial_cad_quality_gate_tests() {
    test_matching_patch_passes_dense_boundary_gate();
    test_inset_patch_fails_boundary_and_corner_drift();
    test_report_json_contains_machine_readable_metrics();
}
