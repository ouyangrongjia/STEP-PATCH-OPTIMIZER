#include "io/StepReader.h"
#include "validate/StrictTopologyGate.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::filesystem::path beforeStep;
    std::filesystem::path afterStep;
    std::filesystem::path temporaryStep;
    std::filesystem::path report;
};

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.c_str()), utf8.size()};
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out << "\\u00";
                const char* digits = "0123456789ABCDEF";
                out << digits[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out << digits[static_cast<unsigned char>(c) & 0xF];
            } else {
                out << c;
            }
            break;
        }
    }
    return out.str();
}

std::string quote_json(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

std::string bool_json(const bool value) {
    return value ? "true" : "false";
}

void print_usage() {
    std::cerr
        << "Usage: strict_topology_gate_probe --before-step <source.stp> --after-step <result.stp> "
        << "--report <report.json> [--temporary-step <roundtrip.stp>]\n";
}

bool parse_options(int argc, char* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto require_value = [&](const char* name) -> const char* {
            if (index + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++index];
        };

        if (arg == "--before-step") {
            const auto* value = require_value("--before-step");
            if (value == nullptr) {
                return false;
            }
            options.beforeStep = value;
        } else if (arg == "--after-step") {
            const auto* value = require_value("--after-step");
            if (value == nullptr) {
                return false;
            }
            options.afterStep = value;
        } else if (arg == "--temporary-step") {
            const auto* value = require_value("--temporary-step");
            if (value == nullptr) {
                return false;
            }
            options.temporaryStep = value;
        } else if (arg == "--report") {
            const auto* value = require_value("--report");
            if (value == nullptr) {
                return false;
            }
            options.report = value;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    return !options.beforeStep.empty() &&
        !options.afterStep.empty() &&
        !options.report.empty();
}

std::string stats_json(const spo::ShapeStats& stats, const int indent) {
    const std::string pad(static_cast<size_t>(indent), ' ');
    std::ostringstream out;
    out << "{\n";
    out << pad << "  \"solids\": " << stats.solids << ",\n";
    out << pad << "  \"shells\": " << stats.shells << ",\n";
    out << pad << "  \"faces\": " << stats.faces << ",\n";
    out << pad << "  \"edges\": " << stats.edges << ",\n";
    out << pad << "  \"vertices\": " << stats.vertices << "\n";
    out << pad << "}";
    return out.str();
}

bool write_report(const Options& options, const spo::StrictTopologyGateReport& report) {
    std::filesystem::create_directories(options.report.parent_path());
    std::ofstream out(options.report, std::ios::binary);
    if (!out.good()) {
        return false;
    }

    out << "{\n";
    out << "  \"tool\": \"strict_topology_gate_probe\",\n";
    out << "  \"before_step\": " << quote_json(path_to_utf8(options.beforeStep)) << ",\n";
    out << "  \"after_step\": " << quote_json(path_to_utf8(options.afterStep)) << ",\n";
    out << "  \"passed\": " << bool_json(report.passed) << ",\n";
    out << "  \"failure_reason\": " << quote_json(spo::toString(report.failureReason)) << ",\n";
    out << "  \"message\": " << quote_json(report.message) << ",\n";
    out << "  \"warning\": " << quote_json(report.warningMessage) << ",\n";
    out << "  \"require_watertight_solid\": " << bool_json(report.watertightSolidRequired) << ",\n";
    out << "  \"require_roundtrip_watertight\": " << bool_json(report.roundtripWatertightRequired) << ",\n";
    out << "  \"before_stats\": " << stats_json(report.beforeStats, 2) << ",\n";
    out << "  \"after_stats\": " << stats_json(report.afterStats, 2) << ",\n";
    out << "  \"roundtrip_stats\": " << stats_json(report.roundtripStats, 2) << ",\n";
    out << "  \"before_free_edges\": " << report.beforeFreeEdges << ",\n";
    out << "  \"after_free_edges\": " << report.afterFreeEdges << ",\n";
    out << "  \"roundtrip_free_edges\": " << report.roundtripFreeEdges << ",\n";
    out << "  \"before_multiple_edges\": " << report.beforeMultipleEdges << ",\n";
    out << "  \"after_multiple_edges\": " << report.afterMultipleEdges << ",\n";
    out << "  \"roundtrip_multiple_edges\": " << report.roundtripMultipleEdges << ",\n";
    out << "  \"before_brep_check_valid\": " << bool_json(report.beforeBRepCheckValid) << ",\n";
    out << "  \"after_brep_check_valid\": " << bool_json(report.afterBRepCheckValid) << ",\n";
    out << "  \"roundtrip_brep_check_valid\": " << bool_json(report.roundtripBRepCheckValid) << ",\n";
    out << "  \"step_export_ok\": " << bool_json(report.stepExportOk) << ",\n";
    out << "  \"step_roundtrip_ok\": " << bool_json(report.stepRoundtripOk) << "\n";
    out << "}\n";
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }
    if (!std::filesystem::exists(options.beforeStep)) {
        std::cerr << "before step does not exist: " << path_to_utf8(options.beforeStep) << "\n";
        return 1;
    }
    if (!std::filesystem::exists(options.afterStep)) {
        std::cerr << "after step does not exist: " << path_to_utf8(options.afterStep) << "\n";
        return 1;
    }
    if (options.temporaryStep.empty()) {
        options.temporaryStep = options.report.parent_path() / "strict_topology_gate_roundtrip.stp";
    }

    const auto beforeRead = spo::StepReader().read(options.beforeStep);
    if (!beforeRead.status.success()) {
        std::cerr << beforeRead.status.message() << "\n";
        return 1;
    }
    const auto afterRead = spo::StepReader().read(options.afterStep);
    if (!afterRead.status.success()) {
        std::cerr << afterRead.status.message() << "\n";
        return 1;
    }

    spo::StrictTopologyGateInput input;
    input.beforeDocument = &beforeRead.document;
    input.afterDocument = &afterRead.document;
    input.requireStepRoundtrip = true;
    input.requireWatertightSolid = true;
    input.requireZeroFreeEdges = true;
    input.requireZeroMultipleEdges = true;
    input.requireRoundtripWatertight = true;
    input.temporaryStepPath = options.temporaryStep;

    const auto report = spo::StrictTopologyGate().evaluate(input);
    if (!write_report(options, report)) {
        std::cerr << "Could not write report: " << path_to_utf8(options.report) << "\n";
        return 1;
    }

    std::cout << "StrictTopologyGate passed: " << report.passed << "\n";
    std::cout << "failure_reason: " << spo::toString(report.failureReason) << "\n";
    std::cout << "report: " << path_to_utf8(options.report) << "\n";
    return report.passed ? 0 : 10;
}
