#pragma once

#include "patch/BoundaryConstrainedPatchBuilder.h"
#include "patch/PatchReplacementReport.h"

#include <TopoDS_Shape.hxx>

#include <memory>
#include <vector>

namespace spo {

class ShapeDocument;
struct PatchClosureSnapshotData;
struct RegionBoundaryAnalysis;

class PatchClosureSnapshot {
public:
    PatchClosureSnapshot() = default;

private:
    std::shared_ptr<PatchClosureSnapshotData> data_;

    friend class PatchClosureDiagnostics;
};

class PatchClosureDiagnostics {
public:
    PatchClosureSnapshot capture(const TopoDS_Shape& shape) const;

    void copyPreRepairReport(
        PatchReplacementReport& report,
        const PatchClosureSnapshot& snapshot) const;

    void copyPostRepairReport(
        PatchReplacementReport& report,
        const PatchClosureSnapshot& snapshot) const;

    std::vector<PatchReplacementFreeEdgeDiagnostic> buildFreeEdgeDiagnostics(
        const PatchClosureSnapshot& preRepairSnapshot,
        const PatchClosureSnapshot& postRepairSnapshot,
        const ShapeDocument& beforeDocument,
        const RegionBoundaryAnalysis& boundary,
        const BoundaryConstrainedPatchBuildResult& buildResult) const;
};

}
