#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "merge/MergePlanner.h"
#include "merge/RegionBoundaryAnalyzer.h"

#include <BRepTools_ReShape.hxx>
#include <Standard_Failure.hxx>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::filesystem::path sourceStep;
    std::filesystem::path baseRemovedOutput;
    std::filesystem::path manifest;
    int candidateId = -1;
    bool autoCandidateId = false;
    double angularThresholdDegrees = 25.0;
    double minEdgeLength = 0.0;
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
        << "Usage: creo_phase1_input_exporter --source-step <model.stp> "
        << "--candidate-id <id|auto> --base-removed-output <base_removed_candidate.stp> "
        << "[--manifest <manifest.json>] [--angle <degrees>] [--min-edge-length <value>]\n";
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
        } else if (arg == "--base-removed-output") {
            const auto* value = require_value("--base-removed-output");
            if (value == nullptr) {
                return false;
            }
            options.baseRemovedOutput = value;
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
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    return !options.sourceStep.empty() &&
        !options.baseRemovedOutput.empty() &&
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

const spo::MergeCandidate* find_candidate(
    const std::vector<spo::MergeCandidate>& candidates,
    const int candidateId) {
    for (const auto& candidate : candidates) {
        if (candidate.candidate_id == candidateId) {
            return &candidate;
        }
    }
    return nullptr;
}

const spo::MergeCandidate* select_auto_candidate(
    const spo::ShapeDocument& document,
    const std::vector<spo::MergeCandidate>& candidates,
    spo::RegionBoundaryAnalysis& outBoundary,
    std::string& outMessage) {
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
            outBoundary = std::move(boundary);
            outMessage = "Auto-selected largest valid FeatureBoundedRefit candidate.";
            return candidate;
        }
        if (firstFailure.empty()) {
            firstFailure = boundary.message;
        }
    }

    outMessage = "Auto candidate selection found no valid boundary candidate.";
    if (!firstFailure.empty()) {
        outMessage += " First boundary failure: " + firstFailure;
    }
    return nullptr;
}

spo::MergePlannerOptions planner_options() {
    spo::MergePlannerOptions options;
    options.enable_feature_bounded_refit_candidates = true;
    options.min_feature_bounded_region_faces = 2;
    return options;
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

std::string int_array_json(const std::vector<int>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            out << ", ";
        }
        out << values[index];
    }
    out << "]";
    return out.str();
}

bool write_manifest(
    const Options& options,
    const spo::ShapeDocument& sourceDocument,
    const spo::ShapeDocument& removedDocument,
    const spo::MergeCandidate& candidate,
    const spo::RegionBoundaryAnalysis& boundary,
    const int generatedCandidateCount,
    const std::string& selectionMessage) {
    if (options.manifest.empty()) {
        return true;
    }

    std::filesystem::create_directories(options.manifest.parent_path());
    std::ofstream out(options.manifest, std::ios::binary);
    if (!out.good()) {
        return false;
    }

    std::vector<int> faceIds;
    faceIds.reserve(candidate.faces.size());
    for (const auto faceId : candidate.faces) {
        faceIds.push_back(faceId);
    }

    out << "{\n";
    out << "  \"tool\": \"creo_phase1_input_exporter\",\n";
    out << "  \"status\": \"base_removed_candidate_exported\",\n";
    out << "  \"removal_method\": \"BRepTools_ReShape::Remove\",\n";
    out << "  \"source_step\": " << quote_json(path_to_utf8(options.sourceStep)) << ",\n";
    out << "  \"base_removed_candidate_step\": " << quote_json(path_to_utf8(options.baseRemovedOutput)) << ",\n";
    out << "  \"candidate_id\": " << candidate.candidate_id << ",\n";
    out << "  \"candidate_selection_mode\": " << quote_json(options.autoCandidateId ? "auto" : "explicit") << ",\n";
    out << "  \"candidate_selection_message\": " << quote_json(selectionMessage) << ",\n";
    out << "  \"generated_candidate_count\": " << generatedCandidateCount << ",\n";
    out << "  \"source_face_ids\": " << int_array_json(faceIds) << ",\n";
    out << "  \"source_face_count\": " << candidate_face_count(candidate) << ",\n";
    out << "  \"boundary_edge_count\": " << candidate_boundary_edge_count(candidate) << ",\n";
    out << "  \"boundary_valid\": " << bool_json(boundary.valid) << ",\n";
    out << "  \"boundary_closed\": " << bool_json(boundary.boundary_closed) << ",\n";
    out << "  \"boundary_loop_count\": " << boundary.boundary_loops.size() << ",\n";
    out << "  \"source_stats\": " << stats_json(sourceDocument.stats(), 2) << ",\n";
    out << "  \"base_removed_stats\": " << stats_json(removedDocument.stats(), 2) << "\n";
    out << "}\n";
    return true;
}

