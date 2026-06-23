#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "merge/MergePlanner.h"
#include "merge/RegionBoundaryAnalyzer.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <Interface_Static.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

struct Options {
    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path manifest;
    std::filesystem::path targetStep;
    std::filesystem::path sourceStep;
    int candidateId = -1;
    bool autoCandidateId = false;
    double angularThresholdDegrees = 25.0;
    double minEdgeLength = 0.0;
};

struct BoxInfo {
    bool valid = false;
    double minX = 0.0;
    double minY = 0.0;
    double minZ = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    double maxZ = 0.0;
};

struct StepUnitInfo {
    bool scanned = false;
    std::string unit = "unknown";
    std::string evidence;
    int lineNumber = 0;
};

struct CandidateTarget {
    bool valid = false;
    int candidateId = -1;
    BoxInfo bbox;
    std::string message;
};

struct AlignmentResult {
    bool applied = false;
    bool targetCandidateUsed = false;
    double scaleFactor = 1.0;
    double translationX = 0.0;
    double translationY = 0.0;
    double translationZ = 0.0;
    double rawBboxScaleRatio = 1.0;
    BoxInfo inputBBox;
    BoxInfo targetStepBBox;
    BoxInfo targetCandidateBBox;
    BoxInfo outputBBox;
    double centerOffsetBefore = 0.0;
    double centerOffsetAfter = 0.0;
    std::string method = "read_write_only";
    std::string message;
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

std::string compact_upper_ascii(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if (!std::isspace(ch)) {
            result.push_back(static_cast<char>(std::toupper(ch)));
        }
    }
    return result;
}

bool unit_is_millimeter(const StepUnitInfo& info) {
    return info.unit == "millimeter";
}

std::string unit_json(const StepUnitInfo& info, const int indent) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    std::ostringstream out;
    out << "{\n";
    out << pad << "  \"scanned\": " << bool_json(info.scanned) << ",\n";
    out << pad << "  \"unit\": " << quote_json(info.unit) << ",\n";
    out << pad << "  \"line\": " << info.lineNumber << ",\n";
    out << pad << "  \"evidence\": " << quote_json(info.evidence) << "\n";
    out << pad << "}";
    return out.str();
}

StepUnitInfo detect_step_text_unit(const std::filesystem::path& path) {
    StepUnitInfo result;
    if (path.empty()) {
        result.evidence = "empty path";
        return result;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        result.evidence = "file not readable";
        return result;
    }

    result.scanned = true;
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        const auto compact = compact_upper_ascii(line);
        if (compact.find("SI_UNIT(") == std::string::npos ||
            compact.find(".METRE.") == std::string::npos) {
            continue;
        }

        result.lineNumber = lineNumber;
        result.evidence = line;
        if (compact.find(".MILLI.") != std::string::npos) {
            result.unit = "millimeter";
        } else if (compact.find(".CENTI.") != std::string::npos) {
            result.unit = "centimeter";
        } else if (compact.find(".MICRO.") != std::string::npos) {
            result.unit = "micrometer";
        } else if (compact.find(".KILO.") != std::string::npos) {
            result.unit = "kilometer";
        } else {
            result.unit = "meter";
        }
        return result;
    }

    result.unit = "unknown";
    result.evidence = "no SI_UNIT(... .METRE.) line found";
    return result;
}

class OcctStepWriteUnitGuard {
public:
    OcctStepWriteUnitGuard() {
        if (const auto* value = Interface_Static::CVal("write.step.unit")) {
            previousUnit_ = value;
            hadPreviousUnit_ = true;
        }
        Interface_Static::SetCVal("write.step.unit", "MM");
        if (const auto* value = Interface_Static::CVal("write.step.schema")) {
            previousSchema_ = value;
            hadPreviousSchema_ = true;
        }
        Interface_Static::SetCVal("write.step.schema", "AP214");
    }

