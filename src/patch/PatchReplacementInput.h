#pragma once

namespace spo {

class ShapeDocument;
struct ImportedPatchInfo;
struct MergeCandidate;
struct PatchArtifactPaths;
struct PatchPreviewReport;
struct RegionBoundaryAnalysis;

struct PatchReplacementInput {
    const ShapeDocument* document = nullptr;
    const MergeCandidate* candidate = nullptr;
    const RegionBoundaryAnalysis* boundary = nullptr;
    const ImportedPatchInfo* importedPatch = nullptr;
    const PatchArtifactPaths* artifactPaths = nullptr;
    const PatchPreviewReport* previewReport = nullptr;
    bool allowHighRiskPatchPreview = false;
    bool strictOriginalBoundaryRetrim = false;
};

}
