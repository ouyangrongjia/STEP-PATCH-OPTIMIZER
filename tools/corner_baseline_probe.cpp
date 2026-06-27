#include "command/CommandContext.h"
#include "command/PatchReplacementCommand.h"
#include "brep/ShapeDocument.h"
#include "external/geomagic/GeomagicAutoSurfaceBackend.h"
#include "external/geomagic/GeomagicOutputPathResolver.h"
#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "io/StlWriter.h"
#include "merge/MergePlanner.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchImportService.h"
#include "patch/PatchPreviewReport.h"
#include "stl/StpSampledFittingMeshBuilder.h"
#include "validate/CommercialCadQualityGate.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace {

struct Options {
    std::filesystem::path sourceStep;
    std::filesystem::path patchPath;
    std::filesystem::path outputDir;
    std::filesystem::path reportPath;
    std::filesystem::path wrapCorePath = std::filesystem::path("E:/Geomagic Wrap/wrapCore.exe");
    std::filesystem::path scriptPath;

    int candidateId = -1;
    bool autoCandidateId = true;
    int generatedCandidateCount = 0;
    double angularThresholdDegrees = 25.0;
    double minEdgeLength = 0.0;

    int boundarySamplesPerEdge = 64;
    int featureEdgeSamplesPerEdge = 64;
    double maxBoundaryDistance = 0.03;
    double maxCornerAnchorDistance = 0.02;
    double maxFeatureEdgeDistance = 0.03;

    double geomagicTolerance = 0.03;
    double geomagicDetail = 0.10;
    int timeoutSeconds = 1800;

    bool enableB1CornerFeatureSampling = false;
    int cornerFeatureSamplesPerEdge = 64;
    bool enableB2AdjacentFaceSupportCollar = false;
    int adjacentFaceSupportCollarSamplesPerEdge = 64;
    double adjacentFaceSupportCollarWidth = 0.05;
    int adjacentFaceSupportCollarRingCount = 1;
    bool enableAdaptiveAdjacentFaceSupportCollarWidth = false;
    double adjacentFaceSupportCollarUnderCover = 0.0;
    bool enableB2CornerSafeSupportCollar = false;
    double adjacentFaceSupportCollarMaxOffsetScale = 1.25;
    bool enableCandidateSurfaceOverCover = false;
    int candidateSurfaceOverCoverSamplesPerEdge = 64;
    double candidateSurfaceOverCoverWidth = 0.25;
    int candidateSurfaceOverCoverRingCount = 3;
    double candidateSurfaceOverCoverCornerMiterMaxScale = 1.25;
    bool geomagicSharpenContours = false;
    bool allowHighRiskPatchPreview = false;
    bool strictOriginalBoundaryRetrim = false;
};

struct AppliedStepExportReport {
    bool attempted = false;
    bool success = false;
    bool writeSuccess = false;
    bool readbackSuccess = false;
    std::filesystem::path path;
    std::string message;
};

std::filesystem::path repo_root() {
#if defined(SPO_SOURCE_DIR)
    return std::filesystem::path(SPO_SOURCE_DIR);
#else
    return std::filesystem::current_path();
#endif
}

std::string path_to_string(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.c_str()), utf8.size()};
}

QString path_to_qstring(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.c_str()), static_cast<qsizetype>(utf8.size()));
}

QJsonArray edge_ids_to_json(const std::vector<spo::EdgeId>& edgeIds) {
    QJsonArray array;
    for (const auto edgeId : edgeIds) {
        array.append(static_cast<int>(edgeId));
    }
    return array;
}

QJsonArray ints_to_json(const std::vector<int>& values) {
    QJsonArray array;
    for (const auto value : values) {
        array.append(value);
    }
    return array;
}

QJsonArray strings_to_json(const std::vector<std::string>& values) {
    QJsonArray array;
    for (const auto& value : values) {
        array.append(QString::fromStdString(value));
    }
    return array;
}

QJsonObject path_object(const std::filesystem::path& path) {
    QJsonObject object;
    object.insert("path", path_to_qstring(path));
    object.insert("exists", std::filesystem::exists(path));
    return object;
}

void print_usage() {
    std::cerr
        << "Usage: corner_baseline_probe --source-step <model.stp> --candidate-id <id|auto> [options]\n"
        << "\n"
        << "Options:\n"
        << "  --candidate-id <id|auto>                 Default: auto.\n"
        << "  --patch <patch.stp|patch.igs>             Reuse an existing patch and skip Geomagic.\n"
        << "  --output-dir <dir>                       Default: <repo>/data/baseline_runs.\n"
        << "  --report <path>                          Default: <output-dir>/baseline_report.json.\n"
        << "  --wrap-core <path>                       Default: E:/Geomagic Wrap/wrapCore.exe.\n"
        << "  --script <path>                          Default: <repo>/scripts/geomagic_wrap/autosurface_pipeline.py.\n"
        << "  --angle <degrees>                        Feature edge angle threshold, default 25.\n"
        << "  --min-edge-length <value>                Feature edge minimum length, default 0.\n"
        << "  --boundary-samples <n>                   Dense boundary samples per edge, default 64.\n"
        << "  --feature-edge-samples <n>               Feature edge samples per edge, default 64.\n"
        << "  --max-boundary-distance <value>          Default 0.03.\n"
        << "  --max-corner-distance <value>            Default 0.02.\n"
        << "  --max-feature-edge-distance <value>      Default 0.03.\n"
        << "  --geomagic-tolerance <value>             Default 0.03.\n"
        << "  --geomagic-detail <0..1>                 Default 0.10.\n"
        << "  --timeout-seconds <n>                    Default 1800.\n"
        << "  --b1-corner-feature-sampling            Enable B1 corner / feature edge dense anchors.\n"
        << "  --corner-feature-samples <n>             B1 dense samples per feature edge, default 64.\n"
        << "  --b2-adjacent-face-support-collar        Enable B2 adjacent-face seam support collar.\n"
        << "  --support-collar-samples <n>             B2 samples per boundary edge, default 64.\n"
        << "  --support-collar-width <value>           B2 auxiliary adjacent support collar width, default 0.05.\n"
        << "  --support-collar-rings <n>               B2 auxiliary support collar ring count, default 1.\n"
        << "  --adaptive-support-collar-width          Use max(configured_width, 2*g_under, 2*h95) per boundary edge.\n"
        << "  --support-collar-under-cover <value>     Current max under-cover used by adaptive support width.\n"
        << "  --b2-corner-safe-support-collar          Enable B2.3 support collar corner clamp.\n"
        << "  --support-collar-max-offset-scale <v>    B2.3 max collar offset scale, default 1.25.\n"
        << "  --candidate-surface-over-cover           Enable source-face parallel candidate over-cover.\n"
        << "  --candidate-over-cover-samples <n>       Candidate over-cover samples per edge, default 64.\n"
        << "  --candidate-over-cover-width <value>     Candidate over-cover width, default 0.25.\n"
        << "  --candidate-over-cover-rings <n>         Candidate over-cover ring count, default 3.\n"
        << "  --candidate-over-cover-miter-max-scale <v> Candidate over-cover miter cap scale, default 1.25.\n"
        << "  --geomagic-sharpen-contours              Enable Geomagic sharpenConstrainedContours.\n"
        << "  --allow-high-risk-patch-preview          Continue past patch preview high-risk warnings for experiments.\n"
        << "  --strict-original-boundary-retrim        Rebuild original-boundary pcurves on the selected patch surface for experiments.\n";
}

