#include "command/CommandContext.h"
#include "command/PlaneRegionBatchMergeCommand.h"
#include "command/PlaneRegionMergeCommand.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <filesystem>
#include <fstream>

namespace {

struct CommandFixture {
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

CommandFixture make_command_fixture() {
    const gp_Pnt p00(0.0, 0.0, 0.0);
    const gp_Pnt p10(1.0, 0.0, 0.0);
    const gp_Pnt p20(2.0, 0.0, 0.0);
    const gp_Pnt p01(0.0, 1.0, 0.0);
    const gp_Pnt p11(1.0, 1.0, 0.0);
    const gp_Pnt p21(2.0, 1.0, 0.0);

    const auto bottom_left = BRepBuilderAPI_MakeEdge(p00, p10).Edge();
    const auto shared = BRepBuilderAPI_MakeEdge(p10, p11).Edge();
    const auto top_left = BRepBuilderAPI_MakeEdge(p11, p01).Edge();
    const auto left = BRepBuilderAPI_MakeEdge(p01, p00).Edge();
    const auto bottom_right = BRepBuilderAPI_MakeEdge(p10, p20).Edge();
    const auto right = BRepBuilderAPI_MakeEdge(p20, p21).Edge();
    const auto top_right = BRepBuilderAPI_MakeEdge(p21, p11).Edge();

    BRepBuilderAPI_MakeWire left_wire;
    left_wire.Add(bottom_left);
    left_wire.Add(shared);
    left_wire.Add(top_left);
    left_wire.Add(left);
    const auto left_face = BRepBuilderAPI_MakeFace(left_wire.Wire()).Face();

    BRepBuilderAPI_MakeWire right_wire;
    right_wire.Add(bottom_right);
    right_wire.Add(right);
    right_wire.Add(top_right);
    right_wire.Add(TopoDS::Edge(shared.Reversed()));
    const auto right_face = BRepBuilderAPI_MakeFace(right_wire.Wire()).Face();

    BRepBuilderAPI_Sewing sewing;
    sewing.Add(left_face);
    sewing.Add(right_face);
    sewing.Perform();

    CommandFixture fixture;
    fixture.context.document = spo::ShapeDocument(sewing.SewedShape(), {});
    fixture.candidate.candidate_id = 8;
    fixture.candidate.candidate_type = spo::MergeCandidateType::PlaneLike;
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
    assert(fixture.candidate.boundary_edges.size() == 6);
    assert(fixture.candidate.internal_edges.size() == 1);
    return fixture;
}

void test_plane_region_merge_command_failure_does_not_pollute_document() {
    auto fixture = make_command_fixture();
    const auto before = fixture.context.document.stats();
    auto bad_candidate = fixture.candidate;
    bad_candidate.candidate_type = spo::MergeCandidateType::CylinderLike;

    spo::PlaneRegionMergeOptions options;
    spo::RegionMergeResult result;
    spo::PlaneRegionMergeCommand command(bad_candidate, options, &result);
    const auto status = command.execute(fixture.context);

    assert(!status.success());
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::UnsupportedCandidateType);
    assert(same_stats(fixture.context.document.stats(), before));
}

void test_plane_region_merge_command_roundtrip_failure_does_not_pollute_document() {
    auto fixture = make_command_fixture();
    const auto before = fixture.context.document.stats();
    auto tempFile = std::filesystem::temp_directory_path();
    tempFile /= "step-patch-optimizer-command-roundtrip-not-a-directory.tmp";
    {
        std::ofstream stream(tempFile.string());
        stream << "not a directory";
    }

    spo::PlaneRegionMergeOptions options;
    options.roundtrip_temp_directory = tempFile;
    spo::RegionMergeResult result;
    spo::PlaneRegionMergeCommand command(fixture.candidate, options, &result);
    const auto status = command.execute(fixture.context);

    std::error_code ignored;
    std::filesystem::remove(tempFile, ignored);
    assert(!status.success());
    assert(!result.success);
    assert(result.failure_reason == spo::RegionMergeFailureReason::ExportRoundtripFailed);
    assert(same_stats(fixture.context.document.stats(), before));
}

void test_plane_region_merge_command_supports_undo_redo_after_success() {
    auto fixture = make_command_fixture();
    const auto before = fixture.context.document.stats();

    spo::PlaneRegionMergeOptions options;
    spo::RegionMergeResult result;
    spo::PlaneRegionMergeCommand command(fixture.candidate, options, &result);
    const auto execute_status = command.execute(fixture.context);

    assert(execute_status.success());
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

void test_plane_region_batch_merge_command_supports_undo_redo_after_success() {
    auto fixture = make_command_fixture();
    const auto before = fixture.context.document.stats();

    spo::PlaneRegionMergeOptions options;
    spo::RegionMergeResult result;
    spo::PlaneRegionBatchMergeCommand command({fixture.candidate}, options, &result);
    const auto execute_status = command.execute(fixture.context);

    assert(execute_status.success());
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

void run_plane_region_merge_command_tests() {
    test_plane_region_merge_command_failure_does_not_pollute_document();
    test_plane_region_merge_command_roundtrip_failure_does_not_pollute_document();
    test_plane_region_merge_command_supports_undo_redo_after_success();
    test_plane_region_batch_merge_command_supports_undo_redo_after_success();
}
