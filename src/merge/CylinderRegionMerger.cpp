#include "merge/CylinderRegionMerger.h"

#include "merge/RegionMergeStub.h"

namespace spo {

RegionMergeResult CylinderRegionMerger::merge(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionMergeOptions& options) const {
    return makeRegionMergeStubResult(
        document,
        candidate,
        options,
        MergeCandidateType::CylinderLike,
        "CylinderRegionMerger");
}

}
