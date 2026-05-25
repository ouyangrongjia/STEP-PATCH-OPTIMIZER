#include "merge/SphereRegionMerger.h"

#include "merge/RegionMergeStub.h"

namespace spo {

RegionMergeResult SphereRegionMerger::merge(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionMergeOptions& options) const {
    return makeRegionMergeStubResult(
        document,
        candidate,
        options,
        MergeCandidateType::SphereLike,
        "SphereRegionMerger");
}

}
