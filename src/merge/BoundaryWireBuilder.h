#pragma once

#include "merge/RegionBoundaryAnalyzer.h"

#include <TopoDS_Wire.hxx>

#include <string>

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
};

}
