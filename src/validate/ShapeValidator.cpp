#include "validate/ShapeValidator.h"

#include <BRepCheck_Analyzer.hxx>

namespace spo {

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
