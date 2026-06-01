#include "io/StlReader.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace spo {

namespace {

constexpr std::uintmax_t kHeaderSize = 80;
constexpr std::uintmax_t kCountSize = 4;
constexpr std::uintmax_t kTriangleRecordSize = 50;
constexpr std::uintmax_t kMinimumBinarySize = kHeaderSize + kCountSize;

StlReadResult read_error(const std::string& message) {
    StlReadResult result;
    result.message = message;
    return result;
}

bool read_exact(std::ifstream& stream, char* data, std::streamsize size) {
    stream.read(data, size);
    return stream.good();
}

bool starts_with_solid(const char* data, std::size_t size) {
    constexpr char kSolid[] = {'s', 'o', 'l', 'i', 'd'};
    if (size < sizeof(kSolid)) {
        return false;
    }
    for (std::size_t i = 0; i < sizeof(kSolid); ++i) {
        if (data[i] != kSolid[i]) {
            return false;
        }
    }
    return true;
}

bool file_starts_with_solid(const std::filesystem::path& path) {
    std::array<char, 5> prefix {};
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    stream.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    return starts_with_solid(prefix.data(), static_cast<std::size_t>(stream.gcount()));
}

std::uint32_t read_u32_le(const std::array<unsigned char, 4>& bytes) {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8u)
        | (static_cast<std::uint32_t>(bytes[2]) << 16u)
        | (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

float read_float_le(const unsigned char* bytes) {
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8u)
        | (static_cast<std::uint32_t>(bytes[2]) << 16u)
        | (static_cast<std::uint32_t>(bytes[3]) << 24u);

    float result = 0.0f;
    static_assert(sizeof(result) == sizeof(value));
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

StlVec3 read_vec3(const unsigned char* bytes) {
    return {
        static_cast<double>(read_float_le(bytes)),
        static_cast<double>(read_float_le(bytes + 4)),
        static_cast<double>(read_float_le(bytes + 8)),
    };
}

StlTriangle read_triangle(const std::array<unsigned char, kTriangleRecordSize>& bytes) {
    StlTriangle triangle;
    triangle.normal = read_vec3(bytes.data());
    triangle.v0 = read_vec3(bytes.data() + 12);
    triangle.v1 = read_vec3(bytes.data() + 24);
    triangle.v2 = read_vec3(bytes.data() + 36);
    return triangle;
}

}

StlReadResult StlReader::read(const std::filesystem::path& path) const {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return read_error("STL file does not exist.");
    }

    const auto fileSize = std::filesystem::file_size(path, error);
    if (error) {
        return read_error("Could not determine STL file size.");
    }

    if (fileSize < kMinimumBinarySize) {
        if (file_starts_with_solid(path)) {
            return read_error("ASCII STL is unsupported.");
        }
        return read_error("Binary STL file is too short.");
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return read_error("Could not open STL file.");
    }

    std::array<char, kHeaderSize> header {};
    if (!read_exact(stream, header.data(), static_cast<std::streamsize>(header.size()))) {
        return read_error("Could not read STL header.");
    }

    std::array<unsigned char, kCountSize> countBytes {};
    if (!read_exact(
            stream,
            reinterpret_cast<char*>(countBytes.data()),
            static_cast<std::streamsize>(countBytes.size()))) {
        return read_error("Could not read STL triangle count.");
    }

    const auto triangleCount = read_u32_le(countBytes);
    const auto expectedSize = kMinimumBinarySize
        + static_cast<std::uintmax_t>(triangleCount) * kTriangleRecordSize;
    if (fileSize != expectedSize) {
        if (starts_with_solid(header.data(), header.size())) {
            return read_error("ASCII STL is unsupported.");
        }
        return read_error("Binary STL triangle count does not match file size.");
    }

    StlReadResult result;
    result.mesh.triangles().reserve(triangleCount);

    for (std::uint32_t i = 0; i < triangleCount; ++i) {
        std::array<unsigned char, kTriangleRecordSize> triangleBytes {};
        if (!read_exact(
                stream,
                reinterpret_cast<char*>(triangleBytes.data()),
                static_cast<std::streamsize>(triangleBytes.size()))) {
            return read_error("Could not read STL triangle record.");
        }
        result.mesh.addTriangle(read_triangle(triangleBytes));
    }

    result.success = true;
    return result;
}

}