    ~OcctStepWriteUnitGuard() {
        if (hadPreviousUnit_) {
            Interface_Static::SetCVal("write.step.unit", previousUnit_.c_str());
        }
        if (hadPreviousSchema_) {
            Interface_Static::SetCVal("write.step.schema", previousSchema_.c_str());
        }
    }

private:
    bool hadPreviousUnit_ = false;
    bool hadPreviousSchema_ = false;
    std::string previousUnit_;
    std::string previousSchema_;
};

void print_usage() {
    std::cerr
        << "Usage: step_unit_normalizer --input <in.stp> --output <out.stp> "
        << "[--manifest <manifest.json>] "
        << "[--target-step <base_removed_candidate.stp>] "
        << "[--source-step <original.stp> --candidate-id <id|auto>]\n";
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

        if (arg == "--input") {
            const auto* value = require_value("--input");
            if (value == nullptr) {
                return false;
            }
            options.input = value;
        } else if (arg == "--output") {
            const auto* value = require_value("--output");
            if (value == nullptr) {
                return false;
            }
            options.output = value;
        } else if (arg == "--manifest") {
            const auto* value = require_value("--manifest");
            if (value == nullptr) {
                return false;
            }
            options.manifest = value;
        } else if (arg == "--target-step") {
            const auto* value = require_value("--target-step");
            if (value == nullptr) {
                return false;
            }
            options.targetStep = value;
        } else if (arg == "--source-step") {
            const auto* value = require_value("--source-step");
            if (value == nullptr) {
                return false;
            }
            options.sourceStep = value;
        } else if (arg == "--candidate-id") {
            const auto* value = require_value("--candidate-id");
            if (value == nullptr) {
                return false;
            }
            const std::string candidateValue(value);
            if (candidateValue == "auto") {
                options.autoCandidateId = true;
                options.candidateId = -1;
            } else {
                options.autoCandidateId = false;
                options.candidateId = std::stoi(candidateValue);
            }
        } else if (arg == "--angle") {
            const auto* value = require_value("--angle");
            if (value == nullptr) {
                return false;
            }
            options.angularThresholdDegrees = std::stod(value);
        } else if (arg == "--min-edge-length") {
            const auto* value = require_value("--min-edge-length");
            if (value == nullptr) {
                return false;
            }
            options.minEdgeLength = std::stod(value);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    return !options.input.empty() && !options.output.empty();
}

BoxInfo bbox_from_shape(const TopoDS_Shape& shape) {
    BoxInfo info;
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) {
        return info;
    }
    box.Get(info.minX, info.minY, info.minZ, info.maxX, info.maxY, info.maxZ);
    info.valid = std::isfinite(info.minX) &&
        std::isfinite(info.minY) &&
        std::isfinite(info.minZ) &&
        std::isfinite(info.maxX) &&
        std::isfinite(info.maxY) &&
        std::isfinite(info.maxZ);
    return info;
}

gp_Pnt bbox_center(const BoxInfo& box) {
    return {
        (box.minX + box.maxX) * 0.5,
        (box.minY + box.maxY) * 0.5,
        (box.minZ + box.maxZ) * 0.5};
}

