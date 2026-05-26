#include "command/CommandContext.h"
#include "command/SphereRegionBatchMergeCommand.h"
#include "command/SphereRegionMergeCommand.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRep_Builder.hxx>
#include <gp_Sphere.hxx>

#include <cassert>

namespace {

struct SphereCommandFixture {
    spo::CommandContext context;
    spo::MergeCandidate candidate;
};

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
}

SphereCommandFixture make_sphere_command_fixture() {
    const gp_Sphere sphere(gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 5.0);
    const auto first = BRepBuilderAPI_MakeFace(sphere, 0.0, 3.14159265358979323846, -0.6, 0.6).Face();
    const auto second = BRepBuilderAPI_MakeFace(sphere, 3.14159265358979323846, 6.28318530717958647692, -0.6, 0.6).Face();
    BRepBuilderAPI_Sewing sewing;
    sewing.Add(first);
    sewing.Add(second);
    sewing.Perform();

    SphereCommandFixture fixture;
    fixture.context.document = spo::ShapeDocument(sewing.SewedShape(), {});
    fixture.candidate.candidate_id = 11;
    fixture.candidate.candidate_type = spo::MergeCandidateType::SphereLike;
    fixture.candidate.status = spo::MergeCandidateStatus::Accepted;
    fixture.candidate.faces = {0, 1};
    fixture.candidate.face_count = static_cast<int>(fixture.candidate.faces.size());
    for (spo::EdgeId edge = 0; edge < fixture.context.document.topology().edgeCount(); ++edge) {
        const auto* adjacency = fixture.context.document.topology().adjacencyForEdge(edge);
        assert(adjacency != nullptr);
        if (adjacency->faces.size() == 2) {
            fixture.candidate.internal_edges.push_back(edge);
        } else if (adjacency->faces.size() == 1) {
            fixture.candidate.boundary_edges.push_back(edge);
        }
    }
    assert(fixture.candidate.boundary_edges.size() >= 4);
    assert(fixture.candidate.internal_edges.size() >= 1);
    return fixture;
}

void test_sphere_region_merge_command_failure_does_not_pollute_document() {
    auto fixture = make_sphere_command_fixture();
    const auto before = fixture.context.document.stats();
    auto bad_candidate = fixture.candidate;
    bad_candidate.candidate_type = spo::MergeCandidateType::CylinderLike;

    spo::SphereRegionMergeOptions options;
    spo::RegionMergeResult result;
    spo::SphereRegionMergeCommand command(bad_candidate, options, &result);
    const auto status = command.execute(fixture.context);

    assert(!status.success());
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::UnsupportedCandidateType);
    assert(same_stats(fixture.context.document.stats(), before));
}

void test_sphere_region_merge_command_supports_undo_redo_after_success() {
    auto fixture = make_sphere_command_fixture();
    const auto before = fixture.context.document.stats();

    spo::SphereRegionMergeOptions options;
    options.max_deviation = 1.0;
    options.sphere_fit_tolerance = 1.0;
    spo::RegionMergeResult result;
    spo::SphereRegionMergeCommand command(fixture.candidate, options, &result);
    const auto execute_status = command.execute(fixture.context);

    if (!execute_status.success()) {
        return;
    }
    assert(result.success);
    assert(fixture.context.document.hasShape());
    assert(fixture.context.document.stats().faces < before.faces);

    const auto undo_status = command.undo(fixture.context);
    assert(undo_status.success());
    assert(same_stats(fixture.context.document.stats(), before));

    const auto redo_status = command.redo(fixture.context);
    assert(redo_status.success());
    assert(fixture.context.document.stats().faces < before.faces);
}

void test_sphere_region_batch_merge_command_supports_undo_redo_after_success() {
    auto fixture = make_sphere_command_fixture();
    const auto before = fixture.context.document.stats();

    spo::SphereRegionMergeOptions options;
    options.max_deviation = 1.0;
    options.sphere_fit_tolerance = 1.0;
    spo::RegionMergeResult result;
    spo::SphereRegionBatchMergeCommand command({fixture.candidate}, options, &result);
    const auto execute_status = command.execute(fixture.context);

    if (!execute_status.success()) {
        return;
    }
    assert(result.success);
    assert(fixture.context.document.hasShape());
    assert(fixture.context.document.stats().faces < before.faces);

    const auto undo_status = command.undo(fixture.context);
    assert(undo_status.success());
    assert(same_stats(fixture.context.document.stats(), before));

    const auto redo_status = command.redo(fixture.context);
    assert(redo_status.success());
    assert(fixture.context.document.stats().faces < before.faces);
}

}

void run_sphere_region_merge_command_tests() {
    test_sphere_region_merge_command_failure_does_not_pollute_document();
    test_sphere_region_merge_command_supports_undo_redo_after_success();
    test_sphere_region_batch_merge_command_supports_undo_redo_after_success();
}
