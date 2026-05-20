#include "brep/ShapeDocument.h"
#include "merge/SameDomainUnifier.h"

#include <cassert>

namespace {

void test_empty_document_unify_is_noop() {
    const spo::ShapeDocument document;
    const spo::SameDomainUnifier unifier;
    const spo::SameDomainUnifyOptions options;
    const auto result = unifier.unify(document, {}, options);
    assert(!result.document.hasShape());
    assert(result.before.faces == 0);
    assert(result.after.faces == 0);
    assert(result.protected_edges == 0);
}

}

void run_same_domain_merge_tests() {
    test_empty_document_unify_is_noop();
}
