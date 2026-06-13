#include "stl/StlCutChainCutter.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace {

void test_empty_mesh_fails() {
    spo::StlCutChainCutter cutter;
    spo::StlMesh emptyMesh;
    std::vector<gp_Pnt> loop = {
        gp_Pnt(0,0,0), gp_Pnt(1,0,0), gp_Pnt(1,1,0), gp_Pnt(0,1,0)};
    auto result = cutter.cut(emptyMesh, loop);
    assert(!result.success);
    assert(!result.message.empty());
}

void test_short_loop_fails() {
    spo::StlCutChainCutter cutter;
    spo::StlMesh mesh;
    spo::StlTriangle t;
    t.v0 = {0,0,0}; t.v1 = {10,0,0}; t.v2 = {0,10,0};
    t.normal = {0,0,1};
    mesh.addTriangle(t);

    std::vector<gp_Pnt> shortLoop = {gp_Pnt(0,0,0), gp_Pnt(1,0,0)};
    auto result = cutter.cut(mesh, shortLoop);
    assert(!result.success);
    assert(!result.message.empty());
}

void test_topology_building() {
    spo::StlMesh mesh;
    spo::StlTriangle t;
    t.v0 = {0,0,0}; t.v1 = {1,0,0}; t.v2 = {0,1,0};
    t.normal = {0,0,1};
    mesh.addTriangle(t);

    auto topo = spo::StlCutChainCutter::buildTopology(mesh);
    assert(topo.faces.size() == 1);
    assert(topo.vertices.size() == 3);
    assert(!topo.centers.empty());
}

void test_triangle_area() {
    using Vec3 = std::array<double,3>;
    double a = spo::StlCutChainCutter::triangleArea({0,0,0}, {1,0,0}, {0,1,0});
    assert(std::abs(a - 0.5) < 1e-10);
}

void test_polyline_length() {
    using Vec3 = std::array<double,3>;
    double len = spo::StlCutChainCutter::polylineLength({{0,0,0}, {3,0,0}, {3,4,0}});
    assert(std::abs(len - 7.0) < 1e-10);
}

void test_point_segment_distance() {
    using Vec3 = std::array<double,3>;
    double d = spo::StlCutChainCutter::pointSegmentDistance({1,0,0}, {0,0,0}, {2,0,0});
    assert(d < 1e-10);
}

void test_presampled_boundary_can_skip_per_segment_resampling() {
    std::vector<gp_Pnt> loop = {
        gp_Pnt(0,0,0), gp_Pnt(1,0,0), gp_Pnt(1,1,0), gp_Pnt(0,1,0)};

    const auto sampled = spo::StlCutChainCutter::sampleBoundaryLoop(loop, 1);

    assert(sampled.size() == loop.size());
    assert(std::abs(sampled[0][0] - 0.0) < 1e-10);
    assert(std::abs(sampled[1][0] - 1.0) < 1e-10);
    assert(std::abs(sampled[2][1] - 1.0) < 1e-10);
    assert(std::abs(sampled[3][1] - 1.0) < 1e-10);
}

void test_project_to_stl_returns_closest_surface_point() {
    spo::StlMesh mesh;
    spo::StlTriangle t;
    t.v0 = {0,0,0};
    t.v1 = {1,0,0};
    t.v2 = {0,1,0};
    t.normal = {0,0,1};
    mesh.addTriangle(t);

    const auto topo = spo::StlCutChainCutter::buildTopology(mesh);
    std::vector<std::array<double, 3>> green;
    std::vector<std::int64_t> triIds;
    std::vector<double> distances;

    spo::StlCutChainCutter::projectToStl(
        topo,
        {{2.0, 0.0, 0.0}},
        green,
        triIds,
        distances);

    assert(green.size() == 1);
    assert(triIds.size() == 1);
    assert(distances.size() == 1);
    assert(triIds[0] == 0);
    assert(std::abs(green[0][0] - 1.0) < 1e-10);
    assert(std::abs(green[0][1]) < 1e-10);
    assert(std::abs(green[0][2]) < 1e-10);
    assert(std::abs(distances[0] - 1.0) < 1e-10);
}

void run_tests() {
    test_empty_mesh_fails();
    test_short_loop_fails();
    test_topology_building();
    test_triangle_area();
    test_polyline_length();
    test_point_segment_distance();
    test_presampled_boundary_can_skip_per_segment_resampling();
    test_project_to_stl_returns_closest_surface_point();
}

}

void run_stl_cut_chain_cutter_tests() {
    run_tests();
}
