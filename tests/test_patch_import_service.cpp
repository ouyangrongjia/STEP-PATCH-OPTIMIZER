#include "brep/ShapeDocument.h"
#include "io/StepWriter.h"
#include "patch/PatchImportService.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <IGESControl_Writer.hxx>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

namespace {

std::filesystem::path temp_root(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void remove_temp_root(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

std::string path_to_occt_string(const std::filesystem::path& path) {
    const auto utf8Path = path.u8string();
    return {reinterpret_cast<const char*>(utf8Path.c_str()), utf8Path.size()};
}

TopoDS_Shape make_test_box() {
    return BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream);
    stream << text;
}

void write_step_file(const std::filesystem::path& path) {
    const spo::ShapeDocument document(make_test_box(), {});
    const auto result = spo::StepWriter().write(document, path);
    assert(result.success());
    assert(std::filesystem::exists(path));
}

void write_iges_file(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    IGESControl_Writer writer("MM", 0);
    assert(writer.AddShape(make_test_box()));
    writer.ComputeModel();
    const auto occtPath = path_to_occt_string(path);
    assert(writer.Write(occtPath.c_str()));
    assert(std::filesystem::exists(path));
}

bool same_stats(const spo::ShapeStats& lhs, const spo::ShapeStats& rhs) {
    return lhs.solids == rhs.solids &&
        lhs.shells == rhs.shells &&
        lhs.faces == rhs.faces &&
        lhs.edges == rhs.edges &&
        lhs.vertices == rhs.vertices;
}

void test_missing_patch_file_fails() {
    const auto root = temp_root("spo_patch_import_missing");
    const auto result = spo::PatchImportService().importPatch(root / "missing.stp");

    assert(!result.success);
    assert(!result.errorMessage.empty() || !result.message.empty());

    remove_temp_root(root);
}

void test_unsupported_extension_fails() {
    const auto root = temp_root("spo_patch_import_unsupported");
    const auto path = root / "patch.txt";
    write_text_file(path, "not a CAD patch");

    const auto result = spo::PatchImportService().importPatch(path);

    assert(!result.success);
    assert(!result.errorMessage.empty() || !result.message.empty());

    remove_temp_root(root);
}

void test_import_simple_step_patch() {
    const auto root = temp_root("spo_patch_import_step");
    const auto path = root / "patch.stp";
    write_step_file(path);

    const auto result = spo::PatchImportService().importPatch(path);

    assert(result.success);
    assert(result.sourcePath == path);
    assert(!result.shape.IsNull());
    assert(result.faceCount > 0);
    assert(result.edgeCount > 0);
    assert(result.bboxValid);
    assert(result.bboxMinX <= result.bboxMaxX);
    assert(result.bboxMinY <= result.bboxMaxY);
    assert(result.bboxMinZ <= result.bboxMaxZ);
    assert(result.brepCheckValid);

    remove_temp_root(root);
}

void test_import_simple_iges_patch() {
    const auto root = temp_root("spo_patch_import_iges");
    const auto path = root / "patch.igs";
    write_iges_file(path);

    const auto result = spo::PatchImportService().importPatch(path);

    assert(result.success);
    assert(result.sourcePath == path);
    assert(!result.shape.IsNull());
    assert(result.faceCount > 0);
    assert(result.edgeCount > 0);
    assert(result.bboxValid);
    assert(result.brepCheckValid);

    remove_temp_root(root);
}

void test_import_from_result_prefers_step() {
    const auto root = temp_root("spo_patch_import_prefers_step");
    const auto stepPath = root / "patch.stp";
    const auto igesPath = root / "patch.igs";
    write_step_file(stepPath);
    write_iges_file(igesPath);

    spo::GeomagicAutoSurfaceResult geomagicResult;
    geomagicResult.outputStepPath = stepPath;
    geomagicResult.outputIgesPath = igesPath;

    const auto result = spo::PatchImportService().importPatchFromResult(geomagicResult);

    assert(result.success);
    assert(result.sourcePath == stepPath);
    assert(result.attemptedStepPath == stepPath);
    assert(result.attemptedIgesPath.empty());

    remove_temp_root(root);
}

void test_import_from_result_falls_back_to_output_iges() {
    const auto root = temp_root("spo_patch_import_fallback_iges");
    const auto stepPath = root / "missing.stp";
    const auto igesPath = root / "patch.igs";
    write_iges_file(igesPath);

    spo::GeomagicAutoSurfaceResult geomagicResult;
    geomagicResult.outputStepPath = stepPath;
    geomagicResult.outputIgesPath = igesPath;

    const auto result = spo::PatchImportService().importPatchFromResult(geomagicResult);

    assert(result.success);
    assert(result.sourcePath == igesPath);
    assert(result.attemptedStepPath == stepPath);
    assert(result.attemptedIgesPath == igesPath);

    remove_temp_root(root);
}

void test_import_from_result_falls_back_to_preserved_iges() {
    const auto root = temp_root("spo_patch_import_fallback_preserved_iges");
    const auto stepPath = root / "missing.stp";
    const auto outputIgesPath = root / "missing.igs";
    const auto preservedIgesPath = root / "preserved.igs";
    write_iges_file(preservedIgesPath);

    spo::GeomagicAutoSurfaceResult geomagicResult;
    geomagicResult.outputStepPath = stepPath;
    geomagicResult.outputIgesPath = outputIgesPath;
    geomagicResult.preservedIgesPath = preservedIgesPath;

    const auto result = spo::PatchImportService().importPatchFromResult(geomagicResult);

    assert(result.success);
    assert(result.sourcePath == preservedIgesPath);
    assert(result.attemptedStepPath == stepPath);
    assert(result.attemptedIgesPath == preservedIgesPath);

    remove_temp_root(root);
}

void test_service_does_not_accept_or_mutate_shape_document() {
    static_assert(!std::is_invocable_v<
        decltype(&spo::PatchImportService::importPatch),
        const spo::PatchImportService*,
        spo::ShapeDocument&>);

    const auto root = temp_root("spo_patch_import_no_document_mutation");
    const auto path = root / "patch.stp";
    write_step_file(path);

    const spo::ShapeDocument document(make_test_box(), {});
    const auto before = document.stats();

    const auto result = spo::PatchImportService().importPatch(path);

    assert(result.success);
    assert(same_stats(document.stats(), before));

    remove_temp_root(root);
}

}

void run_patch_import_service_tests() {
    test_missing_patch_file_fails();
    test_unsupported_extension_fails();
    test_import_simple_step_patch();
    test_import_simple_iges_patch();
    test_import_from_result_prefers_step();
    test_import_from_result_falls_back_to_output_iges();
    test_import_from_result_falls_back_to_preserved_iges();
    test_service_does_not_accept_or_mutate_shape_document();
}
