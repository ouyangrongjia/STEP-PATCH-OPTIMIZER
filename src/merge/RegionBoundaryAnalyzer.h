#pragma once

#include "brep/ShapeDocument.h"
#include "merge/BoundaryAnalysisFailureReason.h"
#include "merge/MergeCandidate.h"

#include <string>
#include <vector>

namespace spo {

struct RegionBoundaryAnalysis {
    bool valid = false;
    int connected_component_count = 0;
    int outer_wire_count = 0;
    int inner_wire_count = 0;
    bool boundary_closed = false;
    bool has_holes = false;
    bool has_non_manifold_edges = false;
    bool has_branching_boundary = false;
    std::vector<EdgeId> ordered_boundary_edges;
    std::vector<std::vector<EdgeId>> boundary_loops;
    BoundaryAnalysisFailureReason failure_reason = BoundaryAnalysisFailureReason::None;
    std::string message;
};

class RegionBoundaryAnalyzer {
public:
    RegionBoundaryAnalysis analyze(const ShapeDocument& document, const MergeCandidate& candidate) const;
};

}
