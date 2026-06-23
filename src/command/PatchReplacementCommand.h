#pragma once

#include "brep/ShapeDocument.h"
#include "command/Command.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/ImportedPatchInfo.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchPreviewReport.h"
#include "patch/PatchReplacementInput.h"
#include "patch/PatchReplacementReport.h"

namespace spo {

struct PatchReplacementCommandOptions {
    bool requireWatertightSolidGate = false;
    bool requireZeroFreeEdges = false;
    bool requireZeroMultipleEdges = false;
    bool requireRoundtripWatertight = false;
};

class PatchReplacementCommand final : public Command {
public:
    explicit PatchReplacementCommand(
        PatchReplacementInput input,
        PatchReplacementReport* outReport = nullptr,
        PatchReplacementCommandOptions options = {});

    const char* name() const override;
    Result execute(CommandContext& context) override;
    bool undoable() const override;
    Result undo(CommandContext& context) override;
    Result redo(CommandContext& context) override;

    const PatchReplacementReport& report() const;
    const ShapeDocument& afterDocument() const;

private:
    void publishReport();
    void rebuildStableInputFromContext(const ShapeDocument& document);

    PatchReplacementInput input_;
    PatchReplacementReport report_;
    PatchReplacementReport* outReport_ = nullptr;
    PatchReplacementCommandOptions options_;

    ShapeDocument documentSnapshot_;
    MergeCandidate candidateSnapshot_;
    RegionBoundaryAnalysis boundarySnapshot_;
    ImportedPatchInfo importedPatchSnapshot_;
    PatchArtifactPaths artifactPathsSnapshot_;
    PatchPreviewReport previewReportSnapshot_;

    bool hasDocumentInput_ = false;
    bool hasCandidateInput_ = false;
    bool hasBoundaryInput_ = false;
    bool hasImportedPatchInput_ = false;
    bool hasArtifactPathsInput_ = false;
    bool hasPreviewReportInput_ = false;

    ShapeDocument beforeDocument_;
    ShapeDocument afterDocument_;
    bool executed_ = false;
    bool committed_ = false;
};

}
