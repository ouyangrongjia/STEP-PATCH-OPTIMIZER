#pragma once

#include "common/GeometryTypes.h"
#include "patch/MultiFacePatchAnalyzer.h"
#include "patch/PatchReplacementInput.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace spo {

enum class BoundaryConstrainedBuildFailureReason {
    None,
    InvalidInput,
    InvalidBoundary,
    NoPatchFaces,
    ReplacementBuildFailed,
    BoundaryMismatch,
    ShapeFixFailed
};

const char* toString(BoundaryConstrainedBuildFailureReason reason);

struct BoundaryConstrainedPatchBuildOptions {
    bool allowMultiFaceFragment = true;
    bool allowOneFaceSpecialPath = true;
    bool keepInternalPatchEdges = true;
    double bboxToleranceRatio = 4.0;
};

struct BoundaryConstrainedPatchBuildResult {
    bool success = false;
    BoundaryConstrainedBuildFailureReason failureReason = BoundaryConstrainedBuildFailureReason::None;

    TopoDS_Shape replacementShape;
    std::vector<TopoDS_Face> replacementFaces;
    std::vector<TopoDS_Edge> internalPatchEdges;
    std::vector<FaceId> sourceFaceIds;

    bool usedMultiFaceFragment = false;
    bool usedOneFaceSpecialPath = false;
    bool boundaryMismatch = false;

    int sourceFaceCount = 0;
    int patchFaceCount = 0;
    int replacementFaceCount = 0;
    int internalPatchEdgeCount = 0;

    std::string message;
    std::string warningMessage;
};

class BoundaryConstrainedPatchBuilder {
public:
    BoundaryConstrainedPatchBuildResult build(
        const PatchReplacementInput& input,
        const MultiFacePatchAnalysis& analysis,
        const BoundaryConstrainedPatchBuildOptions& options = {}) const;
};

}
