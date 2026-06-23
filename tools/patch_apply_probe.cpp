#include "command/CommandContext.h"
#include "command/PatchReplacementCommand.h"
#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "merge/MergePlanner.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/PatchArtifactLocator.h"
#include "patch/PatchImportService.h"
#include "patch/PatchPreviewReport.h"

#include <filesystem>
#include <iostream>
#include <set>
#include <algorithm>
#include <string>
#include <vector>

namespace {

struct Options {
    std::filesystem::path sourceStep;
    std::filesystem::path patchPath;
    int candidateId = -1;
    bool autoCandidateId = false;
    double angularThresholdDegrees = 25.0;
    double minEdgeLength = 0.0;
};

void print_usage() {
    std::cerr
        << "Usage: patch_apply_probe --source-step <model.stp> --patch <patch.stp|patch.igs> --candidate-id <id|auto> "
        << "[--angle <degrees>] [--min-edge-length <value>]\n";
}

bool parse_options(int argc, char* argv[], Options& options) {
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
                options.candidateId = std::stoi(candidateValue);
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
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    return !options.sourceStep.empty() &&
        !options.patchPath.empty() &&
        (options.autoCandidateId || options.candidateId >= 0);
}

const spo::MergeCandidate* find_candidate(
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

void print_edge_ids(const char* label, const std::vector<spo::EdgeId>& edgeIds) {
    std::cout << label << ":";
    if (edgeIds.empty()) {
        std::cout << " []\n";
        return;
    }
    std::cout << " [";
    for (std::size_t index = 0; index < edgeIds.size(); ++index) {
        if (index > 0) {
            std::cout << ", ";
        }
        std::cout << edgeIds[index];
    }
    std::cout << "]\n";
}

void print_report(const spo::PatchReplacementReport& report) {
    std::cout << "patch status: " << (report.success ? "Applied" : "ApplyFailed") << "\n";
    std::cout << "candidate id: " << report.candidateId << "\n";
    std::cout << "source face count: " << report.sourceFaceCount << "\n";
    std::cout << "source boundary edge count: " << report.sourceBoundaryEdgeCount << "\n";
    std::cout << "patch face count: " << report.patchFaceCount << "\n";
    std::cout << "replacement face count: " << report.replacementFaceCount << "\n";
    std::cout << "used original-boundary surface retrim: " << report.usedOriginalBoundarySurfaceRetrim << "\n";
    std::cout << "attempted multi-surface boundary shell: " << report.attemptedMultiSurfaceBoundaryShell << "\n";
    std::cout << "used multi-surface boundary shell: " << report.usedMultiSurfaceBoundaryShell << "\n";
    std::cout << "retrim boundary pcurve rebuild attempt/success/failure: "
              << report.retrimBoundaryEdgePcurveRebuildAttemptCount << " / "
              << report.retrimBoundaryEdgePcurveRebuildSuccessCount << " / "
              << report.retrimBoundaryEdgePcurveRebuildFailureCount << "\n";
    std::cout << "retrim boundary SameParameter check/failure/maxdev: "
              << report.retrimBoundaryEdgeSameParameterCheckCount << " / "
              << report.retrimBoundaryEdgeSameParameterFailureCount << " / "
              << report.retrimBoundaryEdgeMaxSameParameterDeviation << "\n";
    print_edge_ids(
        "retrim boundary pcurve rebuild failed edge ids",
        report.retrimBoundaryEdgePcurveRebuildFailedEdgeIds);
    print_edge_ids(
        "retrim boundary SameParameter failed edge ids",
        report.retrimBoundaryEdgeSameParameterFailedEdgeIds);
    std::cout << "multi-surface projected/failed samples: "
              << report.multiSurfaceProjectedSampleCount << " / "
              << report.multiSurfaceFailedProjectionCount << "\n";
    std::cout << "multi-surface max/avg projection distance: "
              << report.multiSurfaceMaxProjectionDistance << " / "
              << report.multiSurfaceAverageProjectionDistance << "\n";
    std::cout << "multi-surface assigned boundary segments: "
              << report.multiSurfaceAssignedBoundarySegmentCount << "\n";
    std::cout << "multi-surface split boundary edges: "
              << report.multiSurfaceSplitBoundaryEdgeCount << "\n";
    std::cout << "multi-surface built faces: " << report.multiSurfaceBuiltFaceCount << "\n";
    std::cout << "multi-surface closed/open wires: "
              << report.multiSurfaceClosedWireCount << " / "
              << report.multiSurfaceOpenWireCount << "\n";
    std::cout << "multi-surface multiple-closed-wire faces: "
              << report.multiSurfaceMultipleClosedWireFaceCount << "\n";
    std::cout << "multi-surface failed patch face index: "
              << report.multiSurfaceFailedPatchFaceIndex << "\n";
    std::cout << "multi-surface failed face edge count: "
              << report.multiSurfaceFailedFaceEdgeCount << "\n";
    std::cout << "multi-surface boundary pcurve rebuild attempt/success/failure: "
              << report.multiSurfaceBoundaryEdgePcurveRebuildAttemptCount << " / "
              << report.multiSurfaceBoundaryEdgePcurveRebuildSuccessCount << " / "
              << report.multiSurfaceBoundaryEdgePcurveRebuildFailureCount << "\n";
    std::cout << "multi-surface boundary SameParameter check/failure/maxdev: "
              << report.multiSurfaceBoundaryEdgeSameParameterCheckCount << " / "
              << report.multiSurfaceBoundaryEdgeSameParameterFailureCount << " / "
              << report.multiSurfaceBoundaryEdgeMaxSameParameterDeviation << "\n";
    print_edge_ids(
        "multi-surface boundary pcurve rebuild failed edge ids",
        report.multiSurfaceBoundaryEdgePcurveRebuildFailedEdgeIds);
    print_edge_ids(
        "multi-surface boundary SameParameter failed edge ids",
        report.multiSurfaceBoundaryEdgeSameParameterFailedEdgeIds);
    std::cout << "source faces replaced: " << report.sourceFacesReplaced << "\n";
    std::cout << "repair applied: " << report.repairApplied << "\n";
    std::cout << "repair before face/edge/shell/solid: "
              << report.faceCountBeforeRepair << " / "
              << report.edgeCountBeforeRepair << " / "
              << report.shellCountBeforeRepair << " / "
              << report.solidCountBeforeRepair << "\n";
    std::cout << "repair after face/edge/shell/solid: "
              << report.faceCountAfterRepair << " / "
              << report.edgeCountAfterRepair << " / "
              << report.shellCountAfterRepair << " / "
              << report.solidCountAfterRepair << "\n";
    std::cout << "pre-repair closure face/edge/shell/solid: "
              << report.preRepairFaceCount << " / "
              << report.preRepairEdgeCount << " / "
              << report.preRepairShellCount << " / "
              << report.preRepairSolidCount
              << " brep=" << report.preRepairBRepCheckValid
              << " free=" << report.preRepairFreeEdgeCount
              << " multiple=" << report.preRepairMultipleEdgeCount
              << " degenerated_free=" << report.preRepairDegeneratedFreeEdgeCount << "\n";
    std::cout << "post-repair closure face/edge/shell/solid: "
              << report.postRepairFaceCount << " / "
              << report.postRepairEdgeCount << " / "
              << report.postRepairShellCount << " / "
              << report.postRepairSolidCount
              << " brep=" << report.postRepairBRepCheckValid
              << " free=" << report.postRepairFreeEdgeCount
              << " multiple=" << report.postRepairMultipleEdgeCount
              << " degenerated_free=" << report.postRepairDegeneratedFreeEdgeCount
              << " appeared_after_repair_degenerated="
              << report.appearedAfterRepairDegeneratedFreeEdgeCount << "\n";
    std::cout << "free edge diagnostics: " << report.freeEdgeDiagnostics.size() << "\n";
    for (const auto& diagnostic : report.freeEdgeDiagnostics) {
        std::cout << "  edge_index=" << diagnostic.edgeIndex
                  << " after_repair=" << diagnostic.afterRepair
                  << " appeared_after_repair=" << diagnostic.appearedAfterRepair
                  << " adjacent_faces=" << diagnostic.adjacentFaceCount
                  << " length=" << diagnostic.edgeLength
                  << " tolerance=" << diagnostic.edgeTolerance
                  << " degenerated=" << diagnostic.degenerated
                  << " nearest_original_boundary_edge_id=" << diagnostic.nearestOriginalBoundaryEdgeId
                  << " nearest_distance=" << diagnostic.nearestOriginalBoundaryEdgeDistance
                  << " original_boundary_length=" << diagnostic.nearestOriginalBoundaryEdgeLength
                  << " original_boundary_tolerance=" << diagnostic.nearestOriginalBoundaryEdgeTolerance
                  << " original_adjacent_faces=" << diagnostic.nearestOriginalBoundaryAdjacentFaceCount
                  << " original_pcurve_faces=" << diagnostic.nearestOriginalBoundaryPcurveAvailableFaceCount
                  << " matched_split=" << diagnostic.matchedSplitBoundarySegment
                  << " split_range=[" << diagnostic.matchedSplitBoundaryFirstParameter
                  << ", " << diagnostic.matchedSplitBoundaryLastParameter << "]"
                  << " patch_face_owner=" << diagnostic.patchFaceOwner
                  << " same_edge_split_count=" << diagnostic.sameOriginalBoundaryEdgeSplitSegmentCount
                  << " owner_switch_count=" << diagnostic.sameOriginalBoundaryEdgeOwnerSwitchCount
                  << " degenerated_split_count=" << diagnostic.sameOriginalBoundaryEdgeDegeneratedSegmentCount
                  << " nearest_split=" << diagnostic.nearestSplitBoundarySegment
                  << " nearest_split_edge_id=" << diagnostic.nearestSplitBoundaryOriginalEdgeId
                  << " nearest_split_distance=" << diagnostic.nearestSplitBoundarySegmentDistance
                  << " nearest_split_range=[" << diagnostic.nearestSplitBoundaryFirstParameter
                  << ", " << diagnostic.nearestSplitBoundaryLastParameter << "]"
                  << " nearest_split_owner=" << diagnostic.nearestSplitBoundaryPatchFaceOwner
                  << " projection_faces=" << diagnostic.fittedPatchProjectionFaceCount
                  << " projection_samples=" << diagnostic.fittedPatchProjectionSampleCount
                  << " projection_failed=" << diagnostic.fittedPatchProjectionFailedCount
                  << " projection_min=" << diagnostic.fittedPatchProjectionMinDistance
                  << " projection_max=" << diagnostic.fittedPatchProjectionMaxDistance
                  << " projection_avg=" << diagnostic.fittedPatchProjectionAverageDistance
                  << " nearest_patch_face=" << diagnostic.nearestFittedPatchFaceIndex;
        if (diagnostic.midpointValid) {
            std::cout << " midpoint=(" << diagnostic.midpointX
                      << ", " << diagnostic.midpointY
                      << ", " << diagnostic.midpointZ << ")";
        }
        if (diagnostic.startPointValid && diagnostic.endPointValid) {
            std::cout << " endpoints=(" << diagnostic.startX
                      << ", " << diagnostic.startY
                      << ", " << diagnostic.startZ << ") -> ("
                      << diagnostic.endX
                      << ", " << diagnostic.endY
                      << ", " << diagnostic.endZ << ")";
        }
        std::cout << "\n";
    }
    std::cout << "free edges before/after repair: "
              << report.freeEdgesBeforeRepair << " / "
              << report.freeEdgesAfterRepair << "\n";
    std::cout << "trim diagnostics captured: " << report.trimDiagnostics.captured << "\n";
    std::cout << "trim diagnostics replacement faces: "
              << report.trimDiagnostics.replacementFaceCount << "\n";
    std::cout << "trim diagnostics invalid wires / uv loop self intersections: "
              << report.trimDiagnostics.trimWireInvalidCount << " / "
              << report.trimDiagnostics.trimUvLoopSelfIntersectionCount << "\n";
    std::cout << "trim diagnostics over-cover count/ratio/max: "
              << report.trimDiagnostics.overCoverSampleCount << " / "
              << report.trimDiagnostics.overCoverRatio << " / "
              << report.trimDiagnostics.overCoverMaxDistance << "\n";
    std::cout << "trim diagnostics under-cover count/max: "
              << report.trimDiagnostics.underCoverSampleCount << " / "
              << report.trimDiagnostics.underCoverMaxDistance << "\n";
    std::cout << "trim diagnostics boundary gap max/p95/rms worst-edge: "
              << report.trimDiagnostics.boundaryGapMax << " / "
              << report.trimDiagnostics.boundaryGapP95 << " / "
              << report.trimDiagnostics.boundaryGapRms << " / "
              << report.trimDiagnostics.worstBoundaryEdgeId << "\n";
    std::cout << "trim diagnostics internal seam gap max/p95/rms worst-edge: "
              << report.trimDiagnostics.internalSeamGapMax << " / "
              << report.trimDiagnostics.internalSeamGapP95 << " / "
              << report.trimDiagnostics.internalSeamGapRms << " / "
              << report.trimDiagnostics.worstInternalEdgeId << "\n";
    std::cout << "trim diagnostics roundtrip compared/changed: "
              << report.trimDiagnostics.roundtripCompared << " / "
              << report.trimDiagnostics.roundtripChanged << "\n";
    std::cout << "external CAD diagnostics captured: "
              << report.externalCadDiagnostics.captured << "\n";
    std::cout << "raw patch preflight available/executed/status: "
              << report.externalCadDiagnostics.rawPatchPreflightAvailable << " / "
              << report.externalCadDiagnostics.rawPatchPreflightExecuted << " / "
              << report.externalCadDiagnostics.rawPatchPreflightStatus << "\n";
    std::cout << "raw patch preflight role/path: "
              << report.externalCadDiagnostics.rawPatchPreflightRole << " / "
              << report.externalCadDiagnostics.rawPatchPreflightInputPath << "\n";
    std::cout << "final applied STEP diagnostic eligible/executed/status: "
              << report.externalCadDiagnostics.finalAppliedStepDiagnosticEligible << " / "
              << report.externalCadDiagnostics.finalAppliedStepDiagnosticExecuted << " / "
              << report.externalCadDiagnostics.finalAppliedStepDiagnosticStatus << "\n";
    std::cout << "final applied STEP diagnostic stage/path: "
              << report.externalCadDiagnostics.finalAppliedStepDiagnosticStage << " / "
              << report.externalCadDiagnostics.finalAppliedStepDiagnosticInputPath << "\n";
    std::cout << "final applied STEP diagnostic skipped reason: "
              << report.externalCadDiagnostics.finalAppliedStepDiagnosticSkippedReason << "\n";
    std::cout << "selected sewing tolerance: " << report.selectedSewingTolerance << "\n";
    std::cout << "sewing attempt count: " << report.sewingAttemptCount << "\n";
    std::cout << "best sewing face/edge/shell/solid: "
              << report.bestSewingFaceCount << " / "
              << report.bestSewingEdgeCount << " / "
              << report.bestSewingShellCount << " / "
              << report.bestSewingSolidCount << "\n";
    std::cout << "best sewing free/multiple edges: "
              << report.bestSewingFreeEdges << " / "
              << report.bestSewingMultipleEdges << "\n";
    std::cout << "repair degenerated free edges before/after: "
              << report.degeneratedFreeEdgesBeforeRepair << " / "
              << report.degeneratedFreeEdgesAfterRepair << "\n";
    std::cout << "best sewing degenerated free edges: "
              << report.bestSewingDegeneratedFreeEdgeCount << "\n";
    std::cout << "best sewing BRepCheck: " << report.bestSewingBRepCheckValid << "\n";
    std::cout << "best sewing collapsed: " << report.bestSewingCollapsed << "\n";
    std::cout << "StrictTopologyGate evaluated: " << report.gateEvaluated << "\n";
    std::cout << "StrictTopologyGate passed: " << report.gatePassed << "\n";
    std::cout << "gate after free/multiple edges: "
              << report.gateAfterFreeEdges << " / "
              << report.gateAfterMultipleEdges << "\n";
    std::cout << "gate after BRepCheck: " << report.gateAfterBRepCheckValid << "\n";
    std::cout << "failure reason: " << spo::toString(report.failureReason) << "\n";
    std::cout << "message: " << report.message << "\n";
    std::cout << "warning: " << report.warningMessage << "\n";
}

void print_stage(const char* stage) {
    std::cerr << "[patch_apply_probe] " << stage << "\n";
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

    auto document = read.document;
    print_stage("detecting feature edges");
    const auto featureEdges = spo::FeatureEdgeDetector().detect(
        document.topology(),
        options.angularThresholdDegrees,
        options.minEdgeLength);

    spo::MergePlannerOptions plannerOptions;
    plannerOptions.enable_feature_bounded_refit_candidates = true;
    plannerOptions.min_feature_bounded_region_faces = 2;

    print_stage("planning merge candidates");
    const auto planner = spo::MergePlanner().plan(document, featureEdges, {}, plannerOptions);
    spo::RegionBoundaryAnalysis boundary;
    std::string selectionMessage;
    const auto* candidate = options.autoCandidateId
        ? select_auto_candidate(document, planner.candidates, boundary, selectionMessage)
        : find_candidate(planner.candidates, options.candidateId);
    if (candidate == nullptr) {
        std::cerr << (selectionMessage.empty()
                ? "Candidate id not found."
                : selectionMessage)
                  << " generated candidates: " << planner.candidates.size() << "\n";
        return 1;
    }
    std::cerr << "[patch_apply_probe] selected candidate id " << candidate->candidate_id;
    if (!selectionMessage.empty()) {
        std::cerr << " (" << selectionMessage << ")";
    }
    std::cerr << "\n";

    if (!options.autoCandidateId) {
        print_stage("analyzing original boundary");
        boundary = spo::RegionBoundaryAnalyzer().analyze(document, *candidate);
        if (!boundary.valid) {
            std::cerr << "Boundary analysis failed: " << boundary.message << "\n";
            return 1;
        }
    }

    print_stage("importing patch");
    auto importedPatch = spo::PatchImportService().importPatch(options.patchPath);
    if (!importedPatch.success) {
        std::cerr << "Patch import failed: " << importedPatch.errorMessage << "\n";
        return 1;
    }

    spo::PatchArtifactPaths artifacts;
    artifacts.success = true;
    artifacts.patchStepPath = options.patchPath;
    artifacts.foundStep = true;

    print_stage("building preview report");
    const auto preview = spo::buildPatchPreviewReport(
        &document,
        candidate,
        importedPatch,
        artifacts);
    if (!preview.success || preview.highRisk) {
        std::cerr << "Patch preview is not apply-ready. success="
                  << preview.success << " highRisk=" << preview.highRisk
                  << " message=" << preview.message
                  << " warning=" << preview.warningMessage << "\n";
        return 1;
    }

    spo::PatchReplacementInput input;
    input.document = &document;
    input.candidate = candidate;
    input.boundary = &boundary;
    input.importedPatch = &importedPatch;
    input.artifactPaths = &artifacts;
    input.previewReport = &preview;

    spo::PatchReplacementReport report;
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
    spo::PatchReplacementCommand command(input, &report, commandOptions);
    const auto result = command.execute(context);
    print_stage("printing apply report");
    print_report(report);
    return result.success() ? 0 : 1;
}
