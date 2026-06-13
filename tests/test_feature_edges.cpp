#include "brep/TopologyGraph.h"
#include "feature/FeatureEdgeDetector.h"

#include <BRepPrimAPI_MakeSphere.hxx>

#include <cassert>

namespace {

void test_empty_topology_has_no_feature_edges() {
    const spo::TopologyGraph topology;
    const spo::FeatureEdgeDetector detector;
    const auto result = detector.detect(topology, 25.0);
    assert(result.edges.empty());
    assert(result.sharp_edges == 0);
    assert(result.free_edges == 0);
    assert(result.multiple_edges == 0);
}

void test_closed_seam_edge_is_not_reported_as_free() {
    spo::TopologyGraph topology;
    topology.build(BRepPrimAPI_MakeSphere(10.0).Shape());

    const spo::FeatureEdgeDetector detector;
    const auto result = detector.detect(topology, 25.0);

    assert(result.free_edges == 0);
}

}

void run_feature_edges_tests() {
    test_empty_topology_has_no_feature_edges();
    test_closed_seam_edge_is_not_reported_as_free();
}
