#include "merge/TorusRegionMerger.h"

#include "merge/RegionMergeStub.h"

namespace spo {

RegionMergeResult TorusRegionMerger::merge(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionMergeOptions& options) const {
    return makeRegionMergeStubResult(
        document,
        candidate,
        options,
        MergeCandidateType::TorusLike,
        "TorusRegionMerger");
}

}
