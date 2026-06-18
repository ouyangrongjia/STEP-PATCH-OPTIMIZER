#include "validate/ShapeValidator.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRep_Tool.hxx>

namespace spo {

namespace {

bool is_closed_seam_edge(const TopologyGraph& topology, EdgeId edgeId, const EdgeAdjacency& adjacency) {
    if (adjacency.faces.size() != 1 || adjacency.faces.front() >= topology.faceCount()) {
        return false;
    }
    return BRep_Tool::IsClosed(topology.edge(edgeId), topology.face(adjacency.faces.front()));
}

}

ShapeValidationReport ShapeValidator::validate(const ShapeDocument& document) const {
    ShapeValidationReport report;
    report.has_shape = document.hasShape();
    report.stats = document.stats();

    if (!document.hasShape()) {
        return report;
    }

    const auto& topology = document.topology();
    for (EdgeId id = 0; id < topology.edgeCount(); ++id) {
        const auto* adjacency = topology.adjacencyForEdge(id);
        if (adjacency == nullptr) {
            continue;
        }
        if (adjacency->faces.size() == 1) {
            if (BRep_Tool::Degenerated(topology.edge(id)) ||
                is_closed_seam_edge(topology, id, *adjacency)) {
                continue;
            }
            ++report.free_edges;
        } else if (adjacency->faces.size() > 2) {
            ++report.multiple_edges;
        }
    }

    BRepCheck_Analyzer analyzer(document.shape());
    report.brep_check_valid = analyzer.IsValid();
    return report;
}

}
