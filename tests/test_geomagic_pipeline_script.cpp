#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path repo_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

QString path_to_qstring(const std::filesystem::path& path) {
    const auto utf8Path = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8Path.c_str()), static_cast<qsizetype>(utf8Path.size()));
}

void assert_contains(const std::string& text, const char* needle) {
    assert(text.find(needle) != std::string::npos);
}

void assert_not_contains(const std::string& text, const char* needle) {
    assert(text.find(needle) == std::string::npos);
}

void test_geomagic_pipeline_files_exist() {
    const auto root = repo_root();

    assert(std::filesystem::exists(root / "scripts" / "geomagic_wrap" / "autosurface_pipeline.py"));
    assert(std::filesystem::exists(root / "scripts" / "geomagic_wrap" / "autosurface_config.example.json"));
    assert(std::filesystem::exists(root / "scripts" / "geomagic_wrap" / "README.md"));
}

void test_geomagic_pipeline_script_mentions_required_contract() {
    const auto script = read_text_file(repo_root() / "scripts" / "geomagic_wrap" / "autosurface_pipeline.py");

    assert_contains(script, "FIT_REGION_INPUT");
    assert_contains(script, "FIT_REGION_OUTPUT");
    assert_contains(script, "FIT_REGION_LOG_FILE");
    assert_contains(script, "FIT_REGION_REPAIR_MESH");
    assert_contains(script, "RepairMesh");
    assert_contains(script, "RepairStrategy");
    assert_contains(script, "RemoveNonManifoldVertices");
    assert_contains(script, "FillSmallHoles");
    assert_contains(script, "FIT_REGION_FILL_HOLE_MAX_EDGES");
    assert_contains(script, "FIT_REGION_FILL_HOLE_LENGTH_RATIO");
    assert_contains(script, "FIT_REGION_GEOMETRY_MODE");
    assert_contains(script, "Mechanical");
    assert_contains(script, "requested autoMerge={}");
    assert_contains(script, "numFilled");
    assert_contains(script, "smallHoles");
    assert_contains(script, "nonManifoldEdges");
    assert_contains(script, "AutoSurface");
    assert_contains(script, "ReadFile");
    assert_contains(script, "WriteFile");
    assert_contains(script, "filterId");
    assert_contains(script, "fit_region.py diagnostic log");
    assert_contains(script, "crop_stl");
    assert_contains(script, "crop_stp");
    assert_not_contains(script, "FIT_REGION_OUTPUT_IGES");
    assert_not_contains(script, "FIT_REGION_RESULT_JSON");
    assert_not_contains(script, "FIT_REGION_CONFIG_JSON");
    assert_not_contains(script, "result.json");
    assert_not_contains(script, "error_message");
}

void test_geomagic_pipeline_readme_mentions_manual_contract() {
    const auto readme = read_text_file(repo_root() / "scripts" / "geomagic_wrap" / "README.md");

    assert_contains(readme, "E:\\Geomagic Wrap\\wrapCore.exe");
    assert_contains(readme, "FIT_REGION_INPUT");
    assert_contains(readme, "FIT_REGION_OUTPUT");
    assert_contains(readme, "FIT_REGION_LOG_FILE");
    assert_contains(readme, "FIT_REGION_REPAIR_MESH=1");
    assert_contains(readme, "FIT_REGION_STRICT_PATCH_TARGET=0");
    assert_contains(readme, "data/crop_stl");
    assert_contains(readme, "data/crop_stp");
    assert_not_contains(readme, "FIT_REGION_OUTPUT_IGES");
    assert_not_contains(readme, "FIT_REGION_RESULT_JSON");
    assert_contains(readme, "不做 final CAD replacement");
    assert_contains(readme, "自动测试不调用真实 Geomagic");
}

void test_geomagic_pipeline_example_config_is_valid_json() {
    QFile file(path_to_qstring(repo_root() / "scripts" / "geomagic_wrap" / "autosurface_config.example.json"));
    assert(file.open(QIODevice::ReadOnly));

    const auto document = QJsonDocument::fromJson(file.readAll());
    assert(document.isObject());
    const auto object = document.object();

    assert(object.value("script_path").toString() == "scripts/geomagic_wrap/autosurface_pipeline.py");
    assert(object.value("input_stl_path").toString().contains("data/crop_stl"));
    assert(object.value("output_step_path").toString().contains("data/crop_stp"));
    assert(object.value("fit_region_log_path").toString().contains("fit_region.log"));
    assert(!object.contains("output_iges_path"));
    assert(!object.contains("result_json_path"));
    assert(!object.contains("stdout_log_path"));
    assert(!object.contains("stderr_log_path"));
    assert(object.value("geometry").toString() == "Mechanical");
    assert(object.value("num_patches").toInt() == 1);
}

}

void run_geomagic_pipeline_script_tests() {
    test_geomagic_pipeline_files_exist();
    test_geomagic_pipeline_script_mentions_required_contract();
    test_geomagic_pipeline_readme_mentions_manual_contract();
    test_geomagic_pipeline_example_config_is_valid_json();
}
