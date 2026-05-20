#pragma once

#include "common/GeometryTypes.h"

#include <vector>

namespace spo {

class TopologyGraph;

enum class FeatureEdgeKind {
    Smooth,
    Sharp,
    Free,
    Multiple
};

struct FeatureEdge {
    EdgeId edge = 0;
    FeatureEdgeKind kind = FeatureEdgeKind::Smooth;
    double dihedral_degrees = 0.0;
};

struct FeatureEdgeDetectionResult {
    std::vector<FeatureEdge> edges;
    int sharp_edges = 0;
    int free_edges = 0;
    int multiple_edges = 0;
};

class FeatureEdgeDetector {
public:
    FeatureEdgeDetectionResult detect(
        const TopologyGraph& topology,
        double angularThresholdDegrees,
        double minEdgeLength = 0.0) const;

private:
    static double edgeLength(const TopologyGraph& topology, EdgeId edge);
    static double dihedralAngleDegrees(const TopologyGraph& topology, FaceId first, FaceId second);
};

}
