#include "brep/ShapeDocument.h"
#include "io/StepWriter.h"
#include "merge/SameDomainUnifier.h"
#include "patch/PatchImportService.h"
#include "validate/ShapeValidator.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {

struct Options {
    std::filesystem::path inputStep;
    std::filesystem::path unifiedStep;
    std::filesystem::path report;
};

struct Snapshot {
    spo::ShapeStats stats;
    int freeEdges = 0;
    int multipleEdges = 0;
    bool brepCheckValid = false;
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

std::string bool_json(bool value) {
    return value ? "true" : "false";
}

void print_usage() {
    std::cerr
        << "Usage: raw_patch_unify_probe --input-step <normalized_patch_mm.stp> "
        << "--unified-step <unified.stp> --report <report.json>\n";
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

        if (arg == "--input-step") {
            const auto* value = require_value("--input-step");
            if (value == nullptr) {
                return false;
            }
            options.inputStep = value;
        } else if (arg == "--unified-step") {
            const auto* value = require_value("--unified-step");
            if (value == nullptr) {
                return false;
            }
            options.unifiedStep = value;
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

    return !options.inputStep.empty() && !options.unifiedStep.empty() && !options.report.empty();
}

Snapshot snapshot_for(const spo::ShapeDocument& document) {
    const auto validation = spo::ShapeValidator().validate(document);
    return Snapshot{
        validation.stats,
        validation.free_edges,
        validation.multiple_edges,
        validation.brep_check_valid,
    };
}

std::string stats_json(const spo::ShapeStats& stats, int indent) {
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

std::string snapshot_json(const Snapshot& snapshot, int indent) {
    const std::string pad(static_cast<size_t>(indent), ' ');
    std::ostringstream out;
    out << "{\n";
    out << pad << "  \"stats\": " << stats_json(snapshot.stats, indent + 2) << ",\n";
    out << pad << "  \"free_edges\": " << snapshot.freeEdges << ",\n";
    out << pad << "  \"multiple_edges\": " << snapshot.multipleEdges << ",\n";
    out << pad << "  \"brep_check_valid\": " << bool_json(snapshot.brepCheckValid) << "\n";
    out << pad << "}";
    return out.str();
}

bool write_report(
    const Options& options,
    const Snapshot& before,
    const Snapshot& after,
    const std::optional<Snapshot>& afterRoundtrip,
    const spo::SameDomainUnifyResult& unify,
    bool writeOk,
    const std::string& writeMessage,
    const std::string& roundtripMessage) {
    if (!options.report.parent_path().empty()) {
        std::filesystem::create_directories(options.report.parent_path());
    }

    std::ofstream out(options.report, std::ios::binary);
    if (!out.good()) {
        return false;
    }

    out << "{\n";
    out << "  \"tool\": \"raw_patch_unify_probe\",\n";
    out << "  \"input_step\": " << quote_json(path_to_utf8(options.inputStep)) << ",\n";
    out << "  \"unified_step\": " << quote_json(path_to_utf8(options.unifiedStep)) << ",\n";
    out << "  \"unified_step_write_ok\": " << bool_json(writeOk) << ",\n";
    out << "  \"unified_step_write_message\": " << quote_json(writeMessage) << ",\n";
    out << "  \"unified_step_roundtrip_ok\": " << bool_json(afterRoundtrip.has_value()) << ",\n";
    out << "  \"unified_step_roundtrip_message\": " << quote_json(roundtripMessage) << ",\n";
    out << "  \"unify_edges\": true,\n";
    out << "  \"unify_faces\": true,\n";
    out << "  \"concat_bsplines\": " << bool_json(unify.concat_bsplines) << ",\n";
    out << "  \"protected_edges\": " << unify.protected_edges << ",\n";
    out << "  \"face_reduction_ratio\": " << unify.face_reduction_ratio << ",\n";
    out << "  \"edge_reduction_ratio\": " << unify.edge_reduction_ratio << ",\n";
    out << "  \"before\": " << snapshot_json(before, 2) << ",\n";
    out << "  \"after_unify_in_memory\": " << snapshot_json(after, 2) << ",\n";
    out << "  \"after_unified_step_roundtrip\": ";
    if (afterRoundtrip.has_value()) {
        out << snapshot_json(*afterRoundtrip, 2) << "\n";
    } else {
        out << "null\n";
    }
    out << "}\n";
    return true;
}

void print_snapshot(const char* label, const Snapshot& snapshot) {
    std::cout << label << "\n";
    std::cout << "  faces: " << snapshot.stats.faces << "\n";
    std::cout << "  edges: " << snapshot.stats.edges << "\n";
    std::cout << "  shells: " << snapshot.stats.shells << "\n";
    std::cout << "  free_edges: " << snapshot.freeEdges << "\n";
    std::cout << "  brep_check_valid: " << snapshot.brepCheckValid << "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }

    if (!std::filesystem::exists(options.inputStep)) {
        std::cerr << "input step does not exist: " << path_to_utf8(options.inputStep) << "\n";
        return 1;
    }
    if (!options.unifiedStep.parent_path().empty()) {
        std::filesystem::create_directories(options.unifiedStep.parent_path());
    }

    const auto imported = spo::PatchImportService().importPatch(options.inputStep);
    if (!imported.success) {
        std::cerr << (!imported.errorMessage.empty() ? imported.errorMessage : imported.message) << "\n";
        return 1;
    }

    const spo::ShapeDocument inputDocument(imported.shape, options.inputStep);
    const auto before = snapshot_for(inputDocument);

    spo::SameDomainUnifyOptions unifyOptions;
    const auto unify = spo::SameDomainUnifier().unify(inputDocument, {}, unifyOptions);
    const auto after = snapshot_for(unify.document);

    const auto write = spo::StepWriter().write(unify.document, options.unifiedStep);
    const auto writeMessage = write.success() ? std::string{} : write.message();
    std::optional<Snapshot> afterRoundtrip;
    std::string roundtripMessage;
    if (write.success()) {
        const auto roundtrip = spo::PatchImportService().importPatch(options.unifiedStep);
        if (roundtrip.success) {
            const spo::ShapeDocument roundtripDocument(roundtrip.shape, options.unifiedStep);
            afterRoundtrip = snapshot_for(roundtripDocument);
        } else {
            roundtripMessage = !roundtrip.errorMessage.empty() ? roundtrip.errorMessage : roundtrip.message;
        }
    }

    if (!write_report(options, before, after, afterRoundtrip, unify, write.success(), writeMessage, roundtripMessage)) {
        std::cerr << "failed to write report: " << path_to_utf8(options.report) << "\n";
        return 1;
    }

    std::cout << "raw_patch_unify_probe\n";
    std::cout << "input_step: " << path_to_utf8(options.inputStep) << "\n";
    std::cout << "unified_step: " << path_to_utf8(options.unifiedStep) << "\n";
    std::cout << "report: " << path_to_utf8(options.report) << "\n";
    std::cout << "unified_step_write_ok: " << write.success() << "\n";
    if (!writeMessage.empty()) {
        std::cout << "unified_step_write_message: " << writeMessage << "\n";
    }
    print_snapshot("before", before);
    print_snapshot("after_unify_in_memory", after);
    if (afterRoundtrip.has_value()) {
        print_snapshot("after_unified_step_roundtrip", *afterRoundtrip);
    } else if (!roundtripMessage.empty()) {
        std::cout << "unified_step_roundtrip_message: " << roundtripMessage << "\n";
    }

    return write.success() ? 0 : 1;
}
