#include "validate/ShapeValidator.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRep_Tool.hxx>

namespace spo {

namespace {

bool isClosedSeamOnSingleFace(const TopologyGraph& topology, EdgeId edgeId, const EdgeAdjacency& adjacency) {
    return adjacency.faces.size() == 1 &&
        BRep_Tool::IsClosed(topology.edge(edgeId), topology.face(adjacency.faces.front()));
}

bool isDegeneratedEdge(const TopologyGraph& topology, EdgeId edgeId) {
    return BRep_Tool::Degenerated(topology.edge(edgeId));
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
        if (adjacency->use_count == 1 &&
            !isClosedSeamOnSingleFace(topology, id, *adjacency) &&
            !isDegeneratedEdge(topology, id)) {
            ++report.free_edges;
        } else if (adjacency->use_count > 2) {
            ++report.multiple_edges;
        }
    }

    BRepCheck_Analyzer analyzer(document.shape());
    report.brep_check_valid = analyzer.IsValid();
    return report;
}

}
