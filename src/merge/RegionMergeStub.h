#pragma once

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionMergeOptions.h"
#include "merge/RegionMergeResult.h"

namespace spo {

RegionMergeResult makeRegionMergeStubResult(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionMergeOptions& options,
    MergeCandidateType expectedType,
    const char* mergerName);

}
