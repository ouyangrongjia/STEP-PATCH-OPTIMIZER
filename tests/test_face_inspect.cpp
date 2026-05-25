#include "brep/ShapeDocument.h"
#include "brep/SurfaceTypeProbe.h"
#include "merge/FaceInspector.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <cassert>
#include <set>
#include <string>
#include <vector>

namespace {

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
}

spo::ShapeDocument make_box_document() {
    return spo::ShapeDocument(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(), {});
}

void test_surface_type_probe_reports_plane_and_unknown() {
    const auto document = make_box_document();
    assert(document.topology().faceCount() > 0);
    assert(spo::surfaceTypeName(document.topology().face(0)) == "Plane");

    const TopoDS_Face emptyFace;
    assert(spo::surfaceTypeName(emptyFace) == "Unknown");
}

void test_surface_type_probe_reports_cylinder() {
    const auto shape = BRepPrimAPI_MakeCylinder(5.0, 10.0).Shape();
    bool foundCylinder = false;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const auto typeName = spo::surfaceTypeName(TopoDS::Face(explorer.Current()));
        if (typeName == "Cylinder") {
            foundCylinder = true;
            break;
        }
    }
    assert(foundCylinder);
}

spo::MergeCandidate make_single_face_candidate(spo::FaceId faceId) {
    spo::MergeCandidate candidate;
    candidate.candidate_id = 12;
    candidate.candidate_type = spo::MergeCandidateType::PlaneLike;
    candidate.status = spo::MergeCandidateStatus::Pending;
    candidate.risk_level = spo::MergeRiskLevel::Medium;
    candidate.faces = {faceId};
    candidate.face_count = 1;
    candidate.boundary_edge_count = 4;
    candidate.internal_edge_count = 0;
    candidate.max_normal_angle_deg = 1.25;
    candidate.max_distance = 0.005;
    return candidate;
}

void test_face_inspect_reports_candidate_info() {
    const auto document = make_box_document();
    const auto before = document.stats();
    const auto candidate = make_single_face_candidate(0);
    const std::vector<spo::MergeCandidate> candidates {candidate};
    const std::set<int> visibleIds {candidate.candidate_id};

    const auto info = spo::inspectFace(document, 0, candidates, visibleIds, nullptr, {});

    assert(info.valid);
    assert(info.face_id == 0);
    assert(info.surface_type == "Plane");
    assert(info.candidate_state == spo::FaceInspectCandidateState::InVisibleCandidate);
    assert(info.candidate_id == candidate.candidate_id);
    assert(info.candidate_type == spo::MergeCandidateType::PlaneLike);
    assert(info.candidate_status == spo::MergeCandidateStatus::Pending);
    assert(info.risk_level == spo::MergeRiskLevel::Medium);
    assert(info.candidate_face_count == 1);
    assert(same_stats(document.stats(), before));
}

void test_face_inspect_reports_not_in_candidate() {
    const auto document = make_box_document();
    assert(document.topology().faceCount() > 1);
    const auto candidate = make_single_face_candidate(0);
    const std::vector<spo::MergeCandidate> candidates {candidate};

    const auto info = spo::inspectFace(document, 1, candidates, {}, nullptr, {});

    assert(info.valid);
    assert(info.candidate_state == spo::FaceInspectCandidateState::NotInCandidate);
    assert(info.candidate_id == -1);
}

void test_face_inspect_reports_hidden_candidate() {
    const auto document = make_box_document();
    auto candidate = make_single_face_candidate(0);
    candidate.status = spo::MergeCandidateStatus::Hidden;
    const std::vector<spo::MergeCandidate> candidates {candidate};

    const auto info = spo::inspectFace(document, 0, candidates, {}, nullptr, {});

    assert(info.valid);
    assert(info.candidate_state == spo::FaceInspectCandidateState::InHiddenCandidate);
    assert(info.candidate_id == candidate.candidate_id);
}

}

void run_face_inspect_tests() {
    test_surface_type_probe_reports_plane_and_unknown();
    test_surface_type_probe_reports_cylinder();
    test_face_inspect_reports_candidate_info();
    test_face_inspect_reports_not_in_candidate();
    test_face_inspect_reports_hidden_candidate();
}
