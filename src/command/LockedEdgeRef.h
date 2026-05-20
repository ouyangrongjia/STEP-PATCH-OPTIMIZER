#pragma once

#include "common/GeometryTypes.h"

#include <gp_Pnt.hxx>

#include <optional>
#include <set>
#include <vector>

namespace spo {

class TopologyGraph;

struct EdgeGeometrySignature {
    gp_Pnt first;
    gp_Pnt last;
    gp_Pnt middle;
    double length = 0.0;
    bool valid = false;
};

struct LockedEdgeRef {
    EdgeId edgeId = 0;
    EdgeGeometrySignature signature;
};

std::optional<LockedEdgeRef> makeLockedEdgeRef(const TopologyGraph& topology, EdgeId edgeId);
std::set<EdgeId> lockedEdgeIds(const std::vector<LockedEdgeRef>& refs);

}
