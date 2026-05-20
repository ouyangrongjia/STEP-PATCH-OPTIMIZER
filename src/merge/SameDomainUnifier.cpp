#include "merge/SameDomainUnifier.h"

#include <ShapeUpgrade_UnifySameDomain.hxx>

namespace spo {

namespace {

constexpr double kPi = 3.14159265358979323846;

double degreesToRadians(double degrees) {
    return degrees * kPi / 180.0;
}

}

SameDomainUnifyResult SameDomainUnifier::unify(
    const ShapeDocument& document,
    const std::vector<EdgeId>& protectedEdges,
    const SameDomainUnifyOptions& options) const {
    SameDomainUnifyResult result;
    result.before = document.stats();
    result.protected_edges = static_cast<int>(protectedEdges.size());

    if (!document.hasShape()) {
        result.document = document;
        result.after = document.stats();
        return result;
    }

    ShapeUpgrade_UnifySameDomain unifier(
        document.shape(),
        options.unify_edges,
        options.unify_faces,
        options.concat_bsplines);
    unifier.SetLinearTolerance(options.linear_tolerance);
    unifier.SetAngularTolerance(degreesToRadians(options.angular_tolerance_degrees));
    unifier.SetSafeInputMode(true);

    const auto& topology = document.topology();
    for (const auto edge : protectedEdges) {
        if (edge < topology.edgeCount()) {
            unifier.KeepShape(topology.edge(edge));
        }
    }

    unifier.Build();
    result.document = ShapeDocument(unifier.Shape(), document.sourcePath());
    result.after = result.document.stats();
    return result;
}

}
