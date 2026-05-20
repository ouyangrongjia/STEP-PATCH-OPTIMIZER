#pragma once

#include "brep/ShapeDocument.h"

namespace spo {

struct ShapeValidationReport {
    ShapeStats stats;
    int free_edges = 0;
    int multiple_edges = 0;
    bool brep_check_valid = false;
    bool has_shape = false;
};

class ShapeValidator {
public:
    ShapeValidationReport validate(const ShapeDocument& document) const;
};

}
