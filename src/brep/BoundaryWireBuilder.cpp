#include "brep/BoundaryWireBuilder.h"

#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"

#include <BRep_Tool.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>

#include <utility>

namespace spo {

namespace {

BoundaryWireBuildResult failed(std::string message) {
    BoundaryWireBuildResult result;
    result.message = std::move(message);
    return result;
}

}

BoundaryWireBuildResult BoundaryWireBuilder::buildOuterWire(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& analysis) const {
    if (!document.hasShape()) {
        return failed("Boundary wire build requires a loaded shape.");
    }
    if (!analysis.valid) {
        return failed("Boundary analysis is invalid.");
    }
    if (analysis.outer_wire_count != 1) {
        return failed("Boundary analysis must contain exactly one outer wire.");
    }
    if (analysis.inner_wire_count != 0) {
        return failed("Boundary wire build does not support inner wires.");
    }
    if (!analysis.boundary_closed) {
        return failed("Boundary analysis is not closed.");
    }
    if (analysis.ordered_boundary_edges.empty()) {
        return failed("Boundary analysis has no ordered boundary edges.");
    }

    const auto& topology = document.topology();
    BRepBuilderAPI_MakeWire wireBuilder;
    for (const auto edgeId : analysis.ordered_boundary_edges) {
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) {
            return failed("Boundary analysis references a missing boundary edge.");
        }
        const TopoDS_Edge& edge = topology.edge(edgeId);
        if (edge.IsNull()) {
            return failed("Boundary analysis references a null boundary edge.");
        }
        wireBuilder.Add(edge);
        if (!wireBuilder.IsDone()) {
            return failed("Ordered boundary edges cannot be added to one wire.");
        }
    }

    auto wire = wireBuilder.Wire();
    if (wire.IsNull() || !wire.Closed()) {
        return failed("Ordered boundary edges do not form a closed wire.");
    }

    BoundaryWireBuildResult result;
    result.success = true;
    result.wire = wire;
    result.message = "Boundary wire built from one closed outer loop.";
    return result;
}

std::vector<gp_Pnt> BoundaryWireBuilder::sampleWireLoop(
    const TopoDS_Wire& wire,
    int samplesPerEdge) {
    std::vector<gp_Pnt> points;
    if (wire.IsNull() || samplesPerEdge <= 0) {
        return points;
    }

    for (BRepTools_WireExplorer explorer(wire); explorer.More(); explorer.Next()) {
        const auto edge = TopoDS::Edge(explorer.Current());
        if (edge.IsNull()) {
            continue;
        }

        double first = 0.0;
        double last = 0.0;
        auto curve = BRep_Tool::Curve(edge, first, last);
        if (curve.IsNull()) {
            continue;
        }

        const bool reversed = edge.Orientation() == TopAbs_REVERSED;
        for (int k = 0; k < samplesPerEdge; ++k) {
            const double u = static_cast<double>(k) / static_cast<double>(samplesPerEdge);
            const double parameter = reversed
                ? last - (last - first) * u
                : first + (last - first) * u;
            points.push_back(curve->Value(parameter));
        }
    }

    return points;
}

}
