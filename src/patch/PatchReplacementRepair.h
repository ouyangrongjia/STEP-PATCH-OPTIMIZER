#pragma once

#include "patch/PatchReplacementRepairMetrics.h"

#include <TopoDS_Shape.hxx>

#include <string>

namespace spo {

struct PatchReplacementRepairOptions {
    bool runSameParameter = true;
    bool runShapeFixShape = true;
    bool runShapeFixFace = true;
    bool runShapeFixWire = true;
    bool runSewing = true;
    bool runUnifySameDomain = true;
    bool runAdaptiveSewing = true;
    bool runShellToSolid = true;
    double sewingTolerance = 1.0e-4;
    double preferredSewingTolerance = 0.007;
    double minSewingTolerance = 0.001;
    double maxSewingTolerance = 0.1;
    double collapseFaceRatio = 0.5;
    bool keepInternalPatchSeams = true;
};

struct PatchReplacementRepairReport : PatchReplacementRepairMetrics {
    bool success = false;
    std::string message;
    std::string warningMessage;
};

struct PatchReplacementRepairResult {
    bool success = false;
    TopoDS_Shape shape;
    PatchReplacementRepairReport report;
};

PatchReplacementRepairResult repairPatchReplacementShape(
    const TopoDS_Shape& inputShape,
    const PatchReplacementRepairOptions& options = {});

}
