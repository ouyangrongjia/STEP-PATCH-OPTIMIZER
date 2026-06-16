#include "app/AppController.h"
#include "brep/ShapeDocument.h"
#include "command/Command.h"
#include "command/CommandContext.h"
#include "command/CommandHistory.h"
#include "command/DetectFeatureCommand.h"
#include "command/ExportStepCommand.h"
#include "command/LoadStepCommand.h"
#include "command/LockedEdgeRef.h"
#include "command/LockEdgeCommand.h"
#include "command/MergePatchCommand.h"
#include "command/UnlockEdgeCommand.h"
#include "command/ValidateShapeCommand.h"
#include "io/StlReader.h"
#include "io/StlWriter.h"
#include "io/StepWriter.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Surface.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace {

std::filesystem::path temp_step_path(const char* suffix) {
    return std::filesystem::temp_directory_path() /
        ("step-patch-optimizer-command-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + suffix);
}

std::filesystem::path create_test_step() {
    const auto path = temp_step_path(".stp");
    const auto shape = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    const spo::ShapeDocument document(shape, path);
    const spo::StepWriter writer;
    assert(writer.write(document, path).success());
    return path;
}

std::filesystem::path temp_root(const char* name) {
    const auto path = std::filesystem::temp_directory_path() /
        (std::string(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void write_text_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream);
    stream << content;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string trim_line(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
    return text;
}

std::filesystem::path write_mock_geomagic_cmd(const std::filesystem::path& root) {
    const auto path = root / "mock_geomagic.cmd";
    const auto cwdPath = (root / "mock_geomagic_cwd.txt").string();
    write_text_file(
        path,
        "@echo off\n"
        "echo mock geomagic success\n"
        "echo %CD%>\"" + cwdPath + "\"\n"
        "type nul > \"%FIT_REGION_OUTPUT%\"\n"
        "exit /b 0\n");
    return path;
}

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
}

class NonUndoableCommand final : public spo::Command {
public:
    const char* name() const override { return "NonUndoableCommand"; }
    spo::Result execute(spo::CommandContext&) override { return spo::Result::ok(); }
};

class ThrowingCommand final : public spo::Command {
public:
    const char* name() const override { return "ThrowingCommand"; }
    spo::Result execute(spo::CommandContext&) override { throw std::runtime_error("boom"); }
};

void load_context(spo::CommandContext& context, const std::filesystem::path& path) {
    spo::LoadStepCommand load(path);
    assert(load.execute(context).success());
    assert(context.document.hasShape());
}

std::vector<spo::LockedEdgeRef> locked_refs(std::initializer_list<spo::EdgeId> ids) {
    std::vector<spo::LockedEdgeRef> refs;
    for (const auto id : ids) {
        refs.push_back(spo::LockedEdgeRef {id, {}});
    }
    return refs;
}

spo::MergeCandidate make_first_face_candidate(const spo::ShapeDocument& document) {
    spo::MergeCandidate candidate;
    candidate.candidate_id = 3;
    candidate.candidate_type = spo::MergeCandidateType::FeatureBoundedRefit;
    candidate.faces = {0};
    candidate.face_count = 1;
    candidate.boundary_edges = document.topology().edgesForFace(0);
    candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());
    return candidate;
}

spo::StlTriangle make_triangle(
    spo::StlVec3 normal,
    spo::StlVec3 v0,
    spo::StlVec3 v1,
    spo::StlVec3 v2) {
    spo::StlTriangle triangle;
    triangle.normal = normal;
    triangle.v0 = v0;
    triangle.v1 = v1;
    triangle.v2 = v2;
    return triangle;
}

spo::StlVec3 to_vec3(const gp_Pnt& point) {
    return {point.X(), point.Y(), point.Z()};
}

std::filesystem::path create_test_stl_for_first_face(const spo::ShapeDocument& document) {
    const auto& face = document.topology().face(0);
    const auto surface = BRep_Tool::Surface(face);
    assert(!surface.IsNull());

    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    const auto u0 = uMin + (uMax - uMin) * 0.25;
    const auto u1 = uMin + (uMax - uMin) * 0.75;
    const auto v0 = vMin + (vMax - vMin) * 0.25;
    const auto v1 = vMin + (vMax - vMin) * 0.75;

    spo::StlMesh mesh;
    mesh.addTriangle(make_triangle(
        {0.0, 0.0, 1.0},
        to_vec3(surface->Value(u0, v0)),
        to_vec3(surface->Value(u1, v0)),
        to_vec3(surface->Value(u0, v1))));
    mesh.addTriangle(make_triangle(
        {0.0, 0.0, 1.0},
        {100.0, 100.0, 100.0},
        {101.0, 100.0, 100.0},
        {100.0, 101.0, 100.0}));

    const auto path = temp_step_path(".stl");
    assert(spo::StlWriter().write(mesh, path).success);
    return path;
}

}

