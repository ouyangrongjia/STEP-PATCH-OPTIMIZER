#include "brep/ShapeDocument.h"
#include "common/JsonText.h"
#include "io/StepWriter.h"
#include "patch/PatchImportService.h"
#include "validate/ShapeValidator.h"

#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace {

using spo::json_text::path_to_utf8;
using spo::json_text::quote_json;

struct Options {
    std::filesystem::path baseStep;
    std::filesystem::path patchStep;
    std::filesystem::path outputStep;
    std::filesystem::path report;
    std::vector<double> tolerances;
};

struct Snapshot {
    spo::ShapeStats stats;
    int freeEdges = 0;
    int multipleEdges = 0;
    bool brepCheckValid = false;
};

struct Attempt {
    double tolerance = 0.0;
    TopoDS_Shape shape;
    Snapshot snapshot;
    bool madeSolid = false;
};

void print_usage() {
    std::cerr
        << "Usage: step_sew_probe --base-step <base_removed.stp> --patch-step <patch.stp> "
        << "--output-step <sewn.stp> --report <report.json> "
        << "[--tolerance <mm>]...\n";
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

        if (arg == "--base-step") {
            const auto* value = require_value("--base-step");
            if (value == nullptr) {
                return false;
            }
            options.baseStep = value;
        } else if (arg == "--patch-step") {
            const auto* value = require_value("--patch-step");
            if (value == nullptr) {
                return false;
            }
            options.patchStep = value;
        } else if (arg == "--output-step") {
            const auto* value = require_value("--output-step");
            if (value == nullptr) {
                return false;
            }
            options.outputStep = value;
        } else if (arg == "--report") {
            const auto* value = require_value("--report");
            if (value == nullptr) {
                return false;
            }
            options.report = value;
        } else if (arg == "--tolerance") {
            const auto* value = require_value("--tolerance");
            if (value == nullptr) {
                return false;
            }
            const double tolerance = std::stod(value);
            if (!(tolerance > 0.0)) {
                std::cerr << "Sewing tolerance must be positive.\n";
                return false;
            }
            options.tolerances.push_back(tolerance);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    if (options.tolerances.empty()) {
        options.tolerances = {0.001, 0.0025, 0.005, 0.007, 0.01, 0.02, 0.03};
    }
    std::sort(options.tolerances.begin(), options.tolerances.end());
    options.tolerances.erase(
        std::unique(options.tolerances.begin(), options.tolerances.end()),
        options.tolerances.end());

    return !options.baseStep.empty() &&
        !options.patchStep.empty() &&
        !options.outputStep.empty() &&
        !options.report.empty();
}

Snapshot snapshot_for(const TopoDS_Shape& shape) {
    const spo::ShapeDocument document(shape, {});
    const auto validation = spo::ShapeValidator().validate(document);
    return Snapshot{
        validation.stats,
        validation.free_edges,
        validation.multiple_edges,
        validation.brep_check_valid,
    };
}

bool closed_valid_solid(const Snapshot& snapshot) {
    return snapshot.stats.solids > 0 &&
        snapshot.freeEdges == 0 &&
        snapshot.multipleEdges == 0 &&
        snapshot.brepCheckValid;
}

TopoDS_Shape make_solid_if_closed_shell(
    const TopoDS_Shape& shape,
    const Snapshot& snapshot,
    bool& madeSolid) {
    madeSolid = false;
    if (snapshot.stats.solids > 0 ||
        snapshot.stats.shells != 1 ||
        snapshot.freeEdges != 0 ||
        snapshot.multipleEdges != 0) {
        return shape;
    }

    TopExp_Explorer shells(shape, TopAbs_SHELL);
    if (!shells.More()) {
        return shape;
    }
    BRepBuilderAPI_MakeSolid maker(TopoDS::Shell(shells.Current()));
    if (!maker.IsDone()) {
        return shape;
    }
    madeSolid = true;
    return maker.Solid();
}

Attempt sew_at_tolerance(
    const TopoDS_Shape& base,
    const TopoDS_Shape& patch,
    double tolerance) {
    Attempt attempt;
    attempt.tolerance = tolerance;

    BRepBuilderAPI_Sewing sewing(tolerance);
    sewing.SetNonManifoldMode(Standard_False);
    sewing.SetFloatingEdgesMode(Standard_True);
    sewing.SetSameParameterMode(Standard_True);
    sewing.Add(base);
    sewing.Add(patch);
    sewing.Perform();

    attempt.shape = sewing.SewedShape();
    attempt.snapshot = snapshot_for(attempt.shape);
    attempt.shape = make_solid_if_closed_shell(
        attempt.shape,
        attempt.snapshot,
        attempt.madeSolid);
    if (attempt.madeSolid) {
        attempt.snapshot = snapshot_for(attempt.shape);
    }
    return attempt;
}

const Attempt* choose_attempt(const std::vector<Attempt>& attempts, int expectedFaces) {
    const Attempt* best = nullptr;
    for (const auto& attempt : attempts) {
        if (attempt.shape.IsNull()) {
            continue;
        }
        if (best == nullptr ||
            std::make_tuple(
                !closed_valid_solid(attempt.snapshot),
                attempt.snapshot.freeEdges,
                attempt.snapshot.multipleEdges,
                !attempt.snapshot.brepCheckValid,
                std::abs(attempt.snapshot.stats.faces - expectedFaces),
                attempt.tolerance) <
            std::make_tuple(
                !closed_valid_solid(best->snapshot),
                best->snapshot.freeEdges,
                best->snapshot.multipleEdges,
                !best->snapshot.brepCheckValid,
                std::abs(best->snapshot.stats.faces - expectedFaces),
                best->tolerance)) {
            best = &attempt;
        }
    }
    return best;
}

std::string bool_json(bool value) {
    return value ? "true" : "false";
}

void write_snapshot(std::ostream& out, const Snapshot& snapshot, int indent) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    out << "{\n";
    out << pad << "  \"faces\": " << snapshot.stats.faces << ",\n";
    out << pad << "  \"edges\": " << snapshot.stats.edges << ",\n";
    out << pad << "  \"shells\": " << snapshot.stats.shells << ",\n";
    out << pad << "  \"solids\": " << snapshot.stats.solids << ",\n";
    out << pad << "  \"free_edges\": " << snapshot.freeEdges << ",\n";
    out << pad << "  \"multiple_edges\": " << snapshot.multipleEdges << ",\n";
    out << pad << "  \"brep_check_valid\": " << bool_json(snapshot.brepCheckValid) << "\n";
    out << pad << "}";
}