double bbox_diagonal(const BoxInfo& box) {
    if (!box.valid) {
        return 0.0;
    }
    const auto dx = box.maxX - box.minX;
    const auto dy = box.maxY - box.minY;
    const auto dz = box.maxZ - box.minZ;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string bbox_json(const BoxInfo& box, const int indent) {
    if (!box.valid) {
        return "null";
    }

    const std::string pad(static_cast<std::size_t>(indent), ' ');

    std::ostringstream out;
    out << "{\n";
    out << pad << "  \"min_x\": " << box.minX << ",\n";
    out << pad << "  \"min_y\": " << box.minY << ",\n";
    out << pad << "  \"min_z\": " << box.minZ << ",\n";
    out << pad << "  \"max_x\": " << box.maxX << ",\n";
    out << pad << "  \"max_y\": " << box.maxY << ",\n";
    out << pad << "  \"max_z\": " << box.maxZ << ",\n";
    out << pad << "  \"diagonal\": " << bbox_diagonal(box) << "\n";
    out << pad << "}";
    return out.str();
}

int candidate_face_count(const spo::MergeCandidate& candidate) {
    if (candidate.face_count > 0) {
        return candidate.face_count;
    }
    return static_cast<int>(candidate.faces.size());
}

int candidate_boundary_edge_count(const spo::MergeCandidate& candidate) {
    if (candidate.boundary_edge_count > 0) {
        return candidate.boundary_edge_count;
    }
    return static_cast<int>(candidate.boundary_edges.size());
}

spo::MergePlannerOptions planner_options() {
    spo::MergePlannerOptions options;
    options.enable_feature_bounded_refit_candidates = true;
    options.min_feature_bounded_region_faces = 2;
    return options;
}

BoxInfo candidate_bbox(
    const spo::ShapeDocument& document,
    const spo::MergeCandidate& candidate) {
    Bnd_Box box;
    bool added = false;
    for (const auto faceId : candidate.faces) {
        if (faceId < 0 || static_cast<std::size_t>(faceId) >= document.topology().faceCount()) {
            continue;
        }
        BRepBndLib::Add(document.topology().face(faceId), box);
        added = true;
    }
    if (!added || box.IsVoid()) {
        return {};
    }

    BoxInfo info;
    box.Get(info.minX, info.minY, info.minZ, info.maxX, info.maxY, info.maxZ);
    info.valid = true;
    return info;
}

CandidateTarget select_candidate_target(
    const spo::ShapeDocument& document,
    const Options& options) {
    CandidateTarget result;
    const auto featureEdges = spo::FeatureEdgeDetector().detect(
        document.topology(),
        options.angularThresholdDegrees,
        options.minEdgeLength);
    const auto planner = spo::MergePlanner().plan(
        document,
        featureEdges,
        {},
        planner_options());

    const spo::MergeCandidate* selected = nullptr;
    if (!options.autoCandidateId) {
        for (const auto& candidate : planner.candidates) {
            if (candidate.candidate_id == options.candidateId) {
                selected = &candidate;
                break;
            }
        }
        if (selected == nullptr) {
            result.message = "Requested candidate id was not found.";
            return result;
        }
    } else {
        std::vector<const spo::MergeCandidate*> ranked;
        for (const auto& candidate : planner.candidates) {
            if (candidate.valid &&
                candidate.candidate_type == spo::MergeCandidateType::FeatureBoundedRefit &&
                candidate.status != spo::MergeCandidateStatus::Rejected &&
                candidate.status != spo::MergeCandidateStatus::Hidden) {
                ranked.push_back(&candidate);
            }
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto* lhs, const auto* rhs) {
            const auto lhsFaces = candidate_face_count(*lhs);
            const auto rhsFaces = candidate_face_count(*rhs);
            if (lhsFaces != rhsFaces) {
                return lhsFaces > rhsFaces;
            }
            const auto lhsBoundary = candidate_boundary_edge_count(*lhs);
            const auto rhsBoundary = candidate_boundary_edge_count(*rhs);
            if (lhsBoundary != rhsBoundary) {
                return lhsBoundary > rhsBoundary;
            }
            return lhs->candidate_id < rhs->candidate_id;
        });
        for (const auto* candidate : ranked) {
            const auto boundary = spo::RegionBoundaryAnalyzer().analyze(document, *candidate);
            if (boundary.valid) {
                selected = candidate;
                break;
            }
        }
        if (selected == nullptr) {
            result.message = "Auto candidate selection found no valid boundary candidate.";
            return result;
        }
    }

    result.candidateId = selected->candidate_id;
    result.bbox = candidate_bbox(document, *selected);
    result.valid = result.bbox.valid;
    result.message = result.valid
        ? "Selected candidate bbox as alignment target."
        : "Selected candidate bbox is invalid.";
    return result;
}