void run_command_tests() {
    const auto sample = create_test_step();

    spo::CommandContext context;
    load_context(context, sample);

    spo::DetectFeatureCommand detect(25.0, 0.0);
    assert(detect.execute(context).success());

    spo::MergePatchCommand merge(25.0, 0.0, 0.001, false);
    assert(merge.execute(context).success());
    assert(context.document.hasShape());
    assert(!merge.result().concat_bsplines);

    spo::ValidateShapeCommand validate;
    assert(validate.execute(context).success());
    assert(context.validationReport.has_shape);

    const auto output = temp_step_path("-export.stp");
    spo::ExportStepCommand exportStep(output);
    assert(exportStep.execute(context).success());
    std::filesystem::remove(output);

    {
        spo::CommandHistory history;
        spo::CommandContext commandContext;
        assert(history.execute(std::make_unique<NonUndoableCommand>(), commandContext).success());
        assert(!history.canUndo());
        assert(!history.canRedo());
    }

    {
        spo::CommandHistory history;
        spo::CommandContext commandContext;
        const auto result = history.execute(std::make_unique<ThrowingCommand>(), commandContext);
        assert(!result.success());
        assert(result.message().find("ThrowingCommand") != std::string::npos);
        assert(!history.canUndo());
    }

    {
        spo::CommandHistory history;
        spo::CommandContext lockContext;
        load_context(lockContext, sample);
        assert(history.execute(std::make_unique<spo::LockEdgeCommand>(std::vector<spo::EdgeId>{1, 2}), lockContext).success());
        assert(spo::lockedEdgeIds(lockContext.lockedEdges).contains(1));
        assert(spo::lockedEdgeIds(lockContext.lockedEdges).contains(2));
        assert(history.canUndo());

        assert(history.undo(lockContext).success());
        assert(lockContext.lockedEdges.empty());
        assert(history.canRedo());

        assert(history.redo(lockContext).success());
        assert(spo::lockedEdgeIds(lockContext.lockedEdges).contains(1));
        assert(spo::lockedEdgeIds(lockContext.lockedEdges).contains(2));
    }

    {
        spo::CommandHistory history;
        spo::CommandContext unlockContext;
        load_context(unlockContext, sample);
        unlockContext.lockedEdges = locked_refs({1, 2, 3});
        assert(history.execute(std::make_unique<spo::UnlockEdgeCommand>(std::vector<spo::EdgeId>{2, 3}), unlockContext).success());
        assert(spo::lockedEdgeIds(unlockContext.lockedEdges) == std::set<spo::EdgeId>{1});

        assert(history.undo(unlockContext).success());
        assert(spo::lockedEdgeIds(unlockContext.lockedEdges) == std::set<spo::EdgeId>({1, 2, 3}));

        assert(history.redo(unlockContext).success());
        assert(spo::lockedEdgeIds(unlockContext.lockedEdges) == std::set<spo::EdgeId>{1});
    }

    {
        spo::CommandContext unlockWithoutDocument;
        unlockWithoutDocument.lockedEdges = locked_refs({1});
        spo::UnlockEdgeCommand unlock(1);
        assert(!unlock.execute(unlockWithoutDocument).success());
    }

    {
        spo::CommandHistory history;
        spo::CommandContext redoContext;
        load_context(redoContext, sample);
        assert(history.execute(std::make_unique<spo::LockEdgeCommand>(std::vector<spo::EdgeId>{1}), redoContext).success());
        assert(history.undo(redoContext).success());
        assert(history.canRedo());
        assert(history.execute(std::make_unique<spo::LockEdgeCommand>(std::vector<spo::EdgeId>{2}), redoContext).success());
        assert(!history.canRedo());
    }

    {
        spo::CommandHistory history;
        spo::CommandContext mergeContext;
        load_context(mergeContext, sample);
        assert(spo::LockEdgeCommand(1).execute(mergeContext).success());
        const auto beforeStats = mergeContext.document.stats();
        assert(history.execute(std::make_unique<spo::MergePatchCommand>(25.0, 0.0, 0.001, false), mergeContext).success());
        const auto afterStats = mergeContext.document.stats();
        assert(mergeContext.document.hasShape());
        assert(spo::lockedEdgeIds(mergeContext.lockedEdges).size() <= 1);

        assert(history.undo(mergeContext).success());
        assert(mergeContext.document.hasShape());
        assert(same_stats(mergeContext.document.stats(), beforeStats));
        assert(mergeContext.lockedEdges.empty());

        assert(history.redo(mergeContext).success());
        assert(mergeContext.document.hasShape());
        assert(same_stats(mergeContext.document.stats(), afterStats));
    }

    {
        spo::CommandContext protectedContext;
        load_context(protectedContext, sample);
        assert(spo::LockEdgeCommand(1).execute(protectedContext).success());
        spo::MergePatchCommand mergeWithLockedEdge(25.0, 1000000.0, 0.001, false);
        assert(mergeWithLockedEdge.execute(protectedContext).success());
        assert(mergeWithLockedEdge.result().protected_edges == 1);
    }

    {
        spo::CommandContext concatFalseContext;
        load_context(concatFalseContext, sample);
        spo::MergePatchCommand mergeWithoutConcat(25.0, 0.0, 0.001, false);
        assert(mergeWithoutConcat.execute(concatFalseContext).success());
        assert(concatFalseContext.document.hasShape());
        assert(!mergeWithoutConcat.result().concat_bsplines);
    }

    {
        spo::CommandContext concatTrueContext;
        load_context(concatTrueContext, sample);
        spo::MergePatchCommand mergeWithConcat(25.0, 0.0, 0.001, true);
        assert(mergeWithConcat.execute(concatTrueContext).success());
        assert(concatTrueContext.document.hasShape());
        assert(mergeWithConcat.result().concat_bsplines);
    }

    {
        spo::AppController controller;
        assert(controller.openStepFile(sample).success());
        assert(controller.lockEdges({1, 2}).success());
        assert(controller.canUndo());
        assert(!controller.lockedEdges().empty());

        const auto secondSample = create_test_step();
        assert(controller.openStepFile(secondSample).success());
        assert(!controller.canUndo());
        assert(!controller.canRedo());
        assert(controller.lockedEdges().empty());
        std::filesystem::remove(secondSample);
    }

    {
        spo::AppController controller;
        assert(controller.openStepFile(sample).success());
        const auto candidate = make_first_face_candidate(controller.document());
        const auto output = temp_step_path("-local.stl");

        const auto result = controller.cropStlForCandidate(candidate, output);

        assert(!result.success);
        assert(!result.message.empty());
        std::filesystem::remove(output);
    }

    {
        spo::AppController controller;
        assert(controller.openStepFile(sample).success());
        const auto stlPath = create_test_stl_for_first_face(controller.document());
        const auto loadStl = controller.openStlFile(stlPath);
        assert(loadStl.success());
        assert(controller.hasSourceStl());
        assert(controller.sourceStlPath() == stlPath);
        assert(controller.sourceStlMesh().triangleCount() == 2);

        const auto candidate = make_first_face_candidate(controller.document());
        const auto output = temp_step_path("-local.stl");
        const auto result = controller.cropStlForCandidate(candidate, output);

        assert(result.success);
        assert(result.extract.success);
        assert(result.extract.report.success);
        assert(result.extract.report.output_triangle_count > 0);
        assert(result.outputPath == output);
        assert(std::filesystem::exists(output));

        const auto roundTrip = spo::StlReader().read(output);
        assert(roundTrip.success);
        assert(roundTrip.mesh.triangleCount() == result.extract.localMesh.triangleCount());
        std::filesystem::remove(output);
        std::filesystem::remove(stlPath);
    }

    {
        const auto root = temp_root("spo-patch-preview-pipeline-no-document");
        const auto result = spo::AppController::cropAndRunGeomagicForCandidateData(
            {},
            {},
            {},
            root,
            {});

        assert(!result.success);
        assert(!result.message.empty());
        std::filesystem::remove_all(root);
    }

    {
        const auto root = temp_root("spo-patch-preview-pipeline-empty-source");
        spo::AppController controller;
        assert(controller.openStepFile(sample).success());
        const auto candidate = make_first_face_candidate(controller.document());

        const auto result = spo::AppController::cropAndRunGeomagicForCandidateData(
            controller.document(),
            {},
            candidate,
            root,
            {});

        assert(!result.success);
        assert(!result.message.empty());
        std::filesystem::remove_all(root);
    }

    {
        const auto root = temp_root("spo-patch-preview-pipeline-non-feature");
        spo::AppController controller;
        assert(controller.openStepFile(sample).success());
        const auto stlPath = create_test_stl_for_first_face(controller.document());
        assert(controller.openStlFile(stlPath).success());
        auto candidate = make_first_face_candidate(controller.document());
        candidate.candidate_type = spo::MergeCandidateType::SameDomain;

        const auto result = spo::AppController::cropAndRunGeomagicForCandidateData(
            controller.document(),
            controller.sourceStlMesh(),
            candidate,
            root,
            {});

        assert(!result.success);
        assert(!result.message.empty());
        std::filesystem::remove_all(root);
        std::filesystem::remove(stlPath);
    }

    {
        const auto root = temp_root("spo-patch-preview-pipeline-success");
        const auto mock = write_mock_geomagic_cmd(root);
        const auto script = root / "mock_script.py";
        write_text_file(script, "# mock script placeholder\n");

        spo::AppController controller;
        assert(controller.openStepFile(sample).success());
        const auto stlPath = create_test_stl_for_first_face(controller.document());
        assert(controller.openStlFile(stlPath).success());
        const auto candidate = make_first_face_candidate(controller.document());

        spo::GeomagicAutoSurfaceConfig config;
        config.wrapCorePath = mock;
        config.scriptPath = script;
        config.timeoutSeconds = 20;
        config.strictPatchTarget = true;

        const auto result = spo::AppController::cropAndRunGeomagicForCandidateData(
            controller.document(),
            controller.sourceStlMesh(),
            candidate,
            root,
            config);

        const auto documentStem = sample.stem();
        const auto expectedBase = documentStem.string() + "_candidate_0003";
        const auto expectedLocalStl = root / "data" / "crop_stl" / documentStem / (expectedBase + ".stl");
        const auto expectedStep = root / "data" / "crop_stp" / documentStem / (expectedBase + ".stp");
        const auto expectedIges = root / "data" / "crop_igs" / documentStem / (expectedBase + ".igs");
        const auto expectedFitLog = root / "data" / "crop_stp" / documentStem / (expectedBase + "_fit_region.log");

        assert(result.success);
        assert(result.crop.success);
        assert(result.crop.outputPath == expectedLocalStl);
        assert(result.geomagic.success);
        assert(result.geomagic.outputStepPath == expectedStep);
        assert(result.geomagic.outputIgesPath == expectedIges);
        assert(result.geomagic.fitRegionLogPath == expectedFitLog);
        assert(std::filesystem::exists(expectedLocalStl));
        assert(std::filesystem::exists(expectedStep));
        assert(!std::filesystem::exists(expectedIges));
        assert(!std::filesystem::exists(expectedStep.parent_path() / (expectedBase + "_autosurface_config.json")));
        assert(!std::filesystem::exists(expectedStep.parent_path() / (expectedBase + "_autosurface_result.json")));
        assert(!std::filesystem::exists(expectedStep.parent_path() / (expectedBase + "_autosurface_stdout.log")));
        assert(!std::filesystem::exists(expectedStep.parent_path() / (expectedBase + "_autosurface_stderr.log")));
        assert(std::filesystem::path(trim_line(read_text_file(root / "mock_geomagic_cwd.txt"))).lexically_normal() == root.lexically_normal());

        std::filesystem::remove_all(root);
        std::filesystem::remove(stlPath);
    }

    std::filesystem::remove(sample);
}
