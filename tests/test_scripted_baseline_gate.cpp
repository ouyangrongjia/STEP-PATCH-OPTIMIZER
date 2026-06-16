#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

std::filesystem::path source_root() {
#if defined(SPO_SOURCE_DIR)
    return std::filesystem::path(SPO_SOURCE_DIR);
#else
    return std::filesystem::current_path();
#endif
}

void test_corner_baseline_probe_supports_auto_candidate_selection() {
    const auto root = source_root();
    const auto source = read_text_file(root / "tools" / "corner_baseline_probe.cpp");

    assert(source.find("--candidate-id <id|auto>") != std::string::npos);
    assert(source.find("select_candidate_for_baseline") != std::string::npos);
    assert(source.find("candidate_selection_mode") != std::string::npos);
    assert(source.find("generated_candidate_count") != std::string::npos);
}

void test_scripted_baseline_gate_is_not_hardcoded_to_one_sample() {
    const auto root = source_root();
    const auto script = read_text_file(root / "scripts" / "run_corner_baseline_gate.ps1");

    assert(script.find("param(") != std::string::npos);
    assert(script.find("CandidateId = \"auto\"") != std::string::npos);
    assert(script.find("Find-DefaultSourceStep") != std::string::npos);
    assert(script.find("Find-ExistingPatchForCandidate") != std::string::npos);
    assert(script.find("corner_baseline_probe") != std::string::npos);
    assert(script.find("-RealGeomagic") != std::string::npos);
    assert(script.find("--candidate-id") != std::string::npos);
    assert(script.find("candidate_0179") == std::string::npos);
    assert(script.find("03_") == std::string::npos);
}

void test_verify_real_geomagic_runs_scripted_baseline_gate() {
    const auto root = source_root();
    const auto script = read_text_file(root / "scripts" / "verify_spo.ps1");

    assert(script.find("run_corner_baseline_gate.ps1") != std::string::npos);
    assert(script.find("-RealGeomagic") != std::string::npos);
}

}

void run_scripted_baseline_gate_tests() {
    test_corner_baseline_probe_supports_auto_candidate_selection();
    test_scripted_baseline_gate_is_not_hardcoded_to_one_sample();
    test_verify_real_geomagic_runs_scripted_baseline_gate();
}