TopoDS_Shape align_shape_to_box(
    const TopoDS_Shape& inputShape,
    const BoxInfo& inputBox,
    const BoxInfo& targetBox,
    AlignmentResult& result) {
    const auto inputDiagonal = bbox_diagonal(inputBox);
    const auto targetDiagonal = bbox_diagonal(targetBox);
    if (inputDiagonal <= 0.0 || targetDiagonal <= 0.0) {
        result.message = "Input or target bounding-box diagonal is zero.";
        return {};
    }

    result.rawBboxScaleRatio = targetDiagonal / inputDiagonal;
    result.scaleFactor = 1.0;
    if (result.rawBboxScaleRatio > 100.0 || result.rawBboxScaleRatio < 0.01) {
        result.scaleFactor = result.rawBboxScaleRatio;
    }

    const auto originalCenter = bbox_center(inputBox);
    const auto targetCenter = bbox_center(targetBox);
    const double originalCenterDistance = originalCenter.Distance(targetCenter);
    result.centerOffsetBefore = originalCenterDistance;
    const bool needsScale = std::abs(result.scaleFactor - 1.0) > 1.0e-12;
    const bool needsLargeTranslation = originalCenterDistance > targetDiagonal * 0.25;
    if (!needsScale && !needsLargeTranslation) {
        result.applied = false;
        result.method = "already_in_target_candidate_space";
        result.message = "Bbox ratio and center offset do not indicate unit or coordinate mismatch; preserved patch STEP coordinates.";
        return inputShape;
    }

    auto transformed = inputShape;
    if (needsScale) {
        gp_Trsf scaleTransform;
        scaleTransform.SetScale(bbox_center(inputBox), result.scaleFactor);
        transformed = BRepBuilderAPI_Transform(transformed, scaleTransform, true).Shape();
    }

    const auto scaledBox = bbox_from_shape(transformed);
    if (!scaledBox.valid) {
        result.message = "Scaled shape bounding box is invalid.";
        return {};
    }

    const auto scaledCenter = bbox_center(scaledBox);
    gp_Vec translation(scaledCenter, targetCenter);
    result.translationX = translation.X();
    result.translationY = translation.Y();
    result.translationZ = translation.Z();

    if (translation.Magnitude() > 1.0e-12) {
        gp_Trsf translationTransform;
        translationTransform.SetTranslation(translation);
        transformed = BRepBuilderAPI_Transform(transformed, translationTransform, true).Shape();
    }

    result.applied = true;
    result.method = needsScale
        ? "unit_scale_then_center_translation_to_original_candidate_bbox"
        : "large_offset_center_translation_to_original_candidate_bbox";
    result.message = needsScale
        ? "Detected a unit-scale mismatch from bbox ratio and aligned by uniform scale plus center translation."
        : "Detected a large coordinate offset and aligned by center translation only.";
    return transformed;
}

