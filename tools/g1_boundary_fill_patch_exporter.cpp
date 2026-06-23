#include "brep/ShapeDocument.h"
#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "merge/MergePlanner.h"
#include "merge/RegionBoundaryAnalyzer.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepFill_Filling.hxx>
#include <BRepGProp.hxx>
#include <GeomAbs_Shape.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Face.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::filesystem::path sourceStep;
    std::filesystem::path outputPatch;
    std::filesystem::path manifest;
    int candidateId = -1;
    bool autoCandidateId = false;
    double angularThresholdDegrees = 25.0;
    double minEdgeLength = 0.0;
    double tolerance3d = 0.03;
    double tolerance2d = 0.00001;
    double toleranceAngle = 0.01;
    double toleranceCurvature = 0.1;
    int pointsOnCurve = 24;
    int iterations = 3;
    int maxDegree = 8;
    int maxSegments = 12;
};

struct CandidateSelection {
    const spo::MergeCandidate* candidate = nullptr;
    spo::RegionBoundaryAnalysis boundary;
    std::string message;
};

struct FillResult {
    bool success = false;
    bool fillingDone = false;
    bool brepCheckValid = false;
    int boundaryEdgeCount = 0;
    int g1SupportEdgeCount = 0;
    int missingSupportEdgeCount = 0;
    std::vector<spo::EdgeId> missingSupportEdgeIds;
    double g0Error = 0.0;
    double g1Error = 0.0;
    double g2Error = 0.0;
    TopoDS_Face face;
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

std::string edge_ids_json(const std::vector<spo::EdgeId>& ids) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index > 0) {
            out << ", ";
        }
        out << ids[index];
    }
    out << "]";
    return out.str();
}

