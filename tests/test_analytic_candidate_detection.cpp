#include "brep/ShapeDocument.h"
#include "merge/MergePlanner.h"
#include "merge/MergeRegionGrower.h"
#include "merge/PlaneRegionMerger.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Sphere.hxx>

#include <algorithm>
#include <cassert>
#include <set>
#include <vector>

namespace {

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
}

spo::ShapeDocument make_split_cylinder_document() {
    const gp_Cylinder cylinder(gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 5.0);
    const auto first = BRepBuilderAPI_MakeFace(cylinder, 0.0, 3.14159265358979323846, 0.0, 10.0).Face();
    const auto second = BRepBuilderAPI_MakeFace(cylinder, 3.14159265358979323846, 6.28318530717958647692, 0.0, 10.0).Face();
    BRepBuilderAPI_Sewing sewing;
    sewing.Add(first);
    sewing.Add(second);
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

spo::ShapeDocument make_split_nurbs_cylinder_document() {
    BRepBuilderAPI_NurbsConvert converter(make_split_cylinder_document().shape());
    return spo::ShapeDocument(converter.Shape(), "split-nurbs-cylinder");
}

spo::ShapeDocument make_split_plane_document() {
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
    const auto leftFace = BRepBuilderAPI_MakeFace(leftWire.Wire()).Face();

    BRepBuilderAPI_MakeWire rightWire;
    rightWire.Add(bottomRight);
    rightWire.Add(right);
    rightWire.Add(topRight);
    rightWire.Add(TopoDS::Edge(shared.Reversed()));
    const auto rightFace = BRepBuilderAPI_MakeFace(rightWire.Wire()).Face();

    BRepBuilderAPI_Sewing sewing;
    sewing.Add(leftFace);
    sewing.Add(rightFace);
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

spo::ShapeDocument make_split_sphere_document() {
    const gp_Sphere sphere(gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 5.0);
    const auto first = BRepBuilderAPI_MakeFace(sphere, 0.0, 3.14159265358979323846, -0.6, 0.6).Face();
    const auto second = BRepBuilderAPI_MakeFace(sphere, 3.14159265358979323846, 6.28318530717958647692, -0.6, 0.6).Face();
    BRepBuilderAPI_Sewing sewing;
    sewing.Add(first);
    sewing.Add(second);
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

spo::ShapeDocument make_split_cone_document() {
    const gp_Cone cone(gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 0.25, 2.0);
    const auto first = BRepBuilderAPI_MakeFace(cone, 0.0, 3.14159265358979323846, 2.0, 8.0).Face();
    const auto second = BRepBuilderAPI_MakeFace(cone, 3.14159265358979323846, 6.28318530717958647692, 2.0, 8.0).Face();
    BRepBuilderAPI_Sewing sewing;
    sewing.Add(first);
    sewing.Add(second);
    sewing.Perform();
    return spo::ShapeDocument(sewing.SewedShape(), {});
}

spo::ShapeDocument make_compound_document(const std::vector<spo::ShapeDocument>& documents) {
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& document : documents) {
        builder.Add(compound, document.shape());
    }
    return spo::ShapeDocument(compound, {});
}

std::vector<spo::MergeCandidate> candidates_of_type(
    const std::vector<spo::MergeCandidate>& candidates,
    spo::MergeCandidateType type) {
    std::vector<spo::MergeCandidate> result;
    for (const auto& candidate : candidates) {
        if (candidate.candidate_type == type) {
            result.push_back(candidate);
        }
    }
    return result;
}

spo::MergePlannerResult plan_candidates(const spo::ShapeDocument& document, const spo::MergePlannerOptions& options) {
    spo::MergePlanner planner;
    return planner.plan(document, {}, {}, options);
}

void test_cylinder_candidates_respect_enable_flag() {
    const auto document = make_split_cylinder_document();
    const auto before = document.stats();

    spo::MergePlannerOptions disabled;
    disabled.enable_plane_candidates = false;
    disabled.enable_cylinder_candidates = false;
    auto disabledResult = plan_candidates(document, disabled);
    assert(candidates_of_type(disabledResult.candidates, spo::MergeCandidateType::CylinderLike).empty());

    spo::MergePlannerOptions enabled;
    enabled.enable_plane_candidates = false;
    enabled.enable_cylinder_candidates = true;
    auto enabledResult = plan_candidates(document, enabled);
    const auto cylinderCandidates = candidates_of_type(enabledResult.candidates, spo::MergeCandidateType::CylinderLike);
    assert(!cylinderCandidates.empty());
    assert(cylinderCandidates.front().face_count >= 2);
    assert(same_stats(document.stats(), before));
}

void test_nurbs_backed_cylinder_candidates_are_generated() {
    const auto document = make_split_nurbs_cylinder_document();
    const auto before = document.stats();

    spo::MergePlannerOptions options;
    options.enable_plane_candidates = false;
    options.enable_cylinder_candidates = true;

    const auto result = plan_candidates(document, options);
    const auto cylinderCandidates = candidates_of_type(result.candidates, spo::MergeCandidateType::CylinderLike);

    assert(!cylinderCandidates.empty());
    assert(cylinderCandidates.front().face_count >= 2);
    assert(cylinderCandidates.front().fit_error <= 1.0);
    assert(same_stats(document.stats(), before));
}

void test_sphere_candidates_are_generated() {
    const auto document = make_split_sphere_document();
    spo::MergePlannerOptions options;
    options.enable_plane_candidates = false;
    options.enable_sphere_candidates = true;

    const auto result = plan_candidates(document, options);
    const auto sphereCandidates = candidates_of_type(result.candidates, spo::MergeCandidateType::SphereLike);

    assert(!sphereCandidates.empty());
    assert(sphereCandidates.front().face_count >= 2);
}

void test_cone_candidates_are_generated() {
    const auto document = make_split_cone_document();
    spo::MergePlannerOptions options;
    options.enable_plane_candidates = false;
    options.enable_cone_candidates = true;

    const auto result = plan_candidates(document, options);
    const auto coneCandidates = candidates_of_type(result.candidates, spo::MergeCandidateType::ConeLike);

    assert(!coneCandidates.empty());
    assert(coneCandidates.front().face_count >= 2);
}

void test_protected_edge_blocks_analytic_region_growth() {
    const auto document = make_split_cylinder_document();
    spo::MergePlannerOptions options;
    options.enable_plane_candidates = false;
    options.enable_cylinder_candidates = true;

    const auto result = plan_candidates(document, options);
    const auto cylinderCandidates = candidates_of_type(result.candidates, spo::MergeCandidateType::CylinderLike);
    assert(!cylinderCandidates.empty());
    assert(!cylinderCandidates.front().internal_edges.empty());

    spo::MergeRegionGrower grower;
    int visited = 0;
    int rejected = 0;
    const std::set<spo::EdgeId> protectedEdges {cylinderCandidates.front().internal_edges.front()};
    const auto blocked = grower.growCylinderLikeRegions(document, protectedEdges, options, &visited, &rejected);

    for (const auto& candidate : blocked) {
        assert(candidate.internal_edges.empty());
    }
}

void test_candidate_ids_are_global_and_continuous() {
    const auto document = make_compound_document({make_split_cylinder_document(), make_split_sphere_document()});
    spo::MergePlannerOptions options;
    options.enable_plane_candidates = false;
    options.enable_cylinder_candidates = true;
    options.enable_sphere_candidates = true;

    const auto result = plan_candidates(document, options);

    assert(result.candidates.size() >= 2);
    for (std::size_t index = 0; index < result.candidates.size(); ++index) {
        assert(result.candidates[index].candidate_id == static_cast<int>(index));
    }
    assert(!candidates_of_type(result.candidates, spo::MergeCandidateType::CylinderLike).empty());
    assert(!candidates_of_type(result.candidates, spo::MergeCandidateType::SphereLike).empty());
}

void test_plane_like_generation_does_not_regress() {
    const auto document = make_split_plane_document();
    spo::MergePlannerOptions options;
    options.enable_plane_candidates = true;

    const auto result = plan_candidates(document, options);

    assert(!candidates_of_type(result.candidates, spo::MergeCandidateType::PlaneLike).empty());
}

void test_plane_region_merger_still_rejects_non_plane_candidate() {
    const auto document = make_split_cylinder_document();
    spo::MergePlannerOptions plannerOptions;
    plannerOptions.enable_plane_candidates = false;
    plannerOptions.enable_cylinder_candidates = true;
    const auto result = plan_candidates(document, plannerOptions);
    const auto cylinderCandidates = candidates_of_type(result.candidates, spo::MergeCandidateType::CylinderLike);
    assert(!cylinderCandidates.empty());

    spo::PlaneRegionMerger merger;
    const auto mergeResult = merger.merge(document, cylinderCandidates.front(), spo::PlaneRegionMergeOptions {});

    assert(!mergeResult.success);
    assert(mergeResult.failure_reason == spo::RegionMergeFailureReason::UnsupportedCandidateType);
}

}

void run_analytic_candidate_detection_tests() {
    test_cylinder_candidates_respect_enable_flag();
    test_nurbs_backed_cylinder_candidates_are_generated();
    test_sphere_candidates_are_generated();
    test_cone_candidates_are_generated();
    test_protected_edge_blocks_analytic_region_growth();
    test_candidate_ids_are_global_and_continuous();
    test_plane_like_generation_does_not_regress();
    test_plane_region_merger_still_rejects_non_plane_candidate();
}
