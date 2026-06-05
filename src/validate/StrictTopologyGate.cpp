#include "validate/StrictTopologyGate.h"

#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "patch/PatchReplacementReport.h"
#include "validate/ShapeValidator.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>

namespace spo {

namespace {

struct BBoxInfo {
    bool valid = false;
    double minX = 0.0;
    double minY = 0.0;
    double minZ = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    double maxZ = 0.0;
};

void append_warning(StrictTopologyGateReport& report, const std::string& warning) {
    if (warning.empty()) {
        return;
    }
    if (!report.warningMessage.empty()) {
        report.warningMessage += " ";
    }
    report.warningMessage += warning;
}

StrictTopologyGateReport fail(
    StrictTopologyGateReport report,
    StrictTopologyFailureReason reason,
    std::string message) {
    report.failureReason = reason;
    report.message = std::move(message);
    return report;
}

BBoxInfo compute_bbox(const ShapeDocument& document) {
    BBoxInfo info;
    if (!document.hasShape()) {
        return info;
    }

    Bnd_Box box;
    BRepBndLib::Add(document.shape(), box);
    if (box.IsVoid()) {
        return info;
    }

    box.Get(info.minX, info.minY, info.minZ, info.maxX, info.maxY, info.maxZ);
    info.valid = true;
    return info;
}

double diagonal(const BBoxInfo& box) {
    const auto dx = box.maxX - box.minX;
    const auto dy = box.maxY - box.minY;
    const auto dz = box.maxZ - box.minZ;
    if (dx < 0.0 || dy < 0.0 || dz < 0.0) {
        return 0.0;
    }
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double center_distance(const BBoxInfo& before, const BBoxInfo& after) {
    const auto beforeX = (before.minX + before.maxX) * 0.5;
    const auto beforeY = (before.minY + before.maxY) * 0.5;
    const auto beforeZ = (before.minZ + before.maxZ) * 0.5;
    const auto afterX = (after.minX + after.maxX) * 0.5;
    const auto afterY = (after.minY + after.maxY) * 0.5;
    const auto afterZ = (after.minZ + after.maxZ) * 0.5;
    const auto dx = afterX - beforeX;
    const auto dy = afterY - beforeY;
    const auto dz = afterZ - beforeZ;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool bbox_deviation_too_large(const BBoxInfo& before, const BBoxInfo& after) {
    const auto beforeDiagonal = diagonal(before);
    const auto afterDiagonal = diagonal(after);
    if (beforeDiagonal <= 0.0 || afterDiagonal <= 0.0) {
        return true;
    }

    const auto ratio = afterDiagonal / beforeDiagonal;
    return ratio < 0.25 || ratio > 4.0 || center_distance(before, after) / beforeDiagonal > 1.0;
}

std::filesystem::path default_step_path() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "spo_strict_topology_gate_" << now << ".stp";
    return std::filesystem::temp_directory_path() / name.str();
}

bool ensure_parent_directory(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    return !error;
}

struct StepRoundtripValidation {
    bool readable = false;
    ShapeStats stats;
    int freeEdges = 0;
    int multipleEdges = 0;
    bool brepCheckValid = false;
};

StepRoundtripValidation validate_step_roundtrip(const std::filesystem::path& path) {
    StepRoundtripValidation result;
    const StepReader reader;
    const auto read = reader.read(path);
    if (!read.status.success() || !read.document.hasShape()) {
        return result;
    }

    const auto validation = ShapeValidator().validate(read.document);
    result.readable = validation.has_shape;
    result.stats = validation.stats;
    result.freeEdges = validation.free_edges;
    result.multipleEdges = validation.multiple_edges;
    result.brepCheckValid = validation.brep_check_valid;
    return result;
}

}

const char* toString(StrictTopologyFailureReason reason) {
    switch (reason) {
    case StrictTopologyFailureReason::None:
        return "None";
    case StrictTopologyFailureReason::MissingBeforeShape:
        return "MissingBeforeShape";
    case StrictTopologyFailureReason::MissingAfterShape:
        return "MissingAfterShape";
    case StrictTopologyFailureReason::BRepCheckFailed:
        return "BRepCheckFailed";
    case StrictTopologyFailureReason::FreeEdgeIncreased:
        return "FreeEdgeIncreased";
    case StrictTopologyFailureReason::MultipleEdgeIncreased:
        return "MultipleEdgeIncreased";
    case StrictTopologyFailureReason::SolidCountChangedUnexpectedly:
        return "SolidCountChangedUnexpectedly";
    case StrictTopologyFailureReason::ShellCountInvalid:
        return "ShellCountInvalid";
    case StrictTopologyFailureReason::BBoxInvalid:
        return "BBoxInvalid";
    case StrictTopologyFailureReason::BBoxDeviationTooLarge:
        return "BBoxDeviationTooLarge";
    case StrictTopologyFailureReason::StepExportFailed:
        return "StepExportFailed";
    case StrictTopologyFailureReason::StepRoundtripFailed:
        return "StepRoundtripFailed";
    case StrictTopologyFailureReason::ReplacementStatsInvalid:
        return "ReplacementStatsInvalid";
    }
    return "Unknown";
}

StrictTopologyGateReport StrictTopologyGate::evaluate(const StrictTopologyGateInput& input) const {
    StrictTopologyGateReport report;
    report.watertightSolidRequired = input.requireWatertightSolid;
    report.roundtripWatertightRequired = input.requireRoundtripWatertight;

    if (input.replacementReport != nullptr) {
        report.replacementFaceCount = input.replacementReport->replacementFaceCount;
        report.multiFaceReplacement = report.replacementFaceCount > 1;
    }

    if (input.beforeDocument == nullptr || !input.beforeDocument->hasShape()) {
        return fail(report, StrictTopologyFailureReason::MissingBeforeShape, "Strict topology gate requires a before document with a shape.");
    }
    if (input.afterDocument == nullptr || !input.afterDocument->hasShape()) {
        return fail(report, StrictTopologyFailureReason::MissingAfterShape, "Strict topology gate requires an after document with a shape.");
    }

    const auto beforeValidation = ShapeValidator().validate(*input.beforeDocument);
    const auto afterValidation = ShapeValidator().validate(*input.afterDocument);
    report.beforeStats = beforeValidation.stats;
    report.afterStats = afterValidation.stats;
    report.beforeFreeEdges = beforeValidation.free_edges;
    report.afterFreeEdges = afterValidation.free_edges;
    report.beforeMultipleEdges = beforeValidation.multiple_edges;
    report.afterMultipleEdges = afterValidation.multiple_edges;
    report.beforeBRepCheckValid = beforeValidation.brep_check_valid;
    report.afterBRepCheckValid = afterValidation.brep_check_valid;

    if (!report.afterBRepCheckValid) {
        return fail(report, StrictTopologyFailureReason::BRepCheckFailed, "After document failed BRepCheck.");
    }
    if (report.afterFreeEdges > report.beforeFreeEdges) {
        return fail(report, StrictTopologyFailureReason::FreeEdgeIncreased, "After document has more free edges than before document.");
    }
    if (input.requireZeroFreeEdges && report.afterFreeEdges != 0) {
        return fail(report, StrictTopologyFailureReason::FreeEdgeIncreased, "After document has free edges, but watertight apply requires zero free edges.");
    }
    if (report.afterMultipleEdges > report.beforeMultipleEdges) {
        return fail(report, StrictTopologyFailureReason::MultipleEdgeIncreased, "After document has more multiple edges than before document.");
    }
    if (input.requireZeroMultipleEdges && report.afterMultipleEdges != 0) {
        return fail(report, StrictTopologyFailureReason::MultipleEdgeIncreased, "After document has multiple edges, but watertight apply requires zero multiple edges.");
    }
    if (report.afterStats.solids != report.beforeStats.solids) {
        return fail(report, StrictTopologyFailureReason::SolidCountChangedUnexpectedly, "After document solid count changed unexpectedly.");
    }
    if (input.requireWatertightSolid && report.beforeStats.solids > 0 && report.afterStats.solids <= 0) {
        return fail(report, StrictTopologyFailureReason::SolidCountChangedUnexpectedly, "Before document is a solid model, but after document has no solids.");
    }
    if (report.beforeStats.shells > 0 && report.afterStats.shells <= 0) {
        return fail(report, StrictTopologyFailureReason::ShellCountInvalid, "After document shell count is invalid.");
    }

    const auto beforeBBox = compute_bbox(*input.beforeDocument);
    const auto afterBBox = compute_bbox(*input.afterDocument);
    if (!beforeBBox.valid || !afterBBox.valid) {
        return fail(report, StrictTopologyFailureReason::BBoxInvalid, "Before or after document bounding box is invalid.");
    }
    if (bbox_deviation_too_large(beforeBBox, afterBBox)) {
        return fail(report, StrictTopologyFailureReason::BBoxDeviationTooLarge, "After document bounding box deviates too much from before document.");
    }

    if (input.replacementReport != nullptr) {
        if (report.replacementFaceCount < 0) {
            return fail(report, StrictTopologyFailureReason::ReplacementStatsInvalid, "Replacement face count is invalid.");
        }
        if (report.multiFaceReplacement) {
            if (!input.allowMultiFaceReplacement) {
                return fail(report, StrictTopologyFailureReason::ReplacementStatsInvalid, "Multi-face replacement is not allowed by this gate input.");
            }
            append_warning(report, "Accepted multi-face replacement fragment.");
        }
    }

    if (!input.allowFaceCountIncrease && report.afterStats.faces > report.beforeStats.faces) {
        return fail(report, StrictTopologyFailureReason::ReplacementStatsInvalid, "After document face count increased.");
    }
    if (report.afterStats.faces >= report.beforeStats.faces) {
        append_warning(report, "Face count did not decrease; this is allowed for multi-face replacement gate evaluation.");
    }

    if (input.requireStepRoundtrip) {
        const auto requestedPath = !input.temporaryStepPath.empty();
        const auto stepPath = requestedPath ? input.temporaryStepPath : default_step_path();
        if (!ensure_parent_directory(stepPath)) {
            return fail(report, StrictTopologyFailureReason::StepExportFailed, "Could not create temporary STEP export directory.");
        }

        const auto exportResult = StepWriter().write(*input.afterDocument, stepPath);
        report.stepExportOk = exportResult.success();
        if (!report.stepExportOk) {
            return fail(report, StrictTopologyFailureReason::StepExportFailed, exportResult.message());
        }

        const auto roundtrip = validate_step_roundtrip(stepPath);
        report.roundtripStats = roundtrip.stats;
        report.roundtripFreeEdges = roundtrip.freeEdges;
        report.roundtripMultipleEdges = roundtrip.multipleEdges;
        report.roundtripBRepCheckValid = roundtrip.brepCheckValid;
        report.stepRoundtripOk = roundtrip.readable && roundtrip.brepCheckValid;
        if (!report.stepRoundtripOk) {
            if (!requestedPath) {
                std::error_code error;
                std::filesystem::remove(stepPath, error);
            }
            return fail(report, StrictTopologyFailureReason::StepRoundtripFailed, "Exported STEP failed roundtrip readback validation.");
        }
        if (input.requireRoundtripWatertight) {
            if (report.beforeStats.solids > 0 && report.roundtripStats.solids != report.beforeStats.solids) {
                if (!requestedPath) {
                    std::error_code error;
                    std::filesystem::remove(stepPath, error);
                }
                return fail(report, StrictTopologyFailureReason::StepRoundtripFailed, "Roundtrip STEP solid count does not match the before document.");
            }
            if (report.roundtripFreeEdges != 0) {
                if (!requestedPath) {
                    std::error_code error;
                    std::filesystem::remove(stepPath, error);
                }
                return fail(report, StrictTopologyFailureReason::StepRoundtripFailed, "Roundtrip STEP has free edges, but watertight apply requires zero free edges.");
            }
            if (report.roundtripMultipleEdges != 0) {
                if (!requestedPath) {
                    std::error_code error;
                    std::filesystem::remove(stepPath, error);
                }
                return fail(report, StrictTopologyFailureReason::StepRoundtripFailed, "Roundtrip STEP has multiple edges, but watertight apply requires zero multiple edges.");
            }
        }

        if (!requestedPath) {
            std::error_code error;
            std::filesystem::remove(stepPath, error);
        }
    }

    report.passed = true;
    report.failureReason = StrictTopologyFailureReason::None;
    report.message = "Strict topology gate passed.";
    return report;
}

}
