#pragma once

#include "patch/PatchReplacementReport.h"

#include <TopoDS_Shape.hxx>

namespace spo {

struct PatchReplacementRepairResult {
    bool success = false;
    TopoDS_Shape shape;
    PatchReplacementRepairReport report;
};

PatchReplacementRepairResult repairPatchReplacementShape(
    const TopoDS_Shape& inputShape,
    const PatchReplacementRepairOptions& options = {});

}
