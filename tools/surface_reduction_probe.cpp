#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "io/StlReader.h"
#include "merge/MergePlanner.h"
#include "reduction/SurfaceReductionEngine.h"
#include "reduction/RestrictedBSplinePatchFitter.h"
#include "stl/StlRegionIndex.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::string sha256File(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) hash.addData(file.read(1024 * 1024));
    return hash.result().toHex().toStdString();
}

QJsonArray facesToJson(const std::vector<spo::FaceId>& faces) {
    QJsonArray result;
    for (const auto face : faces) result.append(QString::number(face));
    return result;
}

QJsonObject fitTimingToJson(const spo::RestrictedBSplinePatchFitReport& fit) {
    return {
        {"total_ms", fit.timing.totalMs},
        {"topology_preparation_ms", fit.timing.topologyPreparationMs},
        {"scan_crop_ms", fit.timing.scanCropMs},
        {"sampling_ms", fit.timing.samplingMs},
        {"parameterization_ms", fit.timing.parameterizationMs},
        {"kkt_solve_ms", fit.timing.kktSolveMs},
        {"uv_projection_ms", fit.timing.uvProjectionMs},
        {"boundary_refinement_ms", fit.timing.boundaryRefinementMs},
        {"validation_ms", fit.timing.validationMs},
        {"retrim_ms", fit.timing.retrimMs},
        {"brep_check_ms", fit.timing.brepCheckMs},
        {"solve_count", fit.kktSolveCount},
        {"boundary_projection_count", fit.boundaryProjectionCount},
        {"scan_index_candidate_triangles", fit.scanIndexCandidateTriangleCount}};
}

QJsonObject reportToJson(const spo::SurfaceReductionResult& result) {
    const auto& report = result.report;
    QJsonArray groups;
    for (const auto& group : report.groupReports) {
        groups.append(QJsonObject {
            {"faces", facesToJson(group.faces)},
            {"method", spo::toString(group.method)},
            {"fit", fitTimingToJson(group.fit)}});
    }
    QJsonArray rejected;
    for (const auto& group : report.rejectedGroups) {
        rejected.append(QJsonObject {
            {"faces", facesToJson(group.faces)},
            {"stage", spo::toString(group.stage)},
            {"reason", QString::fromStdString(group.reason)},
            {"fit", fitTimingToJson(group.fit)}});
    }
    QJsonArray profiles;
    for (const auto& profile : report.fitProfiles) {
        profiles.append(QJsonObject {
            {"faces", facesToJson(profile.faces)},
            {"success", profile.success},
            {"failure_stage", spo::toString(profile.failureStage)},
            {"total_ms", profile.totalMs},
            {"solve_count", profile.solveCount},
            {"boundary_projection_count", profile.boundaryProjectionCount},
            {"scan_index_candidate_triangles", profile.scanIndexCandidateTriangleCount},
            {"persistent_cache_hit", profile.persistentCacheHit}});
    }
    QJsonArray outputGroups;
    for (const auto& group : report.outputGroups) outputGroups.append(facesToJson(group));
    return {
        {"outcome", spo::toString(report.outcome)},
        {"message", QString::fromStdString(report.message)},
        {"input_faces", report.inputFaceCount},
        {"output_faces", report.outputFaceCount},
        {"output_groups", outputGroups},
        {"max_distance", report.maxDistance},
        {"max_normal_degrees", report.maxNormalAngleDegrees},
        {"max_smooth_seam_degrees", report.maxSmoothSeamAngleDegrees},
        {"all_quality_gates_passed", report.allQualityGatesPassed},
        {"brep_check_valid", report.topologyReport.afterBRepCheckValid},
        {"step_roundtrip_ok", report.topologyReport.stepRoundtripOk},
        {"after_free_edges", report.topologyReport.afterFreeEdges},
        {"after_multiple_edges", report.topologyReport.afterMultipleEdges},
        {"attempted_merges", report.attemptedMergeCount},
        {"fit_cache_hits", report.fitCacheHitCount},
        {"fit_cache_misses", report.fitCacheMissCount},
        {"persistent_cache_hits", report.persistentFitCacheHitCount},
        {"persistent_cache_misses", report.persistentFitCacheMissCount},
        {"persistent_cache_writes", report.persistentFitCacheWriteCount},
        {"persistent_cache_required_miss", report.persistentFitCacheRequiredMiss},
        {"fit_workers", report.fitWorkerCount},
        {"scan_index_candidate_triangles", report.scanIndexCandidateTriangleCount},
        {"fit_p50_ms", report.fitDurationP50Ms},
        {"fit_p95_ms", report.fitDurationP95Ms},
        {"fit_p99_ms", report.fitDurationP99Ms},
        {"timing", QJsonObject {
            {"total_ms", report.timing.totalMs},
            {"topology_precomputation_ms", report.timing.topologyPrecomputationMs},
            {"pair_queue_ms", report.timing.pairQueueMs},
            {"exact_merge_ms", report.timing.exactMergeMs},
            {"restricted_fit_ms", report.timing.restrictedFitMs},
            {"seam_validation_ms", report.timing.seamValidationMs},
            {"assembly_commercial_cad_ms", report.timing.assemblyAndCommercialCadMs},
            {"strict_topology_step_roundtrip_ms", report.timing.strictTopologyAndStepRoundtripMs}}},
        {"fit_profiles", profiles},
        {"groups", groups},
        {"rejected_groups", rejected}};
}

