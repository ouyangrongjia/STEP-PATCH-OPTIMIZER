#include "merge/ConeRegionMerger.h"

#include "merge/RegionMergeStub.h"

namespace spo {

RegionMergeResult ConeRegionMerger::merge(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionMergeOptions& options) const {
    return makeRegionMergeStubResult(
        document,
        candidate,
        options,
        MergeCandidateType::ConeLike,
        "ConeRegionMerger");
}

}
