#include "io/StlReader.h"
#include "io/StlWriter.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr double kTolerance = 1.0e-5;

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

void assert_vec_near(const spo::StlVec3& actual, const spo::StlVec3& expected) {
    assert(std::fabs(actual.x - expected.x) <= kTolerance);
    assert(std::fabs(actual.y - expected.y) <= kTolerance);
    assert(std::fabs(actual.z - expected.z) <= kTolerance);
}

void assert_bbox_near(const spo::StlBoundingBox& actual, const spo::StlBoundingBox& expected) {
    assert(actual.valid);
    assert(expected.valid);
    assert_vec_near(actual.min, expected.min);
    assert_vec_near(actual.max, expected.max);
}

std::filesystem::path temp_path(const char* filename) {
    return std::filesystem::temp_directory_path() / filename;
}

void write_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(stream);
}

void append_u32_le(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xffu));
    bytes.push_back(static_cast<unsigned char>((value >> 8u) & 0xffu));
    bytes.push_back(static_cast<unsigned char>((value >> 16u) & 0xffu));
    bytes.push_back(static_cast<unsigned char>((value >> 24u) & 0xffu));
}

void test_stl_writer_round_trips_binary_mesh() {
    spo::StlMesh mesh;
    mesh.addTriangle(make_triangle(
        {0.0, 0.0, 1.0},
        {-1.0, 0.5, 2.0},
        {3.0, -4.0, 5.0},
        {6.0, 7.0, -8.0}));
    mesh.addTriangle(make_triangle(
        {0.0, 1.0, 0.0},
        {-2.0, 4.0, 1.0},
        {0.25, -5.0, 2.5},
        {8.0, 9.0, 10.0}));
    const auto expectedBbox = mesh.boundingBox();
    const auto path = temp_path("spo_stl_io_roundtrip_binary.stl");
    std::filesystem::remove(path);

    const auto writeResult = spo::StlWriter().write(mesh, path);
    assert(writeResult.success);
    assert(writeResult.message.empty());

    const auto readResult = spo::StlReader().read(path);
    assert(readResult.success);
    assert(readResult.message.empty());
    assert(readResult.mesh.triangleCount() == mesh.triangleCount());
    assert_bbox_near(readResult.mesh.boundingBox(), expectedBbox);

    std::filesystem::remove(path);
}

void test_stl_reader_reports_missing_file() {
    const auto path = temp_path("spo_stl_io_missing_file.stl");
    std::filesystem::remove(path);

    const auto result = spo::StlReader().read(path);

    assert(!result.success);
    assert(!result.message.empty());
}

void test_stl_writer_rejects_empty_mesh() {
    const spo::StlMesh mesh;
    const auto path = temp_path("spo_stl_io_empty_mesh.stl");
    std::filesystem::remove(path);

    const auto result = spo::StlWriter().write(mesh, path);

    assert(!result.success);
    assert(!result.message.empty());
    std::filesystem::remove(path);
}

void test_stl_reader_rejects_truncated_binary_file() {
    const auto path = temp_path("spo_stl_io_truncated.stl");
    write_bytes(path, {'b', 'a', 'd'});

    const auto result = spo::StlReader().read(path);

    assert(!result.success);
    assert(!result.message.empty());
    std::filesystem::remove(path);
}

void test_stl_reader_rejects_triangle_count_mismatch() {
    const auto path = temp_path("spo_stl_io_count_mismatch.stl");
    std::vector<unsigned char> bytes(80, 0);
    append_u32_le(bytes, 1);
    write_bytes(path, bytes);

    const auto result = spo::StlReader().read(path);

    assert(!result.success);
    assert(!result.message.empty());
    std::filesystem::remove(path);
}

void test_stl_reader_reports_ascii_stl_unsupported() {
    const auto path = temp_path("spo_stl_io_ascii.stl");
    const std::string ascii = "solid ascii\nendsolid ascii\n";
    write_bytes(path, std::vector<unsigned char>(ascii.begin(), ascii.end()));

    const auto result = spo::StlReader().read(path);

    assert(!result.success);
    assert(result.message.find("ASCII") != std::string::npos);
    std::filesystem::remove(path);
}

void test_stl_reader_reads_real_clay_stl_if_present() {
    const std::filesystem::path realStl =
        std::filesystem::path(L"data") / L"stl" / L"03_配件_Clay.stl";
    if (!std::filesystem::exists(realStl)) {
        return;
    }

    const auto result = spo::StlReader().read(realStl);
    assert(result.success);
    assert(result.mesh.triangleCount() > 0);

    const auto bbox = result.mesh.boundingBox();
    assert(bbox.valid);

    const auto dx = bbox.max.x - bbox.min.x;
    const auto dy = bbox.max.y - bbox.min.y;
    const auto dz = bbox.max.z - bbox.min.z;
    assert(dx != 0.0 || dy != 0.0 || dz != 0.0);
}

}

void run_stl_io_tests() {
    test_stl_writer_round_trips_binary_mesh();
    test_stl_reader_reports_missing_file();
    test_stl_writer_rejects_empty_mesh();
    test_stl_reader_rejects_truncated_binary_file();
    test_stl_reader_rejects_triangle_count_mismatch();
    test_stl_reader_reports_ascii_stl_unsupported();
    test_stl_reader_reads_real_clay_stl_if_present();
}