bool writeJsonReport(const std::filesystem::path& path, const spo::SurfaceReductionResult& result) {
    if (path.empty()) return true;
    std::filesystem::create_directories(path.parent_path());
    QSaveFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(QJsonDocument(reportToJson(result)).toJson(QJsonDocument::Indented)) < 0) return false;
    return file.commit();
}

spo::SurfaceReductionFailureStage mapFailureStage(
    spo::RestrictedBSplinePatchFitFailureStage stage) {
    using Source = spo::RestrictedBSplinePatchFitFailureStage;
    using Target = spo::SurfaceReductionFailureStage;
    switch (stage) {
    case Source::None: return Target::None;
    case Source::Parameterization: return Target::Parameterization;
    case Source::BoundaryConstraints: return Target::BoundaryConstraints;
    case Source::UvIteration: return Target::UvIteration;
    case Source::ModelSelection: return Target::ModelSelection;
    case Source::Holdout: return Target::Holdout;
    case Source::Jacobian: return Target::Jacobian;
    case Source::Retrim: return Target::Retrim;
    case Source::BoundaryHoldout: return Target::BoundaryHoldout;
    case Source::BrepCheck: return Target::BrepCheck;
    }
    return Target::None;
}

}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "用法：surface_reduction_probe <输入.step> <输出.step> "
                     "[--source-stl <原始.stl> | --cropped-stl <选区裁剪.stl>] "
                     "[--target-ratio <0到1>] "
                     "[--cache-mode <disabled|read-write|read-only-required|refresh>] "
                     "[--cache-dir <目录>] [--algorithm-fingerprint <指纹>] "
                     "[--workers <数量>] [--report-json <报告.json>] "
                     "[--local-fit-only] "
                     "[--local-fit-group <面编号,面编号,...>] "
                     "[--maximum-fit-evaluations <数量>] "
                     "[--maximum-proposal-group-size <数量>] "
                     "[--selection-output <独立选区.step>] "
                     "(<面编号> <面编号> [...] | --candidate-id <候选编号>)\n";
        return 2;
    }
    const std::filesystem::path input = argv[1];
    const std::filesystem::path output = argv[2];
    const auto read = spo::StepReader().read(input);
    if (!read.status.success()) {
        std::cerr << read.status.message() << '\n';
        return 1;
    }

    spo::SurfaceReductionRequest request;
    int candidateId = -1;
    std::filesystem::path sourceStl;
    std::filesystem::path selectionOutput;
    bool sourceStlIsSelectionCrop = false;
    double targetRatio = -1.0;
    spo::SurfaceReductionExecutionOptions execution;
    std::filesystem::path reportJson;
    bool localFitOnly = false;
    std::vector<std::vector<spo::FaceId>> localFitGroups;
    try {
        for (int index = 3; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--source-stl" || argument == "--cropped-stl") {
                if (++index >= argc || !sourceStl.empty()) {
                    std::cerr << "只能指定一个原始 STL 或选区裁剪 STL 路径。\n";
                    return 2;
                }
                sourceStl = argv[index];
                sourceStlIsSelectionCrop = argument == "--cropped-stl";
            } else if (argument == "--target-ratio") {
                if (++index >= argc || targetRatio > 0.0) {
                    std::cerr << "--target-ratio 必须且只能指定一次。\n";
                    return 2;
                }
                targetRatio = std::stod(argv[index]);
            } else if (argument == "--candidate-id") {
                if (++index >= argc || candidateId >= 0) {
                    std::cerr << "--candidate-id 必须且只能指定一个候选编号。\n";
                    return 2;
                }
                candidateId = std::stoi(argv[index]);
            } else if (argument == "--selection-output") {
                if (++index >= argc || !selectionOutput.empty()) {
                    std::cerr << "--selection-output 必须且只能指定一个输出路径。\n";
                    return 2;
                }
                selectionOutput = argv[index];
            } else if (argument == "--cache-mode") {
                if (++index >= argc) return 2;
                const std::string value = argv[index];
                if (value == "disabled") execution.cacheMode = spo::SurfaceReductionCacheMode::Disabled;
                else if (value == "read-write") execution.cacheMode = spo::SurfaceReductionCacheMode::ReadWrite;
                else if (value == "read-only-required") execution.cacheMode = spo::SurfaceReductionCacheMode::ReadOnlyRequired;
                else if (value == "refresh") execution.cacheMode = spo::SurfaceReductionCacheMode::Refresh;
                else { std::cerr << "未知缓存模式。\n"; return 2; }
            } else if (argument == "--cache-dir") {
                if (++index >= argc) return 2;
                execution.cacheDirectory = argv[index];
            } else if (argument == "--algorithm-fingerprint") {
                if (++index >= argc) return 2;
                execution.algorithmFingerprint = argv[index];
            } else if (argument == "--workers") {
                if (++index >= argc) return 2;
                execution.fitWorkerCount = std::stoi(argv[index]);
            } else if (argument == "--report-json") {
                if (++index >= argc) return 2;
                reportJson = argv[index];
            } else if (argument == "--maximum-fit-evaluations") {
                if (++index >= argc) return 2;
                request.options.maximumRestrictedFitEvaluations = std::stoi(argv[index]);
            } else if (argument == "--maximum-proposal-group-size") {
                if (++index >= argc) return 2;
                execution.maximumProposalGroupSize = std::stoi(argv[index]);
            } else if (argument == "--local-fit-only") {
                localFitOnly = true;
            } else if (argument == "--local-fit-group") {
                if (++index >= argc) return 2;
                std::vector<spo::FaceId> group;
                std::string value = argv[index];
                std::replace(value.begin(), value.end(), ';', ',');
                std::size_t begin = 0;
                while (begin <= value.size()) {
                    const auto end = value.find(',', begin);
                    const auto token = value.substr(begin, end - begin);
                    if (!token.empty()) group.push_back(static_cast<spo::FaceId>(std::stoull(token)));
                    if (end == std::string::npos) break;
                    begin = end + 1;
                }
                if (group.size() < 2) return 2;
                localFitGroups.push_back(std::move(group));
            } else {
                request.selectedFaces.push_back(static_cast<spo::FaceId>(std::stoull(argument)));
            }
        }
    } catch (...) {
        std::cerr << "候选编号和面编号必须是非负整数。\n";
        return 2;
    }
    if (candidateId < -1 || (candidateId >= 0 &&
        (!request.selectedFaces.empty() || !localFitGroups.empty()))) {
        std::cerr << "候选编号必须是非负整数，且不能与显式面编号同时使用。\n";
        return 2;
    }
    if (candidateId >= 0) {
        spo::MergePlannerOptions plannerOptions;
        plannerOptions.enable_feature_bounded_refit_candidates = true;
        plannerOptions.min_feature_bounded_region_faces = 2;
        const auto features = spo::FeatureEdgeDetector().detect(read.document.topology(), 25.0, 0.0);
        const auto planned = spo::MergePlanner().plan(read.document, features, {}, plannerOptions);
        const auto candidate = std::find_if(
            planned.candidates.begin(), planned.candidates.end(),
            [&](const auto& value) { return value.candidate_id == candidateId; });
        if (candidate == planned.candidates.end()) {
            std::cerr << "没有找到指定候选区域。\n";
            return 1;
        }
        request.selectedFaces = candidate->faces;
    }
    if (request.selectedFaces.size() < 2 && localFitGroups.empty()) {
        std::cerr << "至少需要两个面或一个有效候选区域。\n";
        return 2;
    }
    if (targetRatio > 0.0) {
        if (!(targetRatio < 1.0)) {
            std::cerr << "目标比例必须大于0且小于1。\n";
            return 2;
        }
        request.options.targetOutputRatio = targetRatio;
    }
    if (!sourceStl.empty()) {
        auto stlRead = spo::StlReader().read(sourceStl);
        if (!stlRead.success) {
            std::cerr << stlRead.message << '\n';
            return 1;
        }
        request.scanReferenceStl = std::make_shared<spo::StlMesh>(std::move(stlRead.mesh));
        request.scanReferenceCoverage = sourceStlIsSelectionCrop
            ? spo::ScanReferenceCoverage::SelectionCrop
            : spo::ScanReferenceCoverage::WholeModel;
    }
    if (execution.cacheMode != spo::SurfaceReductionCacheMode::Disabled) {
        execution.sourceFingerprint = sha256File(input);
        execution.scanFingerprint = sourceStl.empty() ? "none" : sha256File(sourceStl);
        if (execution.sourceFingerprint.empty() || execution.scanFingerprint.empty() ||
            execution.algorithmFingerprint.empty()) {
            std::cerr << "持久缓存所需的文件或算法指纹无效。\n";
            return 2;
        }
    }
    execution.detailedTiming = true;

    std::cout << "candidate_id=" << candidateId << '\n'
              << "selected_faces=";
    for (std::size_t index = 0; index < request.selectedFaces.size(); ++index) {
        std::cout << (index == 0 ? "" : ";") << request.selectedFaces[index];
    }
    std::cout << '\n'
              << "source_stl=" << sourceStl.string() << '\n'
              << "source_stl_is_selection_crop=" << sourceStlIsSelectionCrop << '\n'
              << "target_ratio=" << request.options.targetOutputRatio << '\n'
              << std::flush;

    spo::SurfaceReductionResult result;
    if (localFitOnly) {
        spo::RestrictedBSplinePatchFitOptions options;
        options.tolerance3d = request.options.tolerance3d;
        options.normalToleranceDegrees = request.options.normalToleranceDegrees;
        options.maximumControlPoints = request.options.maximumControlPoints;
        options.maximumBoundaryConstraintRefinements = request.options.maximumBoundaryConstraintRefinements;
        options.maximumUvIterations = request.options.maximumUvIterations;
        options.maximumUvBacktrackingSteps = request.options.maximumUvBacktrackingSteps;
        options.uvConvergenceTolerance = request.options.uvConvergenceTolerance;
        options.scanReference = request.scanReferenceStl.get();
        options.scanReferenceCoverage = request.scanReferenceCoverage;
        options.scanReferenceWeight = request.options.scanReferenceWeight;
        options.maximumScanReferenceDistance = request.options.maximumScanReferenceDistance;
        std::unique_ptr<spo::StlRegionIndex> scanIndex;
        if (request.scanReferenceStl != nullptr &&
            request.scanReferenceCoverage == spo::ScanReferenceCoverage::WholeModel) {
            scanIndex = std::make_unique<spo::StlRegionIndex>(*request.scanReferenceStl);
            options.scanRegionIndex = scanIndex.get();
        }
        result.sourceShape = read.document.shape();
        const auto groups = localFitGroups.empty()
            ? std::vector<std::vector<spo::FaceId>> {request.selectedFaces}
            : localFitGroups;
        bool allSucceeded = true;
        for (const auto& group : groups) {
            const auto fit = spo::RestrictedBSplinePatchFitter().fit(read.document, group, options);
            result.report.inputFaceCount += static_cast<int>(group.size());
            result.report.outputFaceCount += fit.success() ? 1 : static_cast<int>(group.size());
            result.report.timing.totalMs += fit.report.timing.totalMs;
            ++result.report.fitCacheMissCount;
            result.report.fitProfiles.push_back({
                group,
                fit.success(),
                mapFailureStage(fit.report.failureStage),
                fit.report.timing.totalMs,
                fit.report.kktSolveCount,
                fit.report.boundaryProjectionCount,
                fit.report.scanIndexCandidateTriangleCount,
                false});
            if (fit.success()) {
                result.report.outputGroups.push_back(group);
                result.report.groupReports.push_back({
                    group,
                    spo::SurfaceReductionMergeMethod::RestrictedBSpline,
                    fit.report});
            } else {
                allSucceeded = false;
                result.report.rejectedGroups.push_back({
                    group,
                    mapFailureStage(fit.report.failureStage),
                    fit.report.message,
                    fit.report});
            }
        }
        result.report.outcome = allSucceeded
            ? spo::SurfaceReductionOutcome::TargetAchieved
            : spo::SurfaceReductionOutcome::NoAcceptableReduction;
        result.report.message = allSucceeded
            ? "代表分组局部拟合全部通过。"
            : "代表分组局部拟合已完成，包含预期拒绝项。";
    } else {
        result = spo::SurfaceReductionEngine().reduce(read.document, request, execution);
    }
    std::cout << "outcome=" << spo::toString(result.report.outcome) << '\n'
              << "input_faces=" << result.report.inputFaceCount << '\n'
              << "output_faces=" << result.report.outputFaceCount << '\n'
              << "reduction_ratio=" << result.report.reductionRatio << '\n'
              << "max_distance=" << result.report.maxDistance << '\n'
              << "max_normal_degrees=" << result.report.maxNormalAngleDegrees << '\n'
              << "max_smooth_seam_degrees=" << result.report.maxSmoothSeamAngleDegrees << '\n'
              << "parameterization_valid=" << result.report.parameterizationValid << '\n'
              << "brep_check_valid=" << result.report.topologyReport.afterBRepCheckValid << '\n'
              << "step_roundtrip_ok=" << result.report.topologyReport.stepRoundtripOk << '\n'
              << "commercial_cad_gate_count=" << result.report.commercialCadReports.size() << '\n'
              << "attempted_merges=" << result.report.attemptedMergeCount << '\n'
              << "accepted_merges=" << result.report.acceptedMergeCount << '\n'
              << "expanded_states=" << result.report.expandedStateCount << '\n'
              << "generated_states=" << result.report.generatedStateCount << '\n'
              << "deduplicated_states=" << result.report.deduplicatedStateCount << '\n'
              << "fit_cache_hits=" << result.report.fitCacheHitCount << '\n'
              << "fit_cache_misses=" << result.report.fitCacheMissCount << '\n'
              << "persistent_fit_cache_hits=" << result.report.persistentFitCacheHitCount << '\n'
              << "persistent_fit_cache_misses=" << result.report.persistentFitCacheMissCount << '\n'
              << "persistent_fit_cache_writes=" << result.report.persistentFitCacheWriteCount << '\n'
              << "fit_workers=" << result.report.fitWorkerCount << '\n'
              << "total_ms=" << result.report.timing.totalMs << '\n'
              << "search_budget_exhausted=" << result.report.searchBudgetExhausted << '\n'
              << "step_parameterization_used=" << result.report.stepParameterizationUsed << '\n'
              << "scan_reference_coverage=" << spo::toString(result.report.scanReferenceCoverage) << '\n'
              << "scan_reference_samples=" << result.report.scanReferenceSampleCount << '\n'
              << "max_step_to_scan_distance=" << result.report.maxStepToScanDistance << '\n'
              << "rejected_groups=" << result.report.rejectedGroups.size() << '\n'
              << "message=" << result.report.message << '\n';
    std::cout << "topology_failure_reason=" << spo::toString(result.report.topologyReport.failureReason) << '\n'
              << "topology_message=" << result.report.topologyReport.message << '\n'
              << "before_free_edges=" << result.report.topologyReport.beforeFreeEdges << '\n'
              << "after_free_edges=" << result.report.topologyReport.afterFreeEdges << '\n'
              << "before_multiple_edges=" << result.report.topologyReport.beforeMultipleEdges << '\n'
              << "after_multiple_edges=" << result.report.topologyReport.afterMultipleEdges << '\n';
    for (std::size_t index = 0; index < result.report.outputGroups.size(); ++index) {
        if (result.report.outputGroups[index].size() < 2) {
            continue;
        }
        std::cout << "output_group_" << index << "_faces=";
        for (std::size_t faceIndex = 0; faceIndex < result.report.outputGroups[index].size(); ++faceIndex) {
            std::cout << (faceIndex == 0 ? "" : ";") << result.report.outputGroups[index][faceIndex];
        }
        std::cout << '\n';
    }
    for (std::size_t index = 0; index < result.report.groupReports.size(); ++index) {
        const auto& group = result.report.groupReports[index];
        std::cout << "group_" << index << "_method=" << spo::toString(group.method) << '\n'
                  << "group_" << index << "_parameterization="
                  << spo::toString(group.fit.parameterizationSource) << '\n'
                  << "group_" << index << "_boundary_constraints="
                  << group.fit.boundaryConstraintCount << '\n'
                  << "group_" << index << "_boundary_refinements="
                  << group.fit.boundaryConstraintRefinementCount << '\n'
                  << "group_" << index << "_exterior_guards="
                  << group.fit.exteriorGuardSampleCount << '\n'
                  << "group_" << index << "_max_constraint_residual="
                  << group.fit.maxBoundaryConstraintResidual << '\n'
                  << "group_" << index << "_surface_extent_ratio="
                  << group.fit.supportSurfaceExtentRatio << '\n'
                  << "group_" << index << "_surface_pole_overflow_ratio="
                  << group.fit.supportSurfaceMaxPoleOverflowRatio << '\n'
                  << "group_" << index << "_scan_samples="
                  << group.fit.scanReferenceSampleCount << '\n';
        for (std::size_t iteration = 0; iteration < group.fit.uvIterations.size(); ++iteration) {
            const auto& uv = group.fit.uvIterations[iteration];
            std::cout << "group_" << index << "_uv_" << iteration << "="
                      << uv.objectiveBefore << ';' << uv.objectiveAfter << ';'
                      << uv.maximumShift << ';' << uv.backtrackingSteps << ';'
                      << uv.accepted << ';' << uv.reason << '\n';
        }
    }
    for (std::size_t index = 0; index < result.report.rejectedGroups.size(); ++index) {
        const auto& rejected = result.report.rejectedGroups[index];
        std::cout << "rejection_" << index << "_faces=";
        for (std::size_t faceIndex = 0; faceIndex < rejected.faces.size(); ++faceIndex) {
            std::cout << (faceIndex == 0 ? "" : ";") << rejected.faces[faceIndex];
        }
        std::cout << '\n'
                  << "rejection_" << index << "_stage=" << spo::toString(rejected.stage) << '\n'
                  << "rejection_" << index << "_reason=" << rejected.reason << '\n'
                  << "rejection_" << index << "_parameterization="
                  << spo::toString(rejected.fit.parameterizationSource) << '\n'
                  << "rejection_" << index << "_parameterization_message="
                  << rejected.fit.parameterization.message << '\n'
                  << "rejection_" << index << "_controls="
                  << rejected.fit.uControlPointCount << 'x'
                  << rejected.fit.vControlPointCount << '\n'
                  << "rejection_" << index << "_boundary_constraints="
                  << rejected.fit.boundaryConstraintCount << '\n'
                  << "rejection_" << index << "_boundary_refinements="
                  << rejected.fit.boundaryConstraintRefinementCount << '\n'
                  << "rejection_" << index << "_exterior_guards="
                  << rejected.fit.exteriorGuardSampleCount << '\n'
                  << "rejection_" << index << "_constraint_residual="
                  << rejected.fit.maxBoundaryConstraintResidual << '\n'
                  << "rejection_" << index << "_surface_extent_ratio="
                  << rejected.fit.supportSurfaceExtentRatio << '\n'
                  << "rejection_" << index << "_surface_pole_overflow_ratio="
                  << rejected.fit.supportSurfaceMaxPoleOverflowRatio << '\n'
                  << "rejection_" << index << "_boundary_distance="
                  << rejected.fit.maxBoundaryDistance << '\n'
                  << "rejection_" << index << "_selection_distance="
                  << rejected.fit.maxSelectionDistance << '\n'
                  << "rejection_" << index << "_selection_normal="
                  << rejected.fit.maxSelectionNormalAngleDegrees << '\n'
                  << "rejection_" << index << "_holdout_distance="
                  << rejected.fit.maxHoldoutDistance << '\n'
                  << "rejection_" << index << "_holdout_normal="
                  << rejected.fit.maxHoldoutNormalAngleDegrees << '\n'
                  << "rejection_" << index << "_min_jacobian="
                  << rejected.fit.minimumNormalizedJacobian << '\n'
                  << "rejection_" << index << "_scan_samples="
                  << rejected.fit.scanReferenceSampleCount << '\n';
    }
    if (!writeJsonReport(reportJson, result)) {
        std::cerr << "无法原子写入结构化 JSON 报告。\n";
        return 1;
    }
    if (localFitOnly) {
        return localFitGroups.empty() &&
            result.report.outcome != spo::SurfaceReductionOutcome::TargetAchieved ? 1 : 0;
    }
    if (!result.canApply()) {
        return 1;
    }
    const spo::ShapeDocument reduced(result.reducedShape, input);
    const auto write = spo::StepWriter().write(reduced, output);
    if (!write.success()) {
        std::cerr << write.message() << '\n';
        return 1;
    }
    if (!selectionOutput.empty()) {
        if (result.previewShape.IsNull()) {
            std::cerr << "减面结果没有可导出的独立选区形状。\n";
            return 1;
        }
        const spo::ShapeDocument selection(result.previewShape, input);
        if (selection.topology().faceCount() !=
            static_cast<std::size_t>(result.report.outputFaceCount)) {
            std::cerr << "独立选区面数与减面报告不一致。\n";
            return 1;
        }
        const auto selectionWrite = spo::StepWriter().write(selection, selectionOutput);
        if (!selectionWrite.success()) {
            std::cerr << selectionWrite.message() << '\n';
            return 1;
        }
    }
    return 0;
}
