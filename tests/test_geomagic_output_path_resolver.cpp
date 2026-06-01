#include "external/geomagic/GeomagicOutputPathResolver.h"

#include <cassert>
#include <filesystem>
#include <string>

namespace {

struct ScopedCurrentPath {
    explicit ScopedCurrentPath(const std::filesystem::path& next) :
        previous(std::filesystem::current_path()) {
        std::filesystem::current_path(next);
    }

    ~ScopedCurrentPath() {
        std::filesystem::current_path(previous);
    }

    std::filesystem::path previous;
};

std::filesystem::path temp_root(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void assert_same_path(const std::filesystem::path& actual, const std::filesystem::path& expected) {
    assert(actual.lexically_normal() == expected.lexically_normal());
}

void test_basic_path_resolves_to_step_and_iges() {
    const auto root = temp_root("spo_geomagic_resolver_basic");
    {
        const ScopedCurrentPath cwd(root);

        const auto result = spo::resolveGeomagicOutputPathsFromCropStl(
            std::filesystem::path("data") / "crop_stl" / "model" / "candidate_0001.stl");

        assert(result.success);
        assert(result.message.empty());
        assert_same_path(result.outputStepPath, std::filesystem::path("data") / "crop_stp" / "model" / "candidate_0001.stp");
        assert_same_path(result.outputIgesPath, std::filesystem::path("data") / "crop_igs" / "model" / "candidate_0001.igs");
    }

    std::filesystem::remove_all(root);
}

void test_nested_path_preserves_relative_directory() {
    const auto root = temp_root("spo_geomagic_resolver_nested");
    {
        const ScopedCurrentPath cwd(root);

        const auto result = spo::resolveGeomagicOutputPathsFromCropStl(
            std::filesystem::path("data") / "crop_stl" / "a" / "b" / "candidate_0002.stl");

        assert(result.success);
        assert_same_path(result.outputStepPath, std::filesystem::path("data") / "crop_stp" / "a" / "b" / "candidate_0002.stp");
        assert_same_path(result.outputIgesPath, std::filesystem::path("data") / "crop_igs" / "a" / "b" / "candidate_0002.igs");
    }

    std::filesystem::remove_all(root);
}

void test_chinese_path_preserves_relative_directory() {
    const auto root = temp_root("spo_geomagic_resolver_chinese");
    {
        const ScopedCurrentPath cwd(root);

        const auto result = spo::resolveGeomagicOutputPathsFromCropStl(
            std::filesystem::path("data") / "crop_stl" / L"03_配件_Clay" / "candidate_0007.stl");

        assert(result.success);
        assert(result.outputStepPath.generic_wstring().find(L"03_配件_Clay") != std::wstring::npos);
        assert(result.outputIgesPath.generic_wstring().find(L"03_配件_Clay") != std::wstring::npos);
        assert_same_path(result.outputStepPath, std::filesystem::path("data") / "crop_stp" / L"03_配件_Clay" / "candidate_0007.stp");
        assert_same_path(result.outputIgesPath, std::filesystem::path("data") / "crop_igs" / L"03_配件_Clay" / "candidate_0007.igs");
    }

    std::filesystem::remove_all(root);
}

void test_uppercase_extension_resolves_to_lowercase_outputs() {
    const auto root = temp_root("spo_geomagic_resolver_uppercase");
    {
        const ScopedCurrentPath cwd(root);

        const auto result = spo::resolveGeomagicOutputPathsFromCropStl(
            std::filesystem::path("data") / "crop_stl" / "model" / "candidate_0003.STL");

        assert(result.success);
        assert(result.outputStepPath.extension() == ".stp");
        assert(result.outputIgesPath.extension() == ".igs");
    }

    std::filesystem::remove_all(root);
}

void test_non_crop_stl_input_fails() {
    const auto root = temp_root("spo_geomagic_resolver_non_crop");
    {
        const ScopedCurrentPath cwd(root);

        const auto result = spo::resolveGeomagicOutputPathsFromCropStl(
            std::filesystem::path("data") / "stl" / "model.stl");

        assert(!result.success);
        assert(!result.message.empty());
        assert(result.outputStepPath.empty());
        assert(result.outputIgesPath.empty());
    }

    std::filesystem::remove_all(root);
}

void test_invalid_extension_fails() {
    const auto root = temp_root("spo_geomagic_resolver_invalid_extension");
    {
        const ScopedCurrentPath cwd(root);

        const auto result = spo::resolveGeomagicOutputPathsFromCropStl(
            std::filesystem::path("data") / "crop_stl" / "model" / "candidate_0004.obj");

        assert(!result.success);
        assert(!result.message.empty());
        assert(result.outputStepPath.empty());
        assert(result.outputIgesPath.empty());
    }

    std::filesystem::remove_all(root);
}

void test_output_directories_are_created() {
    const auto root = temp_root("spo_geomagic_resolver_creates_dirs");
    const auto cropStlRoot = root / "crop_stl";
    const auto cropStpRoot = root / "crop_stp";
    const auto cropIgsRoot = root / "crop_igs";
    const auto input = cropStlRoot / "nested" / "candidate_0005.stl";

    const auto result = spo::resolveGeomagicOutputPathsFromCropStl(input, cropStlRoot, cropStpRoot, cropIgsRoot);

    assert(result.success);
    assert(std::filesystem::is_directory(cropStpRoot / "nested"));
    assert(std::filesystem::is_directory(cropIgsRoot / "nested"));

    std::filesystem::remove_all(root);
}

void test_absolute_path_resolves_with_absolute_roots() {
    const auto root = std::filesystem::absolute(temp_root("spo_geomagic_resolver_absolute"));
    const auto cropStlRoot = root / "crop_stl";
    const auto cropStpRoot = root / "crop_stp";
    const auto cropIgsRoot = root / "crop_igs";
    const auto input = std::filesystem::absolute(cropStlRoot / "model" / "candidate_0006.stl");

    const auto result = spo::resolveGeomagicOutputPathsFromCropStl(input, cropStlRoot, cropStpRoot, cropIgsRoot);

    assert(result.success);
    assert_same_path(result.outputStepPath, cropStpRoot / "model" / "candidate_0006.stp");
    assert_same_path(result.outputIgesPath, cropIgsRoot / "model" / "candidate_0006.igs");

    std::filesystem::remove_all(root);
}

}

void run_geomagic_output_path_resolver_tests() {
    test_basic_path_resolves_to_step_and_iges();
    test_nested_path_preserves_relative_directory();
    test_chinese_path_preserves_relative_directory();
    test_uppercase_extension_resolves_to_lowercase_outputs();
    test_non_crop_stl_input_fails();
    test_invalid_extension_fails();
    test_output_directories_are_created();
    test_absolute_path_resolves_with_absolute_roots();
}
