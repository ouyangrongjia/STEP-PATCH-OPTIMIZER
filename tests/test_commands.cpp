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
#include "io/StepWriter.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <set>

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

    std::filesystem::remove(sample);
}
