#pragma once

#include "merge/RegionBoundaryAnalyzer.h"

#include <TopoDS_Wire.hxx>
#include <gp_Pnt.hxx>

#include <string>
#include <vector>

namespace spo {

class ShapeDocument;

struct BoundaryWireBuildResult {
    bool success = false;
    TopoDS_Wire wire;
    std::string message;
};

class BoundaryWireBuilder {
public:
    BoundaryWireBuildResult buildOuterWire(
        const ShapeDocument& document,
        const RegionBoundaryAnalysis& analysis) const;

    static std::vector<gp_Pnt> sampleWireLoop(
        const TopoDS_Wire& wire,
        int samplesPerEdge);
};

}