bool write_report(
    const Options& options,
    const Snapshot& base,
    const Snapshot& patch,
    const std::vector<Attempt>& attempts,
    const Attempt& selected,
    const Snapshot* roundtrip,
    bool writeOk,
    const std::string& writeMessage) {
    if (!options.report.parent_path().empty()) {
        std::filesystem::create_directories(options.report.parent_path());
    }
    std::ofstream out(options.report, std::ios::binary);
    if (!out.good()) {
        return false;
    }

    out << "{\n";
    out << "  \"tool\": \"step_sew_probe\",\n";
    out << "  \"purpose\": \"diagnostic_visual_only\",\n";
    out << "  \"base_step\": " << quote_json(path_to_utf8(options.baseStep)) << ",\n";
    out << "  \"patch_step\": " << quote_json(path_to_utf8(options.patchStep)) << ",\n";
    out << "  \"output_step\": " << quote_json(path_to_utf8(options.outputStep)) << ",\n";
    out << "  \"output_write_ok\": " << bool_json(writeOk) << ",\n";
    out << "  \"output_write_message\": " << quote_json(writeMessage) << ",\n";
    out << "  \"base\": ";
    write_snapshot(out, base, 2);
    out << ",\n  \"patch\": ";
    write_snapshot(out, patch, 2);
    out << ",\n  \"attempts\": [\n";
    for (std::size_t index = 0; index < attempts.size(); ++index) {
        const auto& attempt = attempts[index];
        out << "    {\n";
        out << "      \"tolerance_mm\": " << attempt.tolerance << ",\n";
        out << "      \"made_solid\": " << bool_json(attempt.madeSolid) << ",\n";
        out << "      \"closed_valid_solid\": "
            << bool_json(closed_valid_solid(attempt.snapshot)) << ",\n";
        out << "      \"snapshot\": ";
        write_snapshot(out, attempt.snapshot, 6);
        out << "\n    }" << (index + 1 == attempts.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"selected_tolerance_mm\": " << selected.tolerance << ",\n";
    out << "  \"selected_closed_valid_solid\": "
        << bool_json(closed_valid_solid(selected.snapshot)) << ",\n";
    out << "  \"selected\": ";
    write_snapshot(out, selected.snapshot, 2);
    out << ",\n  \"roundtrip\": ";
    if (roundtrip != nullptr) {
        write_snapshot(out, *roundtrip, 2);
        out << "\n";
    } else {
        out << "null\n";
    }
    out << "}\n";
    return true;
}

void print_snapshot(const char* label, const Snapshot& snapshot) {
    std::cout << label
              << " faces=" << snapshot.stats.faces
              << " edges=" << snapshot.stats.edges
              << " shells=" << snapshot.stats.shells
              << " solids=" << snapshot.stats.solids
              << " free=" << snapshot.freeEdges
              << " multiple=" << snapshot.multipleEdges
              << " brep=" << snapshot.brepCheckValid
              << "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }

    const auto base = spo::PatchImportService().importPatch(options.baseStep);
    if (!base.success) {
        std::cerr << "Failed to read base STEP: " << base.errorMessage << "\n";
        return 1;
    }
    const auto patch = spo::PatchImportService().importPatch(options.patchStep);
    if (!patch.success) {
        std::cerr << "Failed to read patch STEP: " << patch.errorMessage << "\n";
        return 1;
    }

    const auto baseSnapshot = snapshot_for(base.shape);
    const auto patchSnapshot = snapshot_for(patch.shape);
    print_snapshot("base", baseSnapshot);
    print_snapshot("patch", patchSnapshot);

    std::vector<Attempt> attempts;
    attempts.reserve(options.tolerances.size());
    try {
        for (const double tolerance : options.tolerances) {
            attempts.push_back(sew_at_tolerance(base.shape, patch.shape, tolerance));
            std::cout << "tolerance_mm=" << tolerance << " ";
            print_snapshot("sewn", attempts.back().snapshot);
        }
    } catch (const Standard_Failure& error) {
        std::cerr << "Sewing failed: " << error.GetMessageString() << "\n";
        return 1;
    }

    const auto* selected = choose_attempt(
        attempts,
        baseSnapshot.stats.faces + patchSnapshot.stats.faces);
    if (selected == nullptr) {
        std::cerr << "Sewing produced no non-empty result.\n";
        return 1;
    }

    if (!options.outputStep.parent_path().empty()) {
        std::filesystem::create_directories(options.outputStep.parent_path());
    }
    const spo::ShapeDocument outputDocument(selected->shape, options.outputStep);
    const auto write = spo::StepWriter().write(outputDocument, options.outputStep);
    const std::string writeMessage = write.success() ? std::string{} : write.message();

    Snapshot roundtripSnapshot;
    const Snapshot* roundtrip = nullptr;
    if (write.success()) {
        const auto reread = spo::PatchImportService().importPatch(options.outputStep);
        if (reread.success) {
            roundtripSnapshot = snapshot_for(reread.shape);
            roundtrip = &roundtripSnapshot;
        }
    }

    if (!write_report(
            options,
            baseSnapshot,
            patchSnapshot,
            attempts,
            *selected,
            roundtrip,
            write.success(),
            writeMessage)) {
        std::cerr << "Failed to write report: " << path_to_utf8(options.report) << "\n";
        return 1;
    }

    std::cout << "selected_tolerance_mm=" << selected->tolerance << "\n";
    print_snapshot("selected", selected->snapshot);
    if (roundtrip != nullptr) {
        print_snapshot("roundtrip", *roundtrip);
    }
    std::cout << "output_step=" << path_to_utf8(options.outputStep) << "\n";
    std::cout << "report=" << path_to_utf8(options.report) << "\n";
    return write.success() ? 0 : 1;
}