bool parse_options(int argc, char* argv[], Options& options) {
    const auto root = repo_root();
    options.outputDir = root / "data" / "baseline_runs";
    options.scriptPath = root / "scripts" / "geomagic_wrap" / "autosurface_pipeline.py";

    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto requireValue = [&](const char* name) -> const char* {
            if (index + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++index];
        };

        if (arg == "--source-step") {
            const auto* value = requireValue("--source-step");
            if (value == nullptr) {
                return false;
            }
            options.sourceStep = value;
        } else if (arg == "--patch") {
            const auto* value = requireValue("--patch");
            if (value == nullptr) {
                return false;
            }
            options.patchPath = value;
        } else if (arg == "--output-dir") {
            const auto* value = requireValue("--output-dir");
            if (value == nullptr) {
                return false;
            }
            options.outputDir = value;
        } else if (arg == "--report") {
            const auto* value = requireValue("--report");
            if (value == nullptr) {
                return false;
            }
            options.reportPath = value;
        } else if (arg == "--wrap-core") {
            const auto* value = requireValue("--wrap-core");
            if (value == nullptr) {
                return false;
            }
            options.wrapCorePath = value;
        } else if (arg == "--script") {
            const auto* value = requireValue("--script");
            if (value == nullptr) {
                return false;
            }
            options.scriptPath = value;
        } else if (arg == "--candidate-id") {
            const auto* value = requireValue("--candidate-id");
            if (value == nullptr) {
                return false;
            }
            const std::string candidateValue(value);
            if (candidateValue == "auto") {
                options.autoCandidateId = true;
                options.candidateId = -1;
            } else {
                options.autoCandidateId = false;
                options.candidateId = std::stoi(value);
                if (options.candidateId < 0) {
                    std::cerr << "--candidate-id must be a non-negative integer or auto.\n";
                    return false;
                }
            }
        } else if (arg == "--angle") {
            const auto* value = requireValue("--angle");
            if (value == nullptr) {
                return false;
            }
            options.angularThresholdDegrees = std::stod(value);
        } else if (arg == "--min-edge-length") {
            const auto* value = requireValue("--min-edge-length");
            if (value == nullptr) {
                return false;
            }
            options.minEdgeLength = std::stod(value);
        } else if (arg == "--boundary-samples") {
            const auto* value = requireValue("--boundary-samples");
            if (value == nullptr) {
                return false;
            }
            options.boundarySamplesPerEdge = std::stoi(value);
        } else if (arg == "--feature-edge-samples") {
            const auto* value = requireValue("--feature-edge-samples");
            if (value == nullptr) {
                return false;
            }
            options.featureEdgeSamplesPerEdge = std::stoi(value);
        } else if (arg == "--max-boundary-distance") {
            const auto* value = requireValue("--max-boundary-distance");
            if (value == nullptr) {
                return false;
            }
            options.maxBoundaryDistance = std::stod(value);
        } else if (arg == "--max-corner-distance") {
            const auto* value = requireValue("--max-corner-distance");
            if (value == nullptr) {
                return false;
            }
            options.maxCornerAnchorDistance = std::stod(value);
        } else if (arg == "--max-feature-edge-distance") {
            const auto* value = requireValue("--max-feature-edge-distance");
            if (value == nullptr) {
                return false;
            }
            options.maxFeatureEdgeDistance = std::stod(value);
        } else if (arg == "--geomagic-tolerance") {
            const auto* value = requireValue("--geomagic-tolerance");
            if (value == nullptr) {
                return false;
            }
            options.geomagicTolerance = std::stod(value);
        } else if (arg == "--geomagic-detail") {
            const auto* value = requireValue("--geomagic-detail");
            if (value == nullptr) {
                return false;
            }
            options.geomagicDetail = std::stod(value);
        } else if (arg == "--timeout-seconds") {
            const auto* value = requireValue("--timeout-seconds");
            if (value == nullptr) {
                return false;
            }
            options.timeoutSeconds = std::stoi(value);
        } else if (arg == "--b1-corner-feature-sampling") {
            options.enableB1CornerFeatureSampling = true;
        } else if (arg == "--corner-feature-samples") {
            const auto* value = requireValue("--corner-feature-samples");
            if (value == nullptr) {
                return false;
            }
            options.cornerFeatureSamplesPerEdge = std::stoi(value);
        } else if (arg == "--b2-adjacent-face-support-collar") {
            options.enableB2AdjacentFaceSupportCollar = true;
        } else if (arg == "--support-collar-samples") {
            const auto* value = requireValue("--support-collar-samples");
            if (value == nullptr) {
                return false;
            }
            options.adjacentFaceSupportCollarSamplesPerEdge = std::stoi(value);
        } else if (arg == "--support-collar-width") {
            const auto* value = requireValue("--support-collar-width");
            if (value == nullptr) {
                return false;
            }
            options.adjacentFaceSupportCollarWidth = std::stod(value);
        } else if (arg == "--support-collar-rings") {
            const auto* value = requireValue("--support-collar-rings");
            if (value == nullptr) {
                return false;
            }
            options.adjacentFaceSupportCollarRingCount = std::stoi(value);
        } else if (arg == "--adaptive-support-collar-width") {
            options.enableB2AdjacentFaceSupportCollar = true;
            options.enableAdaptiveAdjacentFaceSupportCollarWidth = true;
        } else if (arg == "--support-collar-under-cover") {
            const auto* value = requireValue("--support-collar-under-cover");
            if (value == nullptr) {
                return false;
            }
            options.adjacentFaceSupportCollarUnderCover = std::stod(value);
        } else if (arg == "--b2-corner-safe-support-collar") {
            options.enableB2AdjacentFaceSupportCollar = true;
            options.enableB2CornerSafeSupportCollar = true;
        } else if (arg == "--support-collar-max-offset-scale") {
            const auto* value = requireValue("--support-collar-max-offset-scale");
            if (value == nullptr) {
                return false;
            }
            options.adjacentFaceSupportCollarMaxOffsetScale = std::stod(value);
        } else if (arg == "--candidate-surface-over-cover") {
            options.enableCandidateSurfaceOverCover = true;
        } else if (arg == "--candidate-over-cover-samples") {
            const auto* value = requireValue("--candidate-over-cover-samples");
            if (value == nullptr) {
                return false;
            }
            options.candidateSurfaceOverCoverSamplesPerEdge = std::stoi(value);
        } else if (arg == "--candidate-over-cover-width") {
            const auto* value = requireValue("--candidate-over-cover-width");
            if (value == nullptr) {
                return false;
            }
            options.candidateSurfaceOverCoverWidth = std::stod(value);
        } else if (arg == "--candidate-over-cover-rings") {
            const auto* value = requireValue("--candidate-over-cover-rings");
            if (value == nullptr) {
                return false;
            }
            options.candidateSurfaceOverCoverRingCount = std::stoi(value);
        } else if (arg == "--candidate-over-cover-miter-max-scale") {
            const auto* value = requireValue("--candidate-over-cover-miter-max-scale");
            if (value == nullptr) {
                return false;
            }
            options.candidateSurfaceOverCoverCornerMiterMaxScale = std::stod(value);
        } else if (arg == "--geomagic-sharpen-contours") {
            options.geomagicSharpenContours = true;
        } else if (arg == "--allow-high-risk-patch-preview") {
            options.allowHighRiskPatchPreview = true;
        } else if (arg == "--strict-original-boundary-retrim") {
            options.strictOriginalBoundaryRetrim = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    options.outputDir = std::filesystem::absolute(options.outputDir).lexically_normal();
    if (options.reportPath.empty()) {
        options.reportPath = options.outputDir / "baseline_report.json";
    } else {
        options.reportPath = std::filesystem::absolute(options.reportPath).lexically_normal();
    }

    return !options.sourceStep.empty();
}

struct CandidateSelection {
    const spo::MergeCandidate* candidate = nullptr;
    spo::RegionBoundaryAnalysis boundary;
    std::string message;
};

const spo::MergeCandidate* find_candidate_by_id(
    const std::vector<spo::MergeCandidate>& candidates,
    int candidateId) {
    for (const auto& candidate : candidates) {
        if (candidate.candidate_id == candidateId) {
            return &candidate;
        }
    }
    return nullptr;
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

CandidateSelection select_candidate_for_baseline(
    const spo::ShapeDocument& document,
    const std::vector<spo::MergeCandidate>& candidates,
    const Options& options) {
    CandidateSelection selection;
    if (candidates.empty()) {
        selection.message = "No generated candidates.";
        return selection;
    }

    if (!options.autoCandidateId) {
        const auto* candidate = find_candidate_by_id(candidates, options.candidateId);
        if (candidate == nullptr) {
            selection.message =
                "Candidate id not found. generated candidates: " + std::to_string(candidates.size());
            return selection;
        }

        selection.boundary = spo::RegionBoundaryAnalyzer().analyze(document, *candidate);
        if (!selection.boundary.valid) {
            selection.message = "Boundary analysis failed: " + selection.boundary.message;
            return selection;
        }

        selection.candidate = candidate;
        selection.message = "Selected requested candidate id.";
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

std::string lower_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension;
}

spo::PatchArtifactPaths artifacts_for_patch_path(const std::filesystem::path& patchPath) {
    spo::PatchArtifactPaths artifacts;
    artifacts.success = true;
    artifacts.message = "Using user-provided patch artifact.";
    const auto extension = lower_extension(patchPath);
    if (extension == ".igs" || extension == ".iges") {
        artifacts.patchIgesSidecarPath = patchPath;
        artifacts.foundIgesSidecar = true;
    } else {
        artifacts.patchStepPath = patchPath;
        artifacts.foundStep = true;
    }
    return artifacts;
}

std::filesystem::path document_output_stem(const spo::ShapeDocument& document) {
    const auto stem = document.sourcePath().stem();
    if (!stem.empty()) {
        return stem;
    }
    return std::filesystem::path("document");
}

std::filesystem::path candidate_patch_filename(
    const spo::ShapeDocument& document,
    const spo::MergeCandidate& candidate,
    const char* extension) {
    std::ostringstream name;
    name << "_candidate_" << std::setw(4) << std::setfill('0') << candidate.candidate_id << extension;
    auto filename = document_output_stem(document);
    filename += name.str();
    return filename;
}

std::filesystem::path fitting_stl_path(
    const spo::ShapeDocument& document,
    const spo::MergeCandidate& candidate) {
    const auto root = repo_root();
    return root / "data" / "crop_stl" / document_output_stem(document) /
        candidate_patch_filename(document, candidate, ".stl");
}

std::filesystem::path applied_step_path(
    const Options& options,
    const spo::ShapeDocument& document,
    const spo::MergeCandidate& candidate) {
    return options.outputDir / candidate_patch_filename(document, candidate, "_applied.stp");
}

std::filesystem::path sidecar_path(const std::filesystem::path& outputStepPath, const char* suffix) {
    auto filename = outputStepPath.stem();
    filename += suffix;
    return outputStepPath.parent_path() / filename;
}

void print_stage(const char* stage) {
    std::cerr << "[corner_baseline_probe] " << stage << "\n";
    std::cerr.flush();
}

QJsonObject stats_to_json(const spo::CommercialCadDistanceStats& stats) {
    QJsonObject object;
    object.insert("evaluated", stats.evaluated);
    object.insert("samples", stats.samples);
    object.insert("over_tolerance", stats.overTolerance);
    object.insert("tolerance", stats.tolerance);
    object.insert("max_distance", stats.maxDistance);
    object.insert("mean_distance", stats.meanDistance);
    object.insert("rms_distance", stats.rmsDistance);
    object.insert("p95_distance", stats.p95Distance);
    return object;
}

QJsonObject seam_to_json(const spo::CommercialCadSeamContinuityStats& stats) {
    QJsonObject object;
    object.insert("evaluated", stats.evaluated);
    object.insert("samples", stats.samples);
    object.insert("over_tolerance", stats.overTolerance);
    object.insert("tolerance", stats.tolerance);
    object.insert("max_signed_normal_offset", stats.maxSignedNormalOffset);
    object.insert("max_abs_signed_normal_offset", stats.maxAbsSignedNormalOffset);
    object.insert("mean_abs_signed_normal_offset", stats.meanAbsSignedNormalOffset);
    object.insert("rms_abs_signed_normal_offset", stats.rmsAbsSignedNormalOffset);
    object.insert("p95_abs_signed_normal_offset", stats.p95AbsSignedNormalOffset);
    return object;
}

QJsonObject sampling_to_json(const spo::CommercialCadSamplingReport& sampling) {
    QJsonObject object;
    object.insert("boundary_samples_per_edge", sampling.boundarySamplesPerEdge);
    object.insert("feature_edge_samples_per_edge", sampling.featureEdgeSamplesPerEdge);
    object.insert("anchor_dedup_tolerance", sampling.anchorDedupTolerance);
    object.insert("corner_anchor_source", QString::fromStdString(sampling.cornerAnchorSource));
    object.insert("boundary_edges_sampled", sampling.boundaryEdgesSampled);
    object.insert("boundary_sample_count", sampling.boundarySampleCount);
    object.insert("corner_anchor_count", sampling.cornerAnchorCount);
    object.insert("feature_edge_result_available", sampling.featureEdgeResultAvailable);
    object.insert("feature_boundary_edges_sampled", sampling.featureBoundaryEdgesSampled);
    object.insert("feature_edge_sample_count", sampling.featureEdgeSampleCount);
    return object;
}

QJsonObject quality_to_json(const spo::CommercialCadQualityGateReport& report) {
    QJsonObject object;
    object.insert("evaluated", report.evaluated);
    object.insert("passed", report.passed);
    object.insert("sharp_corner_preservation_passed", report.sharpCornerPreservationPassed);
    object.insert("candidate_id", report.candidateId);
    object.insert("boundary_edge_count", report.boundaryEdgeCount);
    object.insert("feature_boundary_edge_count", report.featureBoundaryEdgeCount);
    object.insert("sampling_report", sampling_to_json(report.sampling));
    object.insert("boundary", stats_to_json(report.boundary));
    object.insert("seam_continuity", seam_to_json(report.seamContinuity));
    object.insert("corner_anchors", stats_to_json(report.cornerAnchors));
    object.insert("feature_edges", stats_to_json(report.featureEdges));
    object.insert("message", QString::fromStdString(report.message));
    object.insert("warning", QString::fromStdString(report.warningMessage));
    return object;
}

QJsonObject fitting_to_json(const spo::StpSampledFittingReport& report) {
    QJsonObject object;
    object.insert("success", report.success);
    object.insert("candidate_id", report.candidateId);
    object.insert("source_face_count", report.sourceFaceCount);
    object.insert("boundary_edge_count", report.boundaryEdgeCount);
    object.insert("boundary_sample_count", report.boundarySampleCount);
    object.insert("boundary_band_sample_count", report.boundaryBandSampleCount);
    object.insert("corner_feature_dense_sampling_enabled", report.cornerFeatureDenseSamplingEnabled);
    object.insert("feature_edge_dense_sample_count", report.featureEdgeDenseSampleCount);
    object.insert("corner_anchor_sample_count", report.cornerAnchorSampleCount);
    object.insert("corner_feature_surface_division_count", report.cornerFeatureSurfaceDivisionCount);
    object.insert("adjacent_face_support_collar_enabled", report.adjacentFaceSupportCollarEnabled);
    object.insert("adjacent_face_support_collar_width", report.adjacentFaceSupportCollarWidth);
    object.insert("adjacent_face_support_collar_ring_count", report.adjacentFaceSupportCollarRingCount);
    object.insert("adjacent_face_support_collar_edge_count", report.adjacentFaceSupportCollarEdgeCount);
    object.insert("adjacent_face_support_collar_sample_count", report.adjacentFaceSupportCollarSampleCount);
    object.insert("adjacent_face_support_collar_triangle_count", report.adjacentFaceSupportCollarTriangleCount);
    object.insert("adjacent_face_support_collar_adjacent_face_sample_count", report.adjacentFaceSupportCollarAdjacentFaceSampleCount);
    object.insert("adjacent_face_support_collar_fallback_count", report.adjacentFaceSupportCollarFallbackCount);
    object.insert("adjacent_face_support_collar_rejected_count", report.adjacentFaceSupportCollarRejectedCount);
    object.insert("adjacent_face_support_collar_boundary_coverage", report.adjacentFaceSupportCollarBoundaryCoverage);
    object.insert(
        "adjacent_face_support_collar_adaptive_width_enabled",
        report.adjacentFaceSupportCollarAdaptiveWidthEnabled);
    object.insert(
        "adjacent_face_support_collar_under_cover",
        report.adjacentFaceSupportCollarUnderCover);
    object.insert(
        "adjacent_face_support_collar_boundary_h95",
        report.adjacentFaceSupportCollarBoundaryH95);
    object.insert(
        "adjacent_face_support_collar_effective_width_min",
        report.adjacentFaceSupportCollarEffectiveWidthMin);
    object.insert(
        "adjacent_face_support_collar_effective_width_mean",
        report.adjacentFaceSupportCollarEffectiveWidthMean);
    object.insert(
        "adjacent_face_support_collar_effective_width_max",
        report.adjacentFaceSupportCollarEffectiveWidthMax);
    object.insert(
        "adjacent_face_support_collar_anchor_count",
        report.adjacentFaceSupportCollarAnchorCount);
    object.insert(
        "adjacent_face_support_collar_body_bridge_sample_count",
        report.adjacentFaceSupportCollarBodyBridgeSampleCount);
    object.insert(
        "adjacent_face_support_collar_body_bridge_triangle_count",
        report.adjacentFaceSupportCollarBodyBridgeTriangleCount);
    object.insert(
        "adjacent_face_support_collar_body_bridge_rejected_count",
        report.adjacentFaceSupportCollarBodyBridgeRejectedCount);
    object.insert(
        "adjacent_face_support_collar_body_bridge_component_count",
        report.adjacentFaceSupportCollarBodyBridgeComponentCount);
    object.insert(
        "adjacent_face_support_collar_body_bridge_max_gap",
        report.adjacentFaceSupportCollarBodyBridgeMaxGap);
    object.insert(
        "adjacent_face_support_collar_corner_clamp_enabled",
        report.adjacentFaceSupportCollarCornerClampEnabled);
    object.insert(
        "adjacent_face_support_collar_corner_clamp_count",
        report.adjacentFaceSupportCollarCornerClampCount);
    object.insert(
        "adjacent_face_support_collar_max_offset",
        report.adjacentFaceSupportCollarMaxOffset);
    object.insert("candidate_surface_over_cover_enabled", report.candidateSurfaceOverCoverEnabled);
    object.insert("candidate_surface_over_cover_width", report.candidateSurfaceOverCoverWidth);
    object.insert("candidate_surface_over_cover_ring_count", report.candidateSurfaceOverCoverRingCount);
    object.insert("candidate_surface_over_cover_edge_count", report.candidateSurfaceOverCoverEdgeCount);
    object.insert("candidate_surface_over_cover_sample_count", report.candidateSurfaceOverCoverSampleCount);
    object.insert("candidate_surface_over_cover_triangle_count", report.candidateSurfaceOverCoverTriangleCount);
    object.insert("candidate_surface_over_cover_fallback_count", report.candidateSurfaceOverCoverFallbackCount);
    object.insert("candidate_surface_over_cover_rejected_count", report.candidateSurfaceOverCoverRejectedCount);
    object.insert("candidate_surface_over_cover_boundary_coverage", report.candidateSurfaceOverCoverBoundaryCoverage);
    object.insert("candidate_surface_over_cover_corner_miter_count", report.candidateSurfaceOverCoverCornerMiterCount);
    object.insert("candidate_surface_over_cover_max_offset", report.candidateSurfaceOverCoverMaxOffset);
    object.insert("candidate_surface_over_cover_normal_leakage_max", report.candidateSurfaceOverCoverNormalLeakageMax);
    object.insert("candidate_surface_over_cover_direction_fallback_count", report.candidateSurfaceOverCoverDirectionFallbackCount);
    object.insert("candidate_surface_over_cover_long_triangle_count", report.candidateSurfaceOverCoverLongTriangleCount);
    object.insert("candidate_surface_over_cover_max_triangle_edge_length", report.candidateSurfaceOverCoverMaxTriangleEdgeLength);
    object.insert("candidate_surface_over_cover_source_face_count", report.candidateSurfaceOverCoverSourceFaceCount);
    object.insert("candidate_surface_over_cover_direction_flip_count", report.candidateSurfaceOverCoverDirectionFlipCount);
    object.insert("interior_sample_count", report.interiorSampleCount);
    object.insert("output_triangle_count", report.outputTriangleCount);
    object.insert("sampling_spacing", report.samplingSpacing);
    object.insert("boundary_spacing", report.boundarySpacing);
    object.insert("band_ring_count", report.bandRingCount);
    object.insert("output_path", path_to_qstring(report.outputPath));
    object.insert("message", QString::fromStdString(report.message));
    object.insert("warning", QString::fromStdString(report.warningMessage));
    return object;
}

QJsonObject geomagic_to_json(const spo::GeomagicAutoSurfaceResult& result) {
    QJsonObject object;
    object.insert("success", result.success);
    object.insert("timed_out", result.timedOut);
    object.insert("exit_code", result.exitCode);
    object.insert("duration_ms", static_cast<double>(result.durationMs));
    object.insert("input_stl_path", path_to_qstring(result.inputStlPath));
    object.insert("output_step_path", path_to_qstring(result.outputStepPath));
    object.insert("output_iges_path", path_to_qstring(result.outputIgesPath));
    object.insert("fit_region_log_path", path_to_qstring(result.fitRegionLogPath));
    object.insert("message", QString::fromStdString(result.message));
    object.insert("error", QString::fromStdString(result.errorMessage));
    object.insert("failed_stage", QString::fromStdString(result.failedStage));
    return object;
}

QJsonObject preview_to_json(const spo::PatchPreviewReport& report) {
    QJsonObject object;
    object.insert("success", report.success);
    object.insert("high_risk", report.highRisk);
    object.insert("candidate_id", report.candidateId);
    object.insert("source_face_count", report.sourceFaceCount);
    object.insert("source_boundary_edge_count", report.sourceBoundaryEdgeCount);
    object.insert("patch_face_count", report.patchFaceCount);
    object.insert("patch_edge_count", report.patchEdgeCount);
    object.insert("patch_shell_count", report.patchShellCount);
    object.insert("patch_solid_count", report.patchSolidCount);
    object.insert("patch_brep_check_valid", report.patchBRepCheckValid);
    object.insert("bbox_center_distance", report.bboxCenterDistance);
    object.insert("bbox_diagonal_ratio", report.bboxDiagonalRatio);
    object.insert("message", QString::fromStdString(report.message));
    object.insert("warning", QString::fromStdString(report.warningMessage));
    object.insert("recommended_action", QString::fromStdString(report.recommendedAction));
    return object;
}

QJsonObject point_to_json(bool valid, double x, double y, double z) {
    QJsonObject object;
    object.insert("valid", valid);
    object.insert("x", x);
    object.insert("y", y);
    object.insert("z", z);
    return object;
}

QJsonObject free_edge_diagnostic_to_json(const spo::PatchReplacementFreeEdgeDiagnostic& diagnostic) {
    QJsonObject object;
    object.insert("after_repair", diagnostic.afterRepair);
    object.insert("appeared_after_repair", diagnostic.appearedAfterRepair);
    object.insert("edge_index", diagnostic.edgeIndex);
    object.insert("adjacent_face_count", diagnostic.adjacentFaceCount);
    object.insert("edge_length", diagnostic.edgeLength);
    object.insert("edge_tolerance", diagnostic.edgeTolerance);
    object.insert("degenerated", diagnostic.degenerated);
    object.insert(
        "midpoint",
        point_to_json(
            diagnostic.midpointValid,
            diagnostic.midpointX,
            diagnostic.midpointY,
            diagnostic.midpointZ));
    object.insert(
        "start",
        point_to_json(
            diagnostic.startPointValid,
            diagnostic.startX,
            diagnostic.startY,
            diagnostic.startZ));
    object.insert(
        "end",
        point_to_json(
            diagnostic.endPointValid,
            diagnostic.endX,
            diagnostic.endY,
            diagnostic.endZ));
    object.insert("nearest_original_boundary_edge_id", diagnostic.nearestOriginalBoundaryEdgeId);
    object.insert("nearest_original_boundary_edge_distance", diagnostic.nearestOriginalBoundaryEdgeDistance);
    object.insert("nearest_original_boundary_edge_length", diagnostic.nearestOriginalBoundaryEdgeLength);
    object.insert("nearest_original_boundary_edge_tolerance", diagnostic.nearestOriginalBoundaryEdgeTolerance);
    object.insert(
        "nearest_original_boundary_start",
        point_to_json(
            diagnostic.nearestOriginalBoundaryStartPointValid,
            diagnostic.nearestOriginalBoundaryStartX,
            diagnostic.nearestOriginalBoundaryStartY,
            diagnostic.nearestOriginalBoundaryStartZ));
    object.insert(
        "nearest_original_boundary_midpoint",
        point_to_json(
            diagnostic.nearestOriginalBoundaryMidpointValid,
            diagnostic.nearestOriginalBoundaryMidpointX,
            diagnostic.nearestOriginalBoundaryMidpointY,
            diagnostic.nearestOriginalBoundaryMidpointZ));
    object.insert(
        "nearest_original_boundary_end",
        point_to_json(
            diagnostic.nearestOriginalBoundaryEndPointValid,
            diagnostic.nearestOriginalBoundaryEndX,
            diagnostic.nearestOriginalBoundaryEndY,
            diagnostic.nearestOriginalBoundaryEndZ));
    object.insert("nearest_original_boundary_parameter_range_valid", diagnostic.nearestOriginalBoundaryParameterRangeValid);
    object.insert("nearest_original_boundary_first_parameter", diagnostic.nearestOriginalBoundaryFirstParameter);
    object.insert("nearest_original_boundary_last_parameter", diagnostic.nearestOriginalBoundaryLastParameter);
    object.insert("nearest_original_boundary_adjacent_face_count", diagnostic.nearestOriginalBoundaryAdjacentFaceCount);
    object.insert("nearest_original_boundary_adjacent_face_ids", ints_to_json(diagnostic.nearestOriginalBoundaryAdjacentFaceIds));
    object.insert("nearest_original_boundary_adjacent_surface_types", strings_to_json(diagnostic.nearestOriginalBoundaryAdjacentSurfaceTypes));
    object.insert("nearest_original_boundary_pcurve_available_face_count", diagnostic.nearestOriginalBoundaryPcurveAvailableFaceCount);
    object.insert("matched_split_boundary_segment", diagnostic.matchedSplitBoundarySegment);
    object.insert("matched_split_boundary_first_parameter", diagnostic.matchedSplitBoundaryFirstParameter);
    object.insert("matched_split_boundary_last_parameter", diagnostic.matchedSplitBoundaryLastParameter);
    object.insert("patch_face_owner", diagnostic.patchFaceOwner);
    object.insert("same_original_boundary_edge_split_segment_count", diagnostic.sameOriginalBoundaryEdgeSplitSegmentCount);
    object.insert("same_original_boundary_edge_owner_switch_count", diagnostic.sameOriginalBoundaryEdgeOwnerSwitchCount);
    object.insert("same_original_boundary_edge_degenerated_segment_count", diagnostic.sameOriginalBoundaryEdgeDegeneratedSegmentCount);
    object.insert("nearest_split_boundary_segment", diagnostic.nearestSplitBoundarySegment);
    object.insert("nearest_split_boundary_original_edge_id", diagnostic.nearestSplitBoundaryOriginalEdgeId);
    object.insert("nearest_split_boundary_segment_distance", diagnostic.nearestSplitBoundarySegmentDistance);
    object.insert("nearest_split_boundary_first_parameter", diagnostic.nearestSplitBoundaryFirstParameter);
    object.insert("nearest_split_boundary_last_parameter", diagnostic.nearestSplitBoundaryLastParameter);
    object.insert("nearest_split_boundary_length", diagnostic.nearestSplitBoundaryLength);
    object.insert("nearest_split_boundary_tolerance", diagnostic.nearestSplitBoundaryTolerance);
    object.insert("nearest_split_boundary_patch_face_owner", diagnostic.nearestSplitBoundaryPatchFaceOwner);
    object.insert("fitted_patch_projection_face_count", diagnostic.fittedPatchProjectionFaceCount);
    object.insert("fitted_patch_projection_sample_count", diagnostic.fittedPatchProjectionSampleCount);
    object.insert("fitted_patch_projection_failed_count", diagnostic.fittedPatchProjectionFailedCount);
    object.insert("fitted_patch_projection_min_distance", diagnostic.fittedPatchProjectionMinDistance);
    object.insert("fitted_patch_projection_max_distance", diagnostic.fittedPatchProjectionMaxDistance);
    object.insert("fitted_patch_projection_average_distance", diagnostic.fittedPatchProjectionAverageDistance);
    object.insert("nearest_fitted_patch_face_index", diagnostic.nearestFittedPatchFaceIndex);
    return object;
}

QJsonArray free_edge_diagnostics_to_json(
    const std::vector<spo::PatchReplacementFreeEdgeDiagnostic>& diagnostics) {
    QJsonArray array;
    for (const auto& diagnostic : diagnostics) {
        array.append(free_edge_diagnostic_to_json(diagnostic));
    }
    return array;
}

QJsonObject trim_diagnostics_to_json(const spo::PatchTrimDiagnosticsReport& report) {
    QJsonObject object;
    object.insert("captured", report.captured);
    object.insert("replacement_face_count", report.replacementFaceCount);
    object.insert("trim_wire_invalid_count", report.trimWireInvalidCount);
    object.insert("trim_uv_loop_self_intersection_count", report.trimUvLoopSelfIntersectionCount);
    object.insert("over_cover_sample_count", report.overCoverSampleCount);
    object.insert("over_cover_total_sample_count", report.overCoverTotalSampleCount);
    object.insert("over_cover_ratio", report.overCoverRatio);
    object.insert("over_cover_max_distance", report.overCoverMaxDistance);
    object.insert("under_cover_sample_count", report.underCoverSampleCount);
    object.insert("under_cover_total_sample_count", report.underCoverTotalSampleCount);
    object.insert("under_cover_max_distance", report.underCoverMaxDistance);
    object.insert("boundary_gap_max", report.boundaryGapMax);
    object.insert("boundary_gap_p95", report.boundaryGapP95);
    object.insert("boundary_gap_rms", report.boundaryGapRms);
    object.insert("internal_seam_gap_max", report.internalSeamGapMax);
    object.insert("internal_seam_gap_p95", report.internalSeamGapP95);
    object.insert("internal_seam_gap_rms", report.internalSeamGapRms);
    object.insert("worst_boundary_edge_id", report.worstBoundaryEdgeId);
    object.insert("worst_internal_edge_id", report.worstInternalEdgeId);
    object.insert("roundtrip_compared", report.roundtripCompared);
    object.insert("roundtrip_changed", report.roundtripChanged);
    return object;
}

QJsonObject external_cad_diagnostics_to_json(const spo::PatchExternalCadDiagnosticsReport& report) {
    QJsonObject object;
    object.insert("captured", report.captured);
    object.insert("raw_patch_preflight_available", report.rawPatchPreflightAvailable);
    object.insert("raw_patch_preflight_executed", report.rawPatchPreflightExecuted);
    object.insert("raw_patch_preflight_input_path", QString::fromStdString(report.rawPatchPreflightInputPath));
    object.insert("raw_patch_preflight_role", QString::fromStdString(report.rawPatchPreflightRole));
    object.insert("raw_patch_preflight_status", QString::fromStdString(report.rawPatchPreflightStatus));
    object.insert("raw_patch_preflight_message", QString::fromStdString(report.rawPatchPreflightMessage));
    object.insert(
        "final_applied_step_diagnostic_stage",
        QString::fromStdString(report.finalAppliedStepDiagnosticStage));
    object.insert("final_applied_step_diagnostic_eligible", report.finalAppliedStepDiagnosticEligible);
    object.insert("final_applied_step_diagnostic_executed", report.finalAppliedStepDiagnosticExecuted);
    object.insert(
        "final_applied_step_diagnostic_input_path",
        QString::fromStdString(report.finalAppliedStepDiagnosticInputPath));
    object.insert(
        "final_applied_step_diagnostic_status",
        QString::fromStdString(report.finalAppliedStepDiagnosticStatus));
    object.insert(
        "final_applied_step_diagnostic_skipped_reason",
        QString::fromStdString(report.finalAppliedStepDiagnosticSkippedReason));
    object.insert(
        "final_applied_step_diagnostic_message",
        QString::fromStdString(report.finalAppliedStepDiagnosticMessage));
    return object;
}

QJsonObject apply_to_json(const spo::PatchReplacementReport& report) {
    QJsonObject object;
    object.insert("success", report.success);
    object.insert("candidate_id", report.candidateId);
    object.insert("used_original_boundary_surface_retrim", report.usedOriginalBoundarySurfaceRetrim);
    object.insert("attempted_multi_surface_boundary_shell", report.attemptedMultiSurfaceBoundaryShell);
    object.insert("used_multi_surface_boundary_shell", report.usedMultiSurfaceBoundaryShell);
    object.insert("patch_face_count", report.patchFaceCount);
    object.insert("replacement_face_count", report.replacementFaceCount);
    object.insert("replacement_edge_count", report.replacementEdgeCount);
    object.insert("replacement_shell_count", report.replacementShellCount);
    object.insert("replacement_solid_count", report.replacementSolidCount);
    object.insert("pre_repair_closure_captured", report.preRepairClosureCaptured);
    object.insert("pre_repair_face_count", report.preRepairFaceCount);
    object.insert("pre_repair_edge_count", report.preRepairEdgeCount);
    object.insert("pre_repair_shell_count", report.preRepairShellCount);
    object.insert("pre_repair_solid_count", report.preRepairSolidCount);
    object.insert("pre_repair_brep_check_valid", report.preRepairBRepCheckValid);
    object.insert("pre_repair_free_edge_count", report.preRepairFreeEdgeCount);
    object.insert("pre_repair_multiple_edge_count", report.preRepairMultipleEdgeCount);
    object.insert("pre_repair_degenerated_free_edge_count", report.preRepairDegeneratedFreeEdgeCount);
    object.insert("post_repair_closure_captured", report.postRepairClosureCaptured);
    object.insert("post_repair_face_count", report.postRepairFaceCount);
    object.insert("post_repair_edge_count", report.postRepairEdgeCount);
    object.insert("post_repair_shell_count", report.postRepairShellCount);
    object.insert("post_repair_solid_count", report.postRepairSolidCount);
    object.insert("post_repair_brep_check_valid", report.postRepairBRepCheckValid);
    object.insert("post_repair_free_edge_count", report.postRepairFreeEdgeCount);
    object.insert("post_repair_multiple_edge_count", report.postRepairMultipleEdgeCount);
    object.insert("post_repair_degenerated_free_edge_count", report.postRepairDegeneratedFreeEdgeCount);
    object.insert(
        "appeared_after_repair_degenerated_free_edge_count",
        report.appearedAfterRepairDegeneratedFreeEdgeCount);
    object.insert("free_edge_diagnostics", free_edge_diagnostics_to_json(report.freeEdgeDiagnostics));
    object.insert("trim_diagnostics", trim_diagnostics_to_json(report.trimDiagnostics));
    object.insert("external_cad_diagnostics", external_cad_diagnostics_to_json(report.externalCadDiagnostics));
    object.insert(
        "retrim_boundary_edge_pcurve_rebuild_attempt_count",
        report.retrimBoundaryEdgePcurveRebuildAttemptCount);
    object.insert(
        "retrim_boundary_edge_pcurve_rebuild_success_count",
        report.retrimBoundaryEdgePcurveRebuildSuccessCount);
    object.insert(
        "retrim_boundary_edge_pcurve_rebuild_failure_count",
        report.retrimBoundaryEdgePcurveRebuildFailureCount);
    object.insert(
        "retrim_boundary_edge_same_parameter_check_count",
        report.retrimBoundaryEdgeSameParameterCheckCount);
    object.insert(
        "retrim_boundary_edge_same_parameter_failure_count",
        report.retrimBoundaryEdgeSameParameterFailureCount);
    object.insert(
        "retrim_boundary_edge_max_same_parameter_deviation",
        report.retrimBoundaryEdgeMaxSameParameterDeviation);
    object.insert(
        "retrim_boundary_edge_pcurve_rebuild_failed_edge_ids",
        edge_ids_to_json(report.retrimBoundaryEdgePcurveRebuildFailedEdgeIds));
    object.insert(
        "retrim_boundary_edge_same_parameter_failed_edge_ids",
        edge_ids_to_json(report.retrimBoundaryEdgeSameParameterFailedEdgeIds));
    object.insert("multi_surface_boundary_sample_count", report.multiSurfaceBoundarySampleCount);
    object.insert("multi_surface_projected_sample_count", report.multiSurfaceProjectedSampleCount);
    object.insert("multi_surface_failed_projection_count", report.multiSurfaceFailedProjectionCount);
    object.insert("multi_surface_max_projection_distance", report.multiSurfaceMaxProjectionDistance);
    object.insert("multi_surface_average_projection_distance", report.multiSurfaceAverageProjectionDistance);
    object.insert("multi_surface_assigned_boundary_segment_count", report.multiSurfaceAssignedBoundarySegmentCount);
    object.insert("multi_surface_split_boundary_edge_count", report.multiSurfaceSplitBoundaryEdgeCount);
    object.insert("multi_surface_built_face_count", report.multiSurfaceBuiltFaceCount);
    object.insert("multi_surface_closed_wire_count", report.multiSurfaceClosedWireCount);
    object.insert("multi_surface_open_wire_count", report.multiSurfaceOpenWireCount);
    object.insert(
        "multi_surface_multiple_closed_wire_face_count",
        report.multiSurfaceMultipleClosedWireFaceCount);
    object.insert(
        "multi_surface_skipped_unowned_open_wire_face_count",
        report.multiSurfaceSkippedUnownedOpenWireFaceCount);
    object.insert("multi_surface_failed_patch_face_index", report.multiSurfaceFailedPatchFaceIndex);
    object.insert("multi_surface_failed_face_edge_count", report.multiSurfaceFailedFaceEdgeCount);
    object.insert(
        "multi_surface_failed_face_original_boundary_segment_count",
        report.multiSurfaceFailedFaceOriginalBoundarySegmentCount);
    object.insert(
        "multi_surface_failed_face_internal_edge_count",
        report.multiSurfaceFailedFaceInternalEdgeCount);
    object.insert(
        "multi_surface_failed_open_wire_edge_count",
        report.multiSurfaceFailedOpenWireEdgeCount);
    object.insert(
        "multi_surface_failed_open_wire_length",
        report.multiSurfaceFailedOpenWireLength);
    object.insert(
        "multi_surface_failed_open_wire_endpoint_gap",
        report.multiSurfaceFailedOpenWireEndpointGap);
    object.insert(
        "multi_surface_failed_open_wire_start",
        point_to_json(
            report.multiSurfaceFailedOpenWireStartPointValid,
            report.multiSurfaceFailedOpenWireStartX,
            report.multiSurfaceFailedOpenWireStartY,
            report.multiSurfaceFailedOpenWireStartZ));
    object.insert(
        "multi_surface_failed_open_wire_end",
        point_to_json(
            report.multiSurfaceFailedOpenWireEndPointValid,
            report.multiSurfaceFailedOpenWireEndX,
            report.multiSurfaceFailedOpenWireEndY,
            report.multiSurfaceFailedOpenWireEndZ));
    object.insert(
        "multi_surface_selected_wire_connect_tolerance",
        report.multiSurfaceSelectedWireConnectTolerance);
    object.insert(
        "multi_surface_fallback_wire_connect_attempted",
        report.multiSurfaceFallbackWireConnectAttempted);
    object.insert(
        "multi_surface_fallback_wire_connect_succeeded",
        report.multiSurfaceFallbackWireConnectSucceeded);
    object.insert("multi_surface_failed_edge_ids", edge_ids_to_json(report.multiSurfaceFailedEdgeIds));
    object.insert(
        "multi_surface_failed_face_original_boundary_edge_ids",
        edge_ids_to_json(report.multiSurfaceFailedFaceOriginalBoundaryEdgeIds));
    object.insert(
        "multi_surface_boundary_edge_pcurve_rebuild_attempt_count",
        report.multiSurfaceBoundaryEdgePcurveRebuildAttemptCount);
    object.insert(
        "multi_surface_boundary_edge_pcurve_rebuild_success_count",
        report.multiSurfaceBoundaryEdgePcurveRebuildSuccessCount);
    object.insert(
        "multi_surface_boundary_edge_pcurve_rebuild_failure_count",
        report.multiSurfaceBoundaryEdgePcurveRebuildFailureCount);
    object.insert(
        "multi_surface_boundary_edge_same_parameter_check_count",
        report.multiSurfaceBoundaryEdgeSameParameterCheckCount);
    object.insert(
        "multi_surface_boundary_edge_same_parameter_failure_count",
        report.multiSurfaceBoundaryEdgeSameParameterFailureCount);
    object.insert(
        "multi_surface_boundary_edge_max_same_parameter_deviation",
        report.multiSurfaceBoundaryEdgeMaxSameParameterDeviation);
    object.insert(
        "multi_surface_boundary_edge_pcurve_rebuild_failed_edge_ids",
        edge_ids_to_json(report.multiSurfaceBoundaryEdgePcurveRebuildFailedEdgeIds));
    object.insert(
        "multi_surface_boundary_edge_same_parameter_failed_edge_ids",
        edge_ids_to_json(report.multiSurfaceBoundaryEdgeSameParameterFailedEdgeIds));
    object.insert("gate_evaluated", report.gateEvaluated);
    object.insert("gate_passed", report.gatePassed);
    object.insert("gate_after_brep_check_valid", report.gateAfterBRepCheckValid);
    object.insert("gate_roundtrip_brep_check_valid", report.gateRoundtripBRepCheckValid);
    object.insert("gate_step_roundtrip_ok", report.gateStepRoundtripOk);
    object.insert("gate_after_free_edges", report.gateAfterFreeEdges);
    object.insert("gate_after_multiple_edges", report.gateAfterMultipleEdges);
    object.insert("gate_roundtrip_free_edges", report.gateRoundtripFreeEdges);
    object.insert("gate_roundtrip_multiple_edges", report.gateRoundtripMultipleEdges);
    object.insert("repair_degenerated_free_edges_before", report.degeneratedFreeEdgesBeforeRepair);
    object.insert("repair_degenerated_free_edges_after", report.degeneratedFreeEdgesAfterRepair);
    object.insert("best_sewing_degenerated_free_edge_count", report.bestSewingDegeneratedFreeEdgeCount);
    object.insert("failure_reason", QString::fromStdString(spo::toString(report.failureReason)));
    object.insert("gate_failure_reason", QString::fromStdString(report.gateFailureReason));
    object.insert("message", QString::fromStdString(report.message));
    object.insert("warning", QString::fromStdString(report.warningMessage));
    return object;
}

QJsonObject applied_step_export_to_json(const AppliedStepExportReport& report) {
    QJsonObject object;
    object.insert("attempted", report.attempted);
    object.insert("success", report.success);
    object.insert("write_success", report.writeSuccess);
    object.insert("readback_success", report.readbackSuccess);
    object.insert("path", path_to_qstring(report.path));
    object.insert("exists", !report.path.empty() && std::filesystem::exists(report.path));
    object.insert("message", QString::fromStdString(report.message));
    return object;
}

void update_external_cad_diagnostics_after_applied_step_export(
    spo::PatchReplacementReport& report,
    const AppliedStepExportReport& appliedStepExport) {
    auto& diagnostics = report.externalCadDiagnostics;
    diagnostics.captured = true;
    diagnostics.finalAppliedStepDiagnosticStage = "AppliedStepAfterSuccessfulApply";
    diagnostics.finalAppliedStepDiagnosticExecuted = false;

    if (appliedStepExport.success &&
        !appliedStepExport.path.empty() &&
        std::filesystem::exists(appliedStepExport.path)) {
        diagnostics.finalAppliedStepDiagnosticEligible = true;
        diagnostics.finalAppliedStepDiagnosticInputPath = path_to_string(appliedStepExport.path);
        diagnostics.finalAppliedStepDiagnosticStatus = "PendingExternalRunner";
        diagnostics.finalAppliedStepDiagnosticSkippedReason.clear();
        diagnostics.finalAppliedStepDiagnosticMessage =
            "Run the final external CAD diagnostic on the applied STEP exported after successful Patch Apply.";
        return;
    }

    diagnostics.finalAppliedStepDiagnosticEligible = false;
    diagnostics.finalAppliedStepDiagnosticInputPath.clear();
    diagnostics.finalAppliedStepDiagnosticStatus = "Skipped";
    if (appliedStepExport.attempted) {
        diagnostics.finalAppliedStepDiagnosticSkippedReason =
            "Applied STEP export or readback failed; final external CAD diagnostics require an exported and readable applied STEP.";
    } else {
        diagnostics.finalAppliedStepDiagnosticSkippedReason =
            "Patch Apply did not produce an applied STEP; raw Geomagic patch diagnostics cannot validate the merged model.";
    }
    diagnostics.finalAppliedStepDiagnosticMessage.clear();
}

AppliedStepExportReport export_applied_step(
    const Options& options,
    const spo::ShapeDocument& document,
    const spo::MergeCandidate& candidate) {
    AppliedStepExportReport report;
    report.attempted = true;
    report.path = applied_step_path(options, document, candidate);

    std::error_code dirError;
    std::filesystem::create_directories(report.path.parent_path(), dirError);
    if (dirError) {
        report.message = "Could not create applied STEP export directory: " + dirError.message();
        return report;
    }

    const auto write = spo::StepWriter().write(document, report.path);
    report.writeSuccess = write.success();
    if (!write.success()) {
        report.message = "Applied STEP export failed: " + write.message();
        return report;
    }

    const auto readback = spo::StepReader().read(report.path);
    report.readbackSuccess = readback.status.success() && readback.document.hasShape();
    if (!report.readbackSuccess) {
        report.message = "Applied STEP readback failed: " + readback.status.message();
        return report;
    }

    report.success = true;
    report.message = "Applied STEP exported and read back successfully.";
    return report;
}

bool write_report(
    const Options& options,
    bool overallSuccess,
    const std::string& stage,
    const spo::StpSampledFittingReport& fitting,
    const spo::GeomagicAutoSurfaceResult& geomagic,
    const spo::PatchPreviewReport& preview,
    const spo::PatchReplacementReport& apply,
    const AppliedStepExportReport& appliedStepExport,
    const spo::CommercialCadQualityGateReport& quality,
    const spo::PatchArtifactPaths& artifacts,
    std::string* errorMessage = nullptr) {
    QJsonObject root;
    root.insert("overall_success", overallSuccess);
    root.insert("stage", QString::fromStdString(stage));
    root.insert("source_step", path_to_qstring(options.sourceStep));
    root.insert("candidate_id", options.candidateId);
    root.insert("candidate_selection_mode", options.autoCandidateId ? "auto" : "explicit");
    root.insert("geomagic_sharpen_contours", options.geomagicSharpenContours);
    root.insert("generated_candidate_count", options.generatedCandidateCount);
    root.insert("output_dir", path_to_qstring(options.outputDir));
    root.insert("report_path", path_to_qstring(options.reportPath));
    root.insert("user_patch_path", path_to_qstring(options.patchPath));
    root.insert("fitting_stl", path_object(fitting.outputPath));
    root.insert("patch_step_path", path_object(artifacts.patchStepPath));
    root.insert("patch_iges_path", path_object(artifacts.patchIgesSidecarPath));
    root.insert("fit_region_log_path", path_object(artifacts.fitRegionLogPath));
    root.insert("stp_sampled_fitting", fitting_to_json(fitting));
    root.insert("geomagic", geomagic_to_json(geomagic));
    root.insert("patch_preview", preview_to_json(preview));
    root.insert("patch_apply", apply_to_json(apply));
    root.insert("applied_step_export", applied_step_export_to_json(appliedStepExport));
    root.insert("commercial_cad_like_quality_gate", quality_to_json(quality));

    std::error_code error;
    std::filesystem::create_directories(options.reportPath.parent_path(), error);
    if (error) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not create report directory: " + error.message();
        }
        return false;
    }

    QFile file(path_to_qstring(options.reportPath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not open report file.";
        }
        return false;
    }

    const QJsonDocument document(root);
    if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not write report file.";
        }
        return false;
    }

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

