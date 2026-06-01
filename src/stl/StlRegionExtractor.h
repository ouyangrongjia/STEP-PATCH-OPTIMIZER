#pragma once

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "stl/StlCropReport.h"
#include "stl/StlMesh.h"

namespace spo {

struct StlRegionExtractorOptions {
    double bboxMarginRatio = 0.01;
    double minMargin = 0.1;
};

struct StlRegionExtractResult {
    bool success = false;
    StlMesh localMesh;
    StlCropReport report;
};

class StlRegionExtractor {
public:
    StlRegionExtractResult extract(
        const ShapeDocument& document,
        const MergeCandidate& candidate,
        const StlMesh& sourceMesh,
        const StlRegionExtractorOptions& options = {}) const;
};

}
