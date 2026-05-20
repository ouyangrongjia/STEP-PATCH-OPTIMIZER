#pragma once

#include "brep/ShapeDocument.h"
#include "common/GeometryTypes.h"

#include <vector>

namespace spo {

struct SameDomainUnifyOptions {
    double linear_tolerance = 0.001;
    double angular_tolerance_degrees = 25.0;
    bool unify_edges = true;
    bool unify_faces = true;
    bool concat_bsplines = false;
};

struct SameDomainUnifyResult {
    ShapeDocument document;
    ShapeStats before;
    ShapeStats after;
    int protected_edges = 0;
};

class SameDomainUnifier {
public:
    SameDomainUnifyResult unify(
        const ShapeDocument& document,
        const std::vector<EdgeId>& protectedEdges,
        const SameDomainUnifyOptions& options) const;
};

}
