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
#include <string>

namespace {

struct Options {
    std::filesystem::path sourceStep;
    std::filesystem::path patchPath;
    int candidateId = -1;
    double angularThresholdDegrees = 25.0;
    double minEdgeLength = 0.0;
};

void print_usage() {
    std::cerr
        << "Usage: patch_apply_probe --source-step <model.stp> --patch <patch.stp|patch.igs> --candidate-id <id> "
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
            options.candidateId = std::stoi(value);
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

    return !options.sourceStep.empty() && !options.patchPath.empty() && options.candidateId >= 0;
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
    std::cout << "multi-surface failed edge ids:";
    for (const auto edgeId : report.multiSurfaceFailedEdgeIds) {
        std::cout << " " << edgeId;
    }
    std::cout << "\n";
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
    std::cout << "free edges before/after repair: "
              << report.freeEdgesBeforeRepair << " / "
              << report.freeEdgesAfterRepair << "\n";
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
    plannerOptions.enable_plane_candidates = false;
    plannerOptions.enable_cylinder_candidates = true;
    plannerOptions.enable_sphere_candidates = false;
    plannerOptions.enable_cone_candidates = true;
    plannerOptions.enable_torus_candidates = true;
    plannerOptions.enable_feature_bounded_refit_candidates = true;
    plannerOptions.min_feature_bounded_region_faces = 2;
    plannerOptions.min_analytic_region_faces = 2;
    plannerOptions.min_region_faces = 2;
    plannerOptions.max_sphere_center_delta = 0.50;
    plannerOptions.max_sphere_radius_delta = 0.25;

    print_stage("planning merge candidates");
    const auto planner = spo::MergePlanner().plan(document, featureEdges, {}, plannerOptions);
    const auto* candidate = find_candidate(planner.candidates, options.candidateId);
    if (candidate == nullptr) {
        std::cerr << "Candidate id not found. generated candidates: "
                  << planner.candidates.size() << "\n";
        return 1;
    }

    print_stage("analyzing original boundary");
    const auto boundary = spo::RegionBoundaryAnalyzer().analyze(document, *candidate);
    if (!boundary.valid) {
        std::cerr << "Boundary analysis failed: " << boundary.message << "\n";
        return 1;
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