void print_usage() {
    std::cerr
        << "Usage: g1_boundary_fill_patch_exporter --source-step <model.stp> "
        << "--candidate-id <id|auto> --output-patch <patch.stp> [--manifest <manifest.json>] "
        << "[--tolerance3d <value>]\n";
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

        if (arg == "--source-step") {
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
        } else if (arg == "--output-patch") {
            const auto* value = require_value("--output-patch");
            if (value == nullptr) {
                return false;
            }
            options.outputPatch = value;
        } else if (arg == "--manifest") {
            const auto* value = require_value("--manifest");
            if (value == nullptr) {
                return false;
            }
            options.manifest = value;
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
        } else if (arg == "--tolerance3d") {
            const auto* value = require_value("--tolerance3d");
            if (value == nullptr) {
                return false;
            }
            options.tolerance3d = std::stod(value);
        } else if (arg == "--points-on-curve") {
            const auto* value = require_value("--points-on-curve");
            if (value == nullptr) {
                return false;
            }
            options.pointsOnCurve = std::stoi(value);
        } else if (arg == "--iterations") {
            const auto* value = require_value("--iterations");
            if (value == nullptr) {
                return false;
            }
            options.iterations = std::stoi(value);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    return !options.sourceStep.empty() &&
        !options.outputPatch.empty() &&
        (options.autoCandidateId || options.candidateId >= 0);
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

CandidateSelection select_candidate(
    const spo::ShapeDocument& document,
    const std::vector<spo::MergeCandidate>& candidates,
    const Options& options) {
    CandidateSelection selection;
    if (!options.autoCandidateId) {
        for (const auto& candidate : candidates) {
            if (candidate.candidate_id == options.candidateId) {
                selection.candidate = &candidate;
                break;
            }
        }
        if (selection.candidate == nullptr) {
            selection.message = "Requested candidate id was not found.";
            return selection;
        }
        selection.boundary = spo::RegionBoundaryAnalyzer().analyze(document, *selection.candidate);
        if (!selection.boundary.valid) {
            selection.message = "Boundary analysis failed: " + selection.boundary.message;
            selection.candidate = nullptr;
        }
        return selection;
    }

    std::vector<const spo::MergeCandidate*> ranked;
    ranked.reserve(candidates.size());
    for (const auto& candidate : candidates) {
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

    std::string firstFailure;
    for (const auto* candidate : ranked) {
        auto boundary = spo::RegionBoundaryAnalyzer().analyze(document, *candidate);
        if (boundary.valid) {
            selection.candidate = candidate;
            selection.boundary = std::move(boundary);
            selection.message = "Auto-selected largest valid FeatureBoundedRefit candidate.";
            return selection;
        }
        if (firstFailure.empty()) {
            firstFailure = boundary.message;
        }
    }

    selection.message = "Auto candidate selection found no valid boundary candidate.";
    if (!firstFailure.empty()) {
        selection.message += " First boundary failure: " + firstFailure;
    }
    return selection;
}

double edge_length(const TopoDS_Edge& edge) {
    if (edge.IsNull()) {
        return 0.0;
    }
    GProp_GProps props;
    BRepGProp::LinearProperties(edge, props);
    return std::max(0.0, props.Mass());
}

bool candidate_contains_face(const spo::MergeCandidate& candidate, spo::FaceId faceId) {
    return std::find(candidate.faces.begin(), candidate.faces.end(), faceId) != candidate.faces.end();
}

TopoDS_Face support_face_for_boundary_edge(
    const spo::ShapeDocument& document,
    const spo::MergeCandidate& candidate,
    spo::EdgeId edgeId) {
    const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
    if (adjacency == nullptr) {
        return {};
    }
    for (const auto faceId : adjacency->faces) {
        if (!candidate_contains_face(candidate, faceId) &&
            faceId >= 0 &&
            static_cast<std::size_t>(faceId) < document.topology().faceCount()) {
            return document.topology().face(faceId);
        }
    }
    return {};
}

FillResult build_g1_fill_face(
    const spo::ShapeDocument& document,
    const spo::MergeCandidate& candidate,
    const spo::RegionBoundaryAnalysis& boundary,
    const Options& options) {
    FillResult result;
    result.boundaryEdgeCount = static_cast<int>(boundary.ordered_boundary_edges.size());
    if (!boundary.valid || boundary.ordered_boundary_edges.empty()) {
        result.message = "G1 filling requires one valid ordered original CAD boundary.";
        return result;
    }

    BRepFill_Filling filling(
        3,
        options.pointsOnCurve,
        options.iterations,
        Standard_False,
        options.tolerance2d,
        options.tolerance3d,
        options.toleranceAngle,
        options.toleranceCurvature,
        options.maxDegree,
        options.maxSegments);
    filling.SetConstrParam(
        options.tolerance2d,
        options.tolerance3d,
        options.toleranceAngle,
        options.toleranceCurvature);

    for (const auto edgeId : boundary.ordered_boundary_edges) {
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= document.topology().edgeCount()) {
            ++result.missingSupportEdgeCount;
            result.missingSupportEdgeIds.push_back(edgeId);
            continue;
        }
        const auto supportFace = support_face_for_boundary_edge(document, candidate, edgeId);
        if (supportFace.IsNull()) {
            ++result.missingSupportEdgeCount;
            result.missingSupportEdgeIds.push_back(edgeId);
            continue;
        }
        const auto& edge = document.topology().edge(edgeId);
        if (edge.IsNull() || edge_length(edge) <= 0.0) {
            ++result.missingSupportEdgeCount;
            result.missingSupportEdgeIds.push_back(edgeId);
            continue;
        }
        filling.Add(edge, supportFace, GeomAbs_G1, Standard_True);
        ++result.g1SupportEdgeCount;
    }

    if (result.g1SupportEdgeCount != result.boundaryEdgeCount) {
        result.message = "G1 filling rejected because every original boundary edge must have one adjacent non-candidate support face.";
        return result;
    }

    try {
        filling.Build();
    } catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        result.message = std::string("BRepFill_Filling threw: ") + (message != nullptr ? message : "unknown OCCT error");
        return result;
    }

    result.fillingDone = filling.IsDone();
    if (!result.fillingDone) {
        result.message = "BRepFill_Filling did not finish.";
        return result;
    }
    result.g0Error = filling.G0Error();
    result.g1Error = filling.G1Error();
    result.g2Error = filling.G2Error();
    if (result.g0Error > options.tolerance3d) {
        result.message = "G1 filling rejected because G0 boundary error exceeds tolerance3d.";
        return result;
    }
    if (result.g1Error > options.toleranceAngle) {
        result.message = "G1 filling rejected because G1 angular error exceeds toleranceAngle.";
        return result;
    }

    auto face = filling.Face();
    if (face.IsNull()) {
        result.message = "BRepFill_Filling produced a null face.";
        return result;
    }

    ShapeFix_Face fixer(face);
    fixer.SetPrecision(options.tolerance3d);
    fixer.Perform();
    const auto fixedFace = fixer.Face();
    if (!fixedFace.IsNull()) {
        face = fixedFace;
    }

    BRepCheck_Analyzer check(face);
    result.brepCheckValid = check.IsValid();
    if (!result.brepCheckValid) {
        result.message = "G1 filling face failed BRepCheck.";
        return result;
    }

    result.face = face;
    result.success = true;
    result.message = "Built G1 boundary-constrained filling patch from original CAD boundary and adjacent support faces.";
    return result;
}

bool write_manifest(
    const Options& options,
    const spo::MergeCandidate& candidate,
    const spo::RegionBoundaryAnalysis& boundary,
    const FillResult& fill) {
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
    out << "  \"tool\": \"g1_boundary_fill_patch_exporter\",\n";
    out << "  \"source_step\": " << quote_json(path_to_utf8(options.sourceStep)) << ",\n";
    out << "  \"output_patch\": " << quote_json(path_to_utf8(options.outputPatch)) << ",\n";
    out << "  \"candidate_id\": " << candidate.candidate_id << ",\n";
    out << "  \"candidate_face_count\": " << candidate_face_count(candidate) << ",\n";
    out << "  \"boundary_valid\": " << bool_json(boundary.valid) << ",\n";
    out << "  \"boundary_edge_count\": " << fill.boundaryEdgeCount << ",\n";
    out << "  \"constraint_order\": \"G1\",\n";
    out << "  \"g1_support_edge_count\": " << fill.g1SupportEdgeCount << ",\n";
    out << "  \"missing_support_edge_count\": " << fill.missingSupportEdgeCount << ",\n";
    out << "  \"missing_support_edge_ids\": " << edge_ids_json(fill.missingSupportEdgeIds) << ",\n";
    out << "  \"tolerance3d\": " << options.tolerance3d << ",\n";
    out << "  \"filling_done\": " << bool_json(fill.fillingDone) << ",\n";
    out << "  \"success\": " << bool_json(fill.success) << ",\n";
    out << "  \"brep_check_valid\": " << bool_json(fill.brepCheckValid) << ",\n";
    out << "  \"g0_error\": " << fill.g0Error << ",\n";
    out << "  \"g1_error\": " << fill.g1Error << ",\n";
    out << "  \"g2_error\": " << fill.g2Error << ",\n";
    out << "  \"message\": " << quote_json(fill.message) << "\n";
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
    if (!std::filesystem::exists(options.sourceStep)) {
        std::cerr << "Source STEP does not exist: " << path_to_utf8(options.sourceStep) << "\n";
        return 1;
    }

    const auto read = spo::StepReader().read(options.sourceStep);
    if (!read.status.success()) {
        std::cerr << read.status.message() << "\n";
        return 1;
    }
    const auto& document = read.document;

    const auto featureEdges = spo::FeatureEdgeDetector().detect(
        document.topology(),
        options.angularThresholdDegrees,
        options.minEdgeLength);
    const auto planner = spo::MergePlanner().plan(
        document,
        featureEdges,
        {},
        planner_options());
    const auto selection = select_candidate(document, planner.candidates, options);
    if (selection.candidate == nullptr) {
        std::cerr << selection.message << "\n";
        return 1;
    }

    const auto fill = build_g1_fill_face(
        document,
        *selection.candidate,
        selection.boundary,
        options);
    if (!fill.success) {
        write_manifest(options, *selection.candidate, selection.boundary, fill);
        std::cerr << fill.message << "\n";
        return 1;
    }

    std::error_code dirError;
    const auto outputParent = options.outputPatch.parent_path();
    if (!outputParent.empty()) {
        std::filesystem::create_directories(outputParent, dirError);
        if (dirError) {
            std::cerr << "Could not create output directory: " << dirError.message() << "\n";
            return 1;
        }
    }

    spo::ShapeDocument patchDocument(fill.face, options.outputPatch);
    const auto write = spo::StepWriter().write(patchDocument, options.outputPatch);
    if (!write.success()) {
        std::cerr << write.message() << "\n";
        return 1;
    }
    if (!write_manifest(options, *selection.candidate, selection.boundary, fill)) {
        std::cerr << "Could not write manifest: " << path_to_utf8(options.manifest) << "\n";
        return 1;
    }

    std::cout << "G1 boundary fill patch: " << path_to_utf8(options.outputPatch) << "\n";
    std::cout << "candidate_id: " << selection.candidate->candidate_id << "\n";
    std::cout << "g0_error: " << fill.g0Error << "\n";
    std::cout << "g1_error: " << fill.g1Error << "\n";
    return 0;
}
