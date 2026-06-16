#include "command/CommandContext.h"
#include "command/PatchReplacementCommand.h"
#include "brep/ShapeDocument.h"
#include "external/geomagic/GeomagicAutoSurfaceBackend.h"
#include "external/geomagic/GeomagicOutputPathResolver.h"
#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StlWriter.h"
#include "merge/MergePlanner.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchImportService.h"
#include "patch/PatchPreviewReport.h"
#include "stl/StpSampledFittingMeshBuilder.h"
#include "validate/CommercialCadQualityGate.h"

#include <QFile>
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
    bool enableB2BoundaryGuardBand = false;
    int boundaryGuardBandSamplesPerEdge = 16;
    int boundaryGuardBandRingCount = 1;
    double boundaryGuardBandSpacing = 0.10;
    bool enableB2OverCoverStrip = false;
    double boundaryOverCoverWidth = 0.10;
    int boundaryOverCoverRingCount = 1;
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
        << "  --b2-boundary-guard-band                Enable B2 STP boundary guard-band sampling.\n"
        << "  --guard-band-samples <n>                 B2 samples per boundary edge, default 16.\n"
        << "  --guard-band-rings <n>                   B2 guard-band ring count, default 1.\n"
        << "  --guard-band-spacing <value>             B2 guard-band spacing, default 0.10.\n"
        << "  --b2-over-cover-strip                   Enable B2.1 fitting STL boundary over-cover strip.\n"
        << "  --over-cover-width <value>               B2.1 over-cover strip total width, default 0.10.\n"
        << "  --over-cover-rings <n>                   B2.1 over-cover ring count, default 1.\n";
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
        } else if (arg == "--b2-boundary-guard-band") {
            options.enableB2BoundaryGuardBand = true;
        } else if (arg == "--guard-band-samples") {
            const auto* value = requireValue("--guard-band-samples");
            if (value == nullptr) {
                return false;
            }
            options.boundaryGuardBandSamplesPerEdge = std::stoi(value);
        } else if (arg == "--guard-band-rings") {
            const auto* value = requireValue("--guard-band-rings");
            if (value == nullptr) {
                return false;
            }
            options.boundaryGuardBandRingCount = std::stoi(value);
        } else if (arg == "--guard-band-spacing") {
            const auto* value = requireValue("--guard-band-spacing");
            if (value == nullptr) {
                return false;
            }
            options.boundaryGuardBandSpacing = std::stod(value);
        } else if (arg == "--b2-over-cover-strip") {
            options.enableB2OverCoverStrip = true;
        } else if (arg == "--over-cover-width") {
            const auto* value = requireValue("--over-cover-width");
            if (value == nullptr) {
                return false;
            }
            options.boundaryOverCoverWidth = std::stod(value);
        } else if (arg == "--over-cover-rings") {
            const auto* value = requireValue("--over-cover-rings");
            if (value == nullptr) {
                return false;
            }
            options.boundaryOverCoverRingCount = std::stoi(value);
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
    object.insert("boundary_guard_band_sampling_enabled", report.boundaryGuardBandSamplingEnabled);
    object.insert("boundary_guard_band_edge_count", report.boundaryGuardBandEdgeCount);
    object.insert("boundary_guard_band_sample_count", report.boundaryGuardBandSampleCount);
    object.insert("boundary_guard_band_triangle_count", report.boundaryGuardBandTriangleCount);
    object.insert("boundary_guard_band_ring_count", report.boundaryGuardBandRingCount);
    object.insert("boundary_guard_band_spacing", report.boundaryGuardBandSpacing);
    object.insert("boundary_guard_band_adjacent_face_sample_count", report.boundaryGuardBandAdjacentFaceSampleCount);
    object.insert("boundary_guard_band_fallback_sample_count", report.boundaryGuardBandFallbackSampleCount);
    object.insert("boundary_over_cover_strip_enabled", report.boundaryOverCoverStripEnabled);
    object.insert("boundary_over_cover_width", report.boundaryOverCoverWidth);
    object.insert("boundary_over_cover_ring_count", report.boundaryOverCoverRingCount);
    object.insert("boundary_over_cover_sample_count", report.boundaryOverCoverSampleCount);
    object.insert("boundary_over_cover_triangle_count", report.boundaryOverCoverTriangleCount);
    object.insert("boundary_over_cover_fallback_count", report.boundaryOverCoverFallbackCount);
    object.insert("boundary_over_cover_rejected_count", report.boundaryOverCoverRejectedCount);
    object.insert("boundary_over_cover_boundary_coverage", report.boundaryOverCoverBoundaryCoverage);
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

QJsonObject apply_to_json(const spo::PatchReplacementReport& report) {
    QJsonObject object;
    object.insert("success", report.success);
    object.insert("candidate_id", report.candidateId);
    object.insert("used_original_boundary_surface_retrim", report.usedOriginalBoundarySurfaceRetrim);
    object.insert("used_multi_surface_boundary_shell", report.usedMultiSurfaceBoundaryShell);
    object.insert("patch_face_count", report.patchFaceCount);
    object.insert("replacement_face_count", report.replacementFaceCount);
    object.insert("replacement_edge_count", report.replacementEdgeCount);
    object.insert("replacement_shell_count", report.replacementShellCount);
    object.insert("replacement_solid_count", report.replacementSolidCount);
    object.insert("gate_evaluated", report.gateEvaluated);
    object.insert("gate_passed", report.gatePassed);
    object.insert("gate_after_brep_check_valid", report.gateAfterBRepCheckValid);
    object.insert("gate_roundtrip_brep_check_valid", report.gateRoundtripBRepCheckValid);
    object.insert("gate_step_roundtrip_ok", report.gateStepRoundtripOk);
    object.insert("gate_after_free_edges", report.gateAfterFreeEdges);
    object.insert("gate_after_multiple_edges", report.gateAfterMultipleEdges);
    object.insert("gate_roundtrip_free_edges", report.gateRoundtripFreeEdges);
    object.insert("gate_roundtrip_multiple_edges", report.gateRoundtripMultipleEdges);
    object.insert("failure_reason", QString::fromStdString(spo::toString(report.failureReason)));
    object.insert("gate_failure_reason", QString::fromStdString(report.gateFailureReason));
    object.insert("message", QString::fromStdString(report.message));
    object.insert("warning", QString::fromStdString(report.warningMessage));
    return object;
}

bool write_report(
    const Options& options,
    bool overallSuccess,
    const std::string& stage,
    const spo::StpSampledFittingReport& fitting,
    const spo::GeomagicAutoSurfaceResult& geomagic,
    const spo::PatchPreviewReport& preview,
    const spo::PatchReplacementReport& apply,
    const spo::CommercialCadQualityGateReport& quality,
    const spo::PatchArtifactPaths& artifacts,
    std::string* errorMessage = nullptr) {
    QJsonObject root;
    root.insert("overall_success", overallSuccess);
    root.insert("stage", QString::fromStdString(stage));
    root.insert("source_step", path_to_qstring(options.sourceStep));
    root.insert("candidate_id", options.candidateId);
    root.insert("candidate_selection_mode", options.autoCandidateId ? "auto" : "explicit");
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
    samplingOptions.enableBoundaryGuardBandSampling = options.enableB2BoundaryGuardBand;
    samplingOptions.boundaryGuardBandSamplesPerEdge = options.boundaryGuardBandSamplesPerEdge;
    samplingOptions.boundaryGuardBandRingCount = options.boundaryGuardBandRingCount;
    samplingOptions.boundaryGuardBandSpacing = options.boundaryGuardBandSpacing;
    samplingOptions.enableBoundaryOverCoverStrip = options.enableB2OverCoverStrip;
    samplingOptions.boundaryOverCoverWidth = options.boundaryOverCoverWidth;
    samplingOptions.boundaryOverCoverRingCount = options.boundaryOverCoverRingCount;
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
    if (!previewReport.success || previewReport.highRisk) {
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

    const auto overallSuccess =
        result.success() &&
        applyReport.gateEvaluated &&
        applyReport.gatePassed &&
        qualityReport.evaluated &&
        qualityReport.passed;

    std::string reportError;
    if (!write_report(
            options,
            overallSuccess,
            overallSuccess ? "completed" : "failed_gate",
            fittingReport,
            geomagicResult,
            previewReport,
            applyReport,
            qualityReport,
            artifacts,
            &reportError)) {
        std::cerr << "Report write failed: " << reportError << "\n";
        return 1;
    }

    std::cout << "corner baseline report: " << path_to_string(options.reportPath) << "\n";
    std::cout << "StrictTopologyGate passed: " << applyReport.gatePassed << "\n";
    std::cout << "CommercialCadLikeQualityGate passed: " << qualityReport.passed << "\n";
    std::cout << "boundary max distance: " << qualityReport.boundary.maxDistance << "\n";
    std::cout << "corner max distance: " << qualityReport.cornerAnchors.maxDistance << "\n";
    std::cout << "feature edge max distance: " << qualityReport.featureEdges.maxDistance << "\n";
    return overallSuccess ? 0 : 1;
}
