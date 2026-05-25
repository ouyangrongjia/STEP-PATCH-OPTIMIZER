#pragma once

#include "brep/ShapeDocument.h"
#include "feature/FeatureEdgeDetector.h"
#include "merge/FaceInspectInfo.h"

#include <set>
#include <vector>

namespace spo {

FaceInspectInfo inspectFace(
    const ShapeDocument& document,
    FaceId faceId,
    const std::vector<MergeCandidate>& candidates,
    const std::set<int>& visibleCandidateIds,
    const FeatureEdgeDetectionResult* featureEdges,
    const std::set<EdgeId>& lockedEdges);

}
