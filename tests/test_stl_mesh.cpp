#include "stl/StlMesh.h"

#include <cassert>

namespace {

void assert_vec_eq(const spo::StlVec3& actual, const spo::StlVec3& expected) {
    assert(actual.x == expected.x);
    assert(actual.y == expected.y);
    assert(actual.z == expected.z);
}

spo::StlTriangle make_triangle(
    spo::StlVec3 normal,
    spo::StlVec3 v0,
    spo::StlVec3 v1,
    spo::StlVec3 v2) {
    spo::StlTriangle triangle;
    triangle.normal = normal;
    triangle.v0 = v0;
    triangle.v1 = v1;
    triangle.v2 = v2;
    return triangle;
}

void test_empty_mesh_has_invalid_bbox() {
    const spo::StlMesh mesh;

    assert(mesh.empty());
    assert(mesh.triangleCount() == 0);
    assert(!mesh.boundingBox().valid);
}

void test_one_triangle_bbox_covers_vertices() {
    spo::StlMesh mesh;
    mesh.addTriangle(make_triangle(
        {0.0, 0.0, 1.0},
        {1.0, -2.0, 3.0},
        {-4.0, 5.0, 6.0},
        {7.0, 8.0, -9.0}));

    const auto bbox = mesh.boundingBox();

    assert(!mesh.empty());
    assert(mesh.triangleCount() == 1);
    assert(bbox.valid);
    assert_vec_eq(bbox.min, {-4.0, -2.0, -9.0});
    assert_vec_eq(bbox.max, {7.0, 8.0, 6.0});
}

void test_multiple_triangles_bbox_covers_all_vertices() {
    spo::StlMesh mesh;
    mesh.addTriangle(make_triangle(
        {0.0, 0.0, 1.0},
        {0.0, 1.0, 2.0},
        {3.0, 4.0, 5.0},
        {6.0, 7.0, 8.0}));
    mesh.addTriangle(make_triangle(
        {0.0, 1.0, 0.0},
        {-10.0, 2.0, 1.0},
        {2.0, -11.0, 3.0},
        {4.0, 5.0, 12.0}));

    const auto bbox = mesh.boundingBox();

    assert(mesh.triangleCount() == 2);
    assert(bbox.valid);
    assert_vec_eq(bbox.min, {-10.0, -11.0, 1.0});
    assert_vec_eq(bbox.max, {6.0, 7.0, 12.0});
}

void test_clear_removes_triangles_and_invalidates_bbox() {
    spo::StlMesh mesh;
    mesh.addTriangle(make_triangle(
        {0.0, 0.0, 1.0},
        {1.0, 2.0, 3.0},
        {4.0, 5.0, 6.0},
        {7.0, 8.0, 9.0}));

    mesh.clear();

    assert(mesh.empty());
    assert(mesh.triangleCount() == 0);
    assert(!mesh.boundingBox().valid);
}

void test_normal_does_not_affect_bbox() {
    spo::StlMesh mesh;
    mesh.addTriangle(make_triangle(
        {1000.0, -1000.0, 500.0},
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 1.0}));

    auto before = mesh.boundingBox();
    mesh.triangles().front().normal = {-2000.0, 3000.0, -4000.0};
    const auto after = mesh.boundingBox();

    assert(before.valid);
    assert(after.valid);
    assert_vec_eq(before.min, after.min);
    assert_vec_eq(before.max, after.max);
    assert_vec_eq(after.min, {0.0, 0.0, 0.0});
    assert_vec_eq(after.max, {1.0, 1.0, 1.0});
}

}

void run_stl_mesh_tests() {
    test_empty_mesh_has_invalid_bbox();
    test_one_triangle_bbox_covers_vertices();
    test_multiple_triangles_bbox_covers_all_vertices();
    test_clear_removes_triangles_and_invalidates_bbox();
    test_normal_does_not_affect_bbox();
}
