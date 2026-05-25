#include "merge/PlaneRegionMerger.h"

#include "merge/RegionMergeStub.h"

namespace spo {

RegionMergeResult PlaneRegionMerger::merge(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionMergeOptions& options) const {
    return makeRegionMergeStubResult(
        document,
        candidate,
        options,
        MergeCandidateType::PlaneLike,
        "PlaneRegionMerger");
}

}