void print_stage(const char* stage) {
    std::cerr << "[creo_phase1_input_exporter] " << stage << "\n";
    std::cerr.flush();
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }

    print_stage("reading source STEP");
    const auto read = spo::StepReader().read(options.sourceStep);
    if (!read.status.success()) {
        std::cerr << read.status.message() << "\n";
        return 1;
    }
    const auto& sourceDocument = read.document;

    print_stage("detecting feature edges");
    const auto featureEdges = spo::FeatureEdgeDetector().detect(
        sourceDocument.topology(),
        options.angularThresholdDegrees,
        options.minEdgeLength);

    print_stage("planning merge candidates");
    const auto planner = spo::MergePlanner().plan(sourceDocument, featureEdges, {}, planner_options());

    spo::RegionBoundaryAnalysis boundary;
    std::string selectionMessage;
    const auto* candidate = options.autoCandidateId
        ? select_auto_candidate(sourceDocument, planner.candidates, boundary, selectionMessage)
        : find_candidate(planner.candidates, options.candidateId);
    if (candidate == nullptr) {
        std::cerr << (selectionMessage.empty() ? "Candidate id not found." : selectionMessage)
                  << " generated candidates: " << planner.candidates.size() << "\n";
        return 1;
    }
    if (!options.autoCandidateId) {
        boundary = spo::RegionBoundaryAnalyzer().analyze(sourceDocument, *candidate);
        if (!boundary.valid) {
            std::cerr << "Boundary analysis failed: " << boundary.message << "\n";
            return 1;
        }
    }

    print_stage("removing candidate faces");
    BRepTools_ReShape reshaper;
    const auto& topology = sourceDocument.topology();
    for (const auto faceId : candidate->faces) {
        if (faceId < 0 || static_cast<std::size_t>(faceId) >= topology.faceCount()) {
            std::cerr << "Candidate source face id is out of range: " << faceId << "\n";
            return 1;
        }
        reshaper.Remove(topology.face(faceId));
    }

    TopoDS_Shape removedShape;
    try {
        removedShape = reshaper.Apply(sourceDocument.shape());
    } catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        std::cerr << "BRepTools_ReShape failed: " << (message != nullptr ? message : "unknown") << "\n";
        return 1;
    }
    if (removedShape.IsNull()) {
        std::cerr << "BRepTools_ReShape produced an empty shape.\n";
        return 1;
    }

    const spo::ShapeDocument removedDocument(removedShape, options.baseRemovedOutput);
    print_stage("writing base_removed_candidate STEP");
    std::filesystem::create_directories(options.baseRemovedOutput.parent_path());
    const auto write = spo::StepWriter().write(removedDocument, options.baseRemovedOutput);
    if (!write.success()) {
        std::cerr << write.message() << "\n";
        return 1;
    }

    if (!write_manifest(
            options,
            sourceDocument,
            removedDocument,
            *candidate,
            boundary,
            static_cast<int>(planner.candidates.size()),
            selectionMessage)) {
        std::cerr << "Could not write manifest: " << path_to_utf8(options.manifest) << "\n";
        return 1;
    }

    std::cout << "base_removed_candidate: " << path_to_utf8(options.baseRemovedOutput) << "\n";
    std::cout << "candidate_id: " << candidate->candidate_id << "\n";
    return 0;
}