bool write_manifest(
    const Options& options,
    const spo::ShapeDocument& document,
    const AlignmentResult& alignment,
    const StepUnitInfo& sourceUnit,
    const StepUnitInfo& targetStepUnit,
    const StepUnitInfo& rawPatchUnit,
    const StepUnitInfo& normalizedPatchUnit,
    const bool success,
    const std::string& failureReason) {
    if (options.manifest.empty()) {
        return true;
    }

    const auto parent = options.manifest.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream out(options.manifest, std::ios::binary);
    if (!out.good()) {
        return false;
    }

    out << "{\n";
    out << "  \"tool\": \"step_unit_normalizer\",\n";
    out << "  \"method\": " << quote_json(alignment.method) << ",\n";
    out << "  \"input\": " << quote_json(path_to_utf8(options.input)) << ",\n";
    out << "  \"output\": " << quote_json(path_to_utf8(options.output)) << ",\n";
    out << "  \"target_step\": " << quote_json(path_to_utf8(options.targetStep)) << ",\n";
    out << "  \"source_step\": " << quote_json(path_to_utf8(options.sourceStep)) << ",\n";
    out << "  \"candidate_id\": " << options.candidateId << ",\n";
    out << "  \"source_unit\": " << quote_json(sourceUnit.unit) << ",\n";
    out << "  \"target_step_unit\": " << quote_json(targetStepUnit.unit) << ",\n";
    out << "  \"raw_patch_unit\": " << quote_json(rawPatchUnit.unit) << ",\n";
    out << "  \"normalized_patch_unit\": " << quote_json(normalizedPatchUnit.unit) << ",\n";
    out << "  \"forced_output_unit\": \"millimeter\",\n";
    out << "  \"normalized_patch_unit_is_millimeter\": " << bool_json(unit_is_millimeter(normalizedPatchUnit)) << ",\n";
    out << "  \"alignment_applied\": " << bool_json(alignment.applied) << ",\n";
    out << "  \"target_candidate_used\": " << bool_json(alignment.targetCandidateUsed) << ",\n";
    out << "  \"raw_bbox_scale_ratio\": " << alignment.rawBboxScaleRatio << ",\n";
    out << "  \"scale_factor\": " << alignment.scaleFactor << ",\n";
    out << "  \"center_offset_before\": " << alignment.centerOffsetBefore << ",\n";
    out << "  \"center_offset_after\": " << alignment.centerOffsetAfter << ",\n";
    out << "  \"translation\": {\n";
    out << "    \"x\": " << alignment.translationX << ",\n";
    out << "    \"y\": " << alignment.translationY << ",\n";
    out << "    \"z\": " << alignment.translationZ << "\n";
    out << "  },\n";
    out << "  \"message\": " << quote_json(alignment.message) << ",\n";
    out << "  \"success\": " << bool_json(success) << ",\n";
    out << "  \"failure_reason\": " << quote_json(failureReason) << ",\n";
    out << "  \"units\": {\n";
    out << "    \"source\": " << unit_json(sourceUnit, 4) << ",\n";
    out << "    \"target_step\": " << unit_json(targetStepUnit, 4) << ",\n";
    out << "    \"raw_patch\": " << unit_json(rawPatchUnit, 4) << ",\n";
    out << "    \"normalized_patch\": " << unit_json(normalizedPatchUnit, 4) << "\n";
    out << "  },\n";
    out << "  \"stats\": {\n";
    out << "    \"solids\": " << document.stats().solids << ",\n";
    out << "    \"shells\": " << document.stats().shells << ",\n";
    out << "    \"faces\": " << document.stats().faces << ",\n";
    out << "    \"edges\": " << document.stats().edges << ",\n";
    out << "    \"vertices\": " << document.stats().vertices << "\n";
    out << "  },\n";
    out << "  \"input_bbox\": " << bbox_json(alignment.inputBBox, 2) << ",\n";
    out << "  \"target_step_bbox\": " << bbox_json(alignment.targetStepBBox, 2) << ",\n";
    out << "  \"target_candidate_bbox\": " << bbox_json(alignment.targetCandidateBBox, 2) << ",\n";
    out << "  \"output_bbox\": " << bbox_json(alignment.outputBBox, 2) << "\n";
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
    if (!std::filesystem::exists(options.input)) {
        std::cerr << "Input STEP does not exist: " << path_to_utf8(options.input) << "\n";
        return 1;
    }
    if (!options.targetStep.empty() && !std::filesystem::exists(options.targetStep)) {
        std::cerr << "Target STEP does not exist: " << path_to_utf8(options.targetStep) << "\n";
        return 1;
    }
    if (!options.sourceStep.empty() && !std::filesystem::exists(options.sourceStep)) {
        std::cerr << "Source STEP does not exist: " << path_to_utf8(options.sourceStep) << "\n";
        return 1;
    }
    if (!options.sourceStep.empty() && !options.autoCandidateId && options.candidateId < 0) {
        std::cerr << "--candidate-id is required when --source-step is provided.\n";
        return 2;
    }

    const auto rawPatchUnit = detect_step_text_unit(options.input);
    const auto targetStepUnit = detect_step_text_unit(options.targetStep);
    const auto sourceUnit = detect_step_text_unit(options.sourceStep);

    const auto read = spo::StepReader().read(options.input);
    if (!read.status.success()) {
        std::cerr << read.status.message() << "\n";
        return 1;
    }

    AlignmentResult alignment;
    alignment.inputBBox = bbox_from_shape(read.document.shape());
    TopoDS_Shape outputShape = read.document.shape();
    if (!options.targetStep.empty()) {
        const auto targetRead = spo::StepReader().read(options.targetStep);
        if (!targetRead.status.success()) {
            std::cerr << targetRead.status.message() << "\n";
            return 1;
        }
        alignment.targetStepBBox = bbox_from_shape(targetRead.document.shape());
    }
    if (!options.sourceStep.empty()) {
        const auto sourceRead = spo::StepReader().read(options.sourceStep);
        if (!sourceRead.status.success()) {
            std::cerr << sourceRead.status.message() << "\n";
            return 1;
        }
        const auto target = select_candidate_target(sourceRead.document, options);
        alignment.targetCandidateBBox = target.bbox;
        alignment.targetCandidateUsed = target.valid;
        if (!target.valid) {
            std::cerr << target.message << "\n";
            return 1;
        }
        outputShape = align_shape_to_box(
            read.document.shape(),
            alignment.inputBBox,
            alignment.targetCandidateBBox,
            alignment);
        if (outputShape.IsNull()) {
            std::cerr << alignment.message << "\n";
            return 1;
        }
        options.candidateId = target.candidateId;
    } else if (!options.targetStep.empty() && alignment.targetStepBBox.valid) {
        outputShape = align_shape_to_box(
            read.document.shape(),
            alignment.inputBBox,
            alignment.targetStepBBox,
            alignment);
        if (outputShape.IsNull()) {
            std::cerr << alignment.message << "\n";
            return 1;
        }
    } else {
        alignment.message = "No alignment target was supplied; wrote the STEP after OCCT read/write only.";
    }
    alignment.outputBBox = bbox_from_shape(outputShape);
    const auto* centerTargetBox = alignment.targetCandidateBBox.valid
        ? &alignment.targetCandidateBBox
        : (alignment.targetStepBBox.valid ? &alignment.targetStepBBox : nullptr);
    if (centerTargetBox != nullptr && alignment.outputBBox.valid) {
        alignment.centerOffsetAfter = bbox_center(alignment.outputBBox).Distance(bbox_center(*centerTargetBox));
    }

    std::error_code dirError;
    const auto outputParent = options.output.parent_path();
    if (!outputParent.empty()) {
        std::filesystem::create_directories(outputParent, dirError);
        if (dirError) {
            std::cerr << "Could not create output directory: " << dirError.message() << "\n";
            return 1;
        }
    }

    const spo::ShapeDocument outputDocument(outputShape, options.output);
    {
        OcctStepWriteUnitGuard writeUnitGuard;
        const auto write = spo::StepWriter().write(outputDocument, options.output);
        if (!write.success()) {
            std::cerr << write.message() << "\n";
            return 1;
        }
    }
    const auto normalizedPatchUnit = detect_step_text_unit(options.output);
    const bool normalizedUnitOk = unit_is_millimeter(normalizedPatchUnit);
    const auto failureReason = normalizedUnitOk
        ? std::string()
        : "Normalized STEP text unit is not millimeter; refusing to continue to Route 2 sewing.";

    if (!write_manifest(
            options,
            outputDocument,
            alignment,
            sourceUnit,
            targetStepUnit,
            rawPatchUnit,
            normalizedPatchUnit,
            normalizedUnitOk,
            failureReason)) {
        std::cerr << "Could not write manifest: " << path_to_utf8(options.manifest) << "\n";
        return 1;
    }
    if (!normalizedUnitOk) {
        std::cerr << failureReason << " Detected unit: " << normalizedPatchUnit.unit << "\n";
        return 1;
    }

    std::cout << "normalized STEP: " << path_to_utf8(options.output) << "\n";
    std::cout << "units: source=" << sourceUnit.unit
              << " raw_patch=" << rawPatchUnit.unit
              << " target_step=" << targetStepUnit.unit
              << " normalized_patch=" << normalizedPatchUnit.unit << "\n";
    return 0;
}