spo::MergePlannerOptions baseline_planner_options() {
    spo::MergePlannerOptions options;
    options.enable_feature_bounded_refit_candidates = true;
    options.min_feature_bounded_region_faces = 2;
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }

    std::error_code dirError;
    std::filesystem::create_directories(options.outputDir, dirError);
    if (dirError) {
        std::cerr << "Could not create output directory: " << dirError.message() << "\n";
        return 1;
    }

    spo::StpSampledFittingReport fittingReport;
    spo::GeomagicAutoSurfaceResult geomagicResult;
    spo::PatchPreviewReport previewReport;
    spo::PatchReplacementReport applyReport;
    AppliedStepExportReport appliedStepExportReport;
    spo::CommercialCadQualityGateReport qualityReport;
    spo::PatchArtifactPaths artifacts;

    auto fail = [&](const std::string& stage, const std::string& message) {
        std::cerr << message << "\n";
        std::string reportError;
        write_report(
            options,
            false,
            stage,
            fittingReport,
            geomagicResult,
            previewReport,
            applyReport,
            appliedStepExportReport,
            qualityReport,
            artifacts,
            &reportError);
        if (!reportError.empty()) {
            std::cerr << "Report write failed: " << reportError << "\n";
        }
        return 1;
    };

    print_stage("reading source STEP");
    const auto read = spo::StepReader().read(options.sourceStep);
    if (!read.status.success()) {
        return fail("read_step", read.status.message());
    }
    auto document = read.document;

    print_stage("detecting feature edges");
    const auto featureEdges = spo::FeatureEdgeDetector().detect(
        document.topology(),
        options.angularThresholdDegrees,
        options.minEdgeLength);

    print_stage("planning merge candidates");
    const auto planner = spo::MergePlanner().plan(
        document,
        featureEdges,
        {},
        baseline_planner_options());
    options.generatedCandidateCount = static_cast<int>(planner.candidates.size());

    print_stage("selecting and analyzing original boundary");
    const auto selection = select_candidate_for_baseline(document, planner.candidates, options);
    if (selection.candidate == nullptr) {
        return fail("plan_candidates", selection.message);
    }
    const auto* candidate = selection.candidate;
    options.candidateId = candidate->candidate_id;
    const auto boundary = selection.boundary;

    print_stage("building STP-sampled fitting STL");
    spo::StlMesh fittingMesh;
    spo::StpSampledFittingOptions samplingOptions;
    samplingOptions.enableCornerFeatureDenseSampling = options.enableB1CornerFeatureSampling;
    samplingOptions.cornerFeatureSamplesPerEdge = options.cornerFeatureSamplesPerEdge;
    samplingOptions.enableAdjacentFaceSupportCollar = options.enableB2AdjacentFaceSupportCollar;
    samplingOptions.adjacentFaceSupportCollarSamplesPerEdge = options.adjacentFaceSupportCollarSamplesPerEdge;
    samplingOptions.adjacentFaceSupportCollarWidth = options.adjacentFaceSupportCollarWidth;
    samplingOptions.adjacentFaceSupportCollarRingCount = options.adjacentFaceSupportCollarRingCount;
    samplingOptions.enableAdaptiveAdjacentFaceSupportCollarWidth =
        options.enableAdaptiveAdjacentFaceSupportCollarWidth;
    samplingOptions.adjacentFaceSupportCollarUnderCover =
        options.adjacentFaceSupportCollarUnderCover;
    samplingOptions.enableAdjacentFaceSupportCollarCornerClamp = options.enableB2CornerSafeSupportCollar;
    samplingOptions.adjacentFaceSupportCollarMaxOffsetScale = options.adjacentFaceSupportCollarMaxOffsetScale;
    samplingOptions.enableCandidateSurfaceOverCover = options.enableCandidateSurfaceOverCover;
    samplingOptions.candidateSurfaceOverCoverSamplesPerEdge =
        options.candidateSurfaceOverCoverSamplesPerEdge;
    samplingOptions.candidateSurfaceOverCoverWidth = options.candidateSurfaceOverCoverWidth;
    samplingOptions.candidateSurfaceOverCoverRingCount = options.candidateSurfaceOverCoverRingCount;
    samplingOptions.candidateSurfaceOverCoverCornerMiterMaxScale =
        options.candidateSurfaceOverCoverCornerMiterMaxScale;
    fittingReport = spo::StpSampledFittingMeshBuilder().build(
        document,
        *candidate,
        samplingOptions,
        fittingMesh);
    if (!fittingReport.success) {
        return fail("stp_sampled_fitting", fittingReport.message);
    }

    const auto localStlPath = fitting_stl_path(document, *candidate);
    fittingReport.outputPath = localStlPath;
    std::error_code stlDirError;
    std::filesystem::create_directories(localStlPath.parent_path(), stlDirError);
    if (stlDirError) {
        fittingReport.success = false;
        fittingReport.message = "Could not create fitting STL directory: " + stlDirError.message();
        return fail("write_fitting_stl", fittingReport.message);
    }
    const auto writeStl = spo::StlWriter().write(fittingMesh, localStlPath);
    if (!writeStl.success) {
        fittingReport.success = false;
        fittingReport.message = writeStl.message;
        return fail("write_fitting_stl", writeStl.message);
    }

    if (options.patchPath.empty()) {
        print_stage("running Geomagic AutoSurface");
        const auto root = repo_root();
        const auto paths = spo::resolveGeomagicOutputPathsFromCropStl(
            localStlPath,
            root / "data" / "crop_stl",
            root / "data" / "crop_stp",
            root / "data" / "crop_igs");
        if (!paths.success) {
            return fail("resolve_geomagic_outputs", paths.message);
        }

        spo::GeomagicAutoSurfaceConfig config;
        config.wrapCorePath = options.wrapCorePath;
        config.scriptPath = options.scriptPath;
        config.inputStlPath = localStlPath;
        config.outputStepPath = paths.outputStepPath;
        config.outputIgesPath = paths.outputIgesPath;
        config.workDir = root;
        config.fitRegionLogPath = sidecar_path(paths.outputStepPath, "_fit_region.log");
        config.geometry = "Mechanical";
        config.autoMerge = true;
        config.adaptiveFit = false;
        config.strictPatchTarget = false;
        config.tolerance = options.geomagicTolerance;
        config.detail = options.geomagicDetail;
        config.sharpenContours = options.geomagicSharpenContours;
        config.timeoutSeconds = options.timeoutSeconds;

        geomagicResult = spo::GeomagicAutoSurfaceBackend().run(config);
        if (!geomagicResult.success) {
            const auto message = geomagicResult.errorMessage.empty()
                ? geomagicResult.message
                : geomagicResult.errorMessage;
            return fail("geomagic", message.empty() ? "Geomagic AutoSurface failed." : message);
        }

        artifacts = spo::PatchArtifactLocator().locateFromResult(geomagicResult);
        if (!artifacts.success) {
            return fail("locate_patch_artifact", artifacts.message);
        }
    } else {
        print_stage("using provided patch artifact");
        artifacts = artifacts_for_patch_path(options.patchPath);
        artifacts.localStlPath = localStlPath;
        geomagicResult.success = true;
        geomagicResult.inputStlPath = localStlPath;
        geomagicResult.outputStepPath = artifacts.patchStepPath;
        geomagicResult.outputIgesPath = artifacts.patchIgesSidecarPath;
        geomagicResult.message = "Geomagic skipped; using user-provided patch.";
    }

    const auto patchPath = artifacts.foundStep ? artifacts.patchStepPath : artifacts.patchIgesSidecarPath;
    print_stage("importing patch");
    auto importedPatch = spo::PatchImportService().importPatch(patchPath);
    if (!importedPatch.success) {
        return fail("import_patch", "Patch import failed: " + importedPatch.errorMessage);
    }

    print_stage("building patch preview report");
    previewReport = spo::buildPatchPreviewReport(
        &document,
        candidate,
        importedPatch,
        artifacts);
    if (!previewReport.success || (previewReport.highRisk && !options.allowHighRiskPatchPreview)) {
        return fail(
            "patch_preview",
            "Patch preview is not apply-ready. success=" +
                std::to_string(previewReport.success) +
                " highRisk=" + std::to_string(previewReport.highRisk) +
                " message=" + previewReport.message +
                " warning=" + previewReport.warningMessage);
    }

    print_stage("evaluating commercial-CAD-like drift gate");
    spo::CommercialCadQualityGateInput qualityInput;
    qualityInput.document = &document;
    qualityInput.candidate = candidate;
    qualityInput.boundary = &boundary;
    qualityInput.featureEdges = &featureEdges;
    qualityInput.patchShape = &importedPatch.shape;
    qualityInput.options.boundarySamplesPerEdge = options.boundarySamplesPerEdge;
    qualityInput.options.featureEdgeSamplesPerEdge = options.featureEdgeSamplesPerEdge;
    qualityInput.options.maxBoundaryDistance = options.maxBoundaryDistance;
    qualityInput.options.maxCornerAnchorDistance = options.maxCornerAnchorDistance;
    qualityInput.options.maxFeatureEdgeDistance = options.maxFeatureEdgeDistance;
    qualityReport = spo::CommercialCadQualityGate().evaluate(qualityInput);

    spo::PatchReplacementInput input;
    input.document = &document;
    input.candidate = candidate;
    input.boundary = &boundary;
    input.importedPatch = &importedPatch;
    input.artifactPaths = &artifacts;
    input.previewReport = &previewReport;
    input.allowHighRiskPatchPreview = options.allowHighRiskPatchPreview;
    input.strictOriginalBoundaryRetrim = options.strictOriginalBoundaryRetrim;

    spo::PatchReplacementCommandOptions commandOptions;
    commandOptions.requireWatertightSolidGate = true;
    commandOptions.requireZeroFreeEdges = true;
    commandOptions.requireZeroMultipleEdges = true;
    commandOptions.requireRoundtripWatertight = true;

    spo::CommandContext context;
    context.document = document;
    context.featureEdges = featureEdges;
    context.sourcePath = options.sourceStep;

    print_stage("executing patch replacement");
    spo::PatchReplacementCommand command(input, &applyReport, commandOptions);
    const auto result = command.execute(context);

    const auto strictApplySuccess =
        result.success() &&
        applyReport.gateEvaluated &&
        applyReport.gatePassed;

    if (strictApplySuccess) {
        print_stage("exporting applied STEP");
        appliedStepExportReport = export_applied_step(options, context.document, *candidate);
    } else if (command.afterDocument().hasShape()) {
        print_stage("exporting gate-failed applied STEP");
        appliedStepExportReport = export_applied_step(options, command.afterDocument(), *candidate);
        if (appliedStepExportReport.success) {
            appliedStepExportReport.message =
                "Gate-failed applied STEP exported for diagnostics; StrictTopologyGate did not pass.";
        }
    }
    update_external_cad_diagnostics_after_applied_step_export(applyReport, appliedStepExportReport);

    const auto exportOk = !appliedStepExportReport.attempted || appliedStepExportReport.success;
    const auto overallSuccess =
        strictApplySuccess &&
        exportOk &&
        qualityReport.evaluated &&
        qualityReport.passed;
    const auto finalStage = overallSuccess
        ? "completed"
        : (!strictApplySuccess
            ? "failed_topology_gate"
            : (!exportOk
                ? "export_applied_step"
                : "failed_quality_gate"));

    std::string reportError;
    if (!write_report(
            options,
            overallSuccess,
            finalStage,
            fittingReport,
            geomagicResult,
            previewReport,
            applyReport,
            appliedStepExportReport,
            qualityReport,
            artifacts,
            &reportError)) {
        std::cerr << "Report write failed: " << reportError << "\n";
        return 1;
    }

    std::cout << "corner baseline report: " << path_to_string(options.reportPath) << "\n";
    std::cout << "StrictTopologyGate passed: " << applyReport.gatePassed << "\n";
    if (appliedStepExportReport.attempted) {
        std::cout << "applied STEP export: " << path_to_string(appliedStepExportReport.path)
                  << " readback=" << appliedStepExportReport.readbackSuccess << "\n";
    }
    std::cout << "CommercialCadLikeQualityGate passed: " << qualityReport.passed << "\n";
    std::cout << "boundary max distance: " << qualityReport.boundary.maxDistance << "\n";
    std::cout << "corner max distance: " << qualityReport.cornerAnchors.maxDistance << "\n";
    std::cout << "feature edge max distance: " << qualityReport.featureEdges.maxDistance << "\n";
    return overallSuccess ? 0 : 1;
}
