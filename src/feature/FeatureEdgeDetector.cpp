#include "feature/FeatureEdgeDetector.h"

#include "brep/TopologyGraph.h"

#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>

#include <algorithm>
#include <cmath>

namespace spo {

namespace {

constexpr double kPi = 3.14159265358979323846;

double clampCosine(double value) {
    return std::max(-1.0, std::min(1.0, value));
}

bool normalAtFaceCenter(const TopoDS_Face& face, gp_Dir& normal) {
    if (face.IsNull()) {
        return false;
    }

    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);

    if (!std::isfinite(uMin) || !std::isfinite(uMax) || !std::isfinite(vMin) || !std::isfinite(vMax)) {
        return false;
    }

    BRepAdaptor_Surface surface(face);
    BRepLProp_SLProps props(surface, (uMin + uMax) * 0.5, (vMin + vMax) * 0.5, 1, Precision::Confusion());
    if (!props.IsNormalDefined()) {
        return false;
    }

    normal = props.Normal();
    if (face.Orientation() == TopAbs_REVERSED) {
        normal.Reverse();
    }
    return true;
}

}

FeatureEdgeDetectionResult FeatureEdgeDetector::detect(
    const TopologyGraph& topology,
    double angularThresholdDegrees,
    double minEdgeLength) const {
    FeatureEdgeDetectionResult result;

    for (EdgeId id = 0; id < topology.edgeCount(); ++id) {
        if (minEdgeLength > 0.0 && edgeLength(topology, id) < minEdgeLength) {
            continue;
        }

        const auto* adjacency = topology.adjacencyForEdge(id);
        if (adjacency == nullptr) {
            continue;
        }

        FeatureEdge edge;
        edge.edge = id;

        if (adjacency->faces.size() == 1) {
            edge.kind = FeatureEdgeKind::Free;
            ++result.free_edges;
        } else if (adjacency->faces.size() > 2) {
            edge.kind = FeatureEdgeKind::Multiple;
            ++result.multiple_edges;
        } else if (adjacency->faces.size() == 2) {
            edge.dihedral_degrees = dihedralAngleDegrees(topology, adjacency->faces[0], adjacency->faces[1]);
            if (edge.dihedral_degrees >= angularThresholdDegrees) {
                edge.kind = FeatureEdgeKind::Sharp;
                ++result.sharp_edges;
            }
        }

        if (edge.kind != FeatureEdgeKind::Smooth) {
            result.edges.push_back(edge);
        }
    }

    return result;
}

double FeatureEdgeDetector::edgeLength(const TopologyGraph& topology, EdgeId edge) {
    GProp_GProps props;
    BRepGProp::LinearProperties(topology.edge(edge), props);
    return props.Mass();
}

double FeatureEdgeDetector::dihedralAngleDegrees(const TopologyGraph& topology, FaceId first, FaceId second) {
    gp_Dir firstNormal;
    gp_Dir secondNormal;
    if (!normalAtFaceCenter(topology.face(first), firstNormal) ||
        !normalAtFaceCenter(topology.face(second), secondNormal)) {
        return 0.0;
    }

    const auto dot = clampCosine(firstNormal.Dot(secondNormal));
    return std::acos(std::abs(dot)) * 180.0 / kPi;
}

}
