#include "brep/EdgeIndex.h"
#include "brep/FaceIndex.h"
#include "brep/TopologyGraph.h"

#include <cassert>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

namespace {

void test_indices_assign_sequential_ids() {
    spo::FaceIndex faces;
    spo::EdgeIndex edges;

    assert(faces.addFace(TopoDS_Face {}) == 0);
    assert(faces.addFace(TopoDS_Face {}) == 1);
    assert(faces.size() == 2);

    assert(edges.addEdge(TopoDS_Edge {}) == 0);
    assert(edges.size() == 1);
}

void test_empty_topology_graph_has_no_items() {
    spo::TopologyGraph graph;
    assert(graph.empty());
    assert(graph.faceCount() == 0);
    assert(graph.edgeCount() == 0);
    assert(!graph.faceIdFor(TopoDS_Face {}).has_value());
    assert(!graph.edgeIdFor(TopoDS_Edge {}).has_value());
}

}

void run_topology_graph_tests() {
    test_indices_assign_sequential_ids();
    test_empty_topology_graph_has_no_items();
}
