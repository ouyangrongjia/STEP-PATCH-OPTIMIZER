#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "merge/MergePlanner.h"
#include "reduction/region_refit/RegionalSurfaceRefitEngine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>

namespace {

struct ProbeTiming {
    double inputReadMilliseconds = 0.0;
    double candidateResolutionMilliseconds = 0.0;
    double engineMilliseconds = 0.0;
    double coreTotalMilliseconds = 0.0;
};

std::vector<std::size_t> parseIds(std::string value) {
    std::replace(value.begin(), value.end(), ';', ',');
    std::vector<std::size_t> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(',', begin);
        const auto token = value.substr(begin, end - begin);
        if (!token.empty()) {
            result.push_back(std::stoull(token));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

QJsonArray idsToJson(const std::vector<std::size_t>& ids) {
    QJsonArray result;
    for (const auto id : ids) {
        result.append(QString::number(id));
    }
    return result;
}

QJsonObject stageToJson(const spo::RegionRefitStageReport& stage) {
    return {
        {"stage", spo::toString(stage.stage)},
        {"success", stage.success},
        {"wall_ms", stage.wallMilliseconds},
        {"cpu_ms", stage.cpuMilliseconds},
        {"peak_working_set_bytes", QString::number(stage.peakWorkingSetBytes)},
        {"peak_private_bytes", QString::number(stage.peakPrivateBytes)},
        {"message", QString::fromStdString(stage.message)}};
}

bool writeJson(const std::filesystem::path& path, const QJsonObject& object) {
    std::filesystem::create_directories(path.parent_path());
    QSaveFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0) {
        return false;
    }
    return file.commit();
}

QJsonObject constraintsToJson(const spo::RegionRefitResult& result) {
    QJsonArray boundary;
    for (const auto& edge : result.constraints.boundaryEdges) {
        QJsonObject item {
            {"edge_id", QString::number(edge.edgeId)},
            {"selected_face_id", QString::number(edge.selectedFaceId)},
            {"continuity", spo::toString(edge.continuity)},
            {"hard_position", edge.hardPositionConstraint},
            {"soft_adjacent_normal", edge.softAdjacentNormalConstraint},
            {"selected_pcurve", !edge.selectedPcurve.IsNull()},
            {"adjacent_pcurve", !edge.adjacentPcurve.IsNull()}};
        if (edge.adjacentFaceId.has_value()) {
            item.insert("adjacent_face_id", QString::number(*edge.adjacentFaceId));
        }
        boundary.append(item);
    }
    return {
        {"status", spo::toString(result.report.status)},
        {"message", QString::fromStdString(result.report.message)},
        {"selected_faces", idsToJson(result.constraints.selectedFaces)},
        {"internal_edges", idsToJson(result.constraints.internalEdges)},
        {"locked_internal_edges", idsToJson(result.constraints.lockedInternalEdges)},
        {"ordered_boundary_edges", idsToJson(result.constraints.orderedBoundaryEdges)},
        {"boundary_edges", boundary}};
}

QJsonObject performanceToJson(
    const spo::RegionRefitResult& result,
    const spo::RegionRefitRequest& request,
    const ProbeTiming& timing) {
    QJsonArray stages;
    for (const auto& stage : result.report.stages) {
        stages.append(stageToJson(stage));
    }
    return {
        {"schema_version", 1},
        {"status", spo::toString(result.report.status)},
        {"failure_stage", spo::toString(result.report.failureStage)},
        {"layout_mode", spo::toString(request.layoutMode)},
        {"wall_budget_ms", QString::number(request.options.maximumWallClockMilliseconds)},
        {"input_read_budget_ms", QString::number(request.options.inputReadBudgetMilliseconds)},
        {"memory_budget_mib", QString::number(request.options.maximumMemoryMiB)},
        {"input_read_ms", timing.inputReadMilliseconds},
        {"candidate_resolution_ms", timing.candidateResolutionMilliseconds},
        {"engine_ms", timing.engineMilliseconds},
        {"core_total_ms", timing.coreTotalMilliseconds},
        {"total_wall_ms", result.report.totalWallMilliseconds},
        {"peak_working_set_bytes", QString::number(result.report.peakWorkingSetBytes)},
        {"peak_private_bytes", QString::number(result.report.peakPrivateBytes)},
        {"full_fit_count", result.report.fullFitCount},
        {"stop_reason", QString::fromStdString(result.report.message)},
        {"stages", stages}};
}

}

int main(int argc, char* argv[]) {
    const auto coreStarted = std::chrono::steady_clock::now();
    if (argc < 5) {
        std::cerr << "用法：native_region_refit_probe <输入.step> <输出目录> "
                     "(--candidate-id <编号> | --faces <编号,编号,...>) "
                     "[--locked-edges <编号,编号,...>] [--layout <adaptive|fixed>] "
                     "[--patch-count <数量>] [--wall-time-ms <毫秒>] "
                     "[--max-memory-mib <MiB>] [--constraints-only]\n";
        return 2;
    }

    const std::filesystem::path input = argv[1];
    const std::filesystem::path outputDirectory = argv[2];
    int candidateId = -1;
    bool constraintsOnly = false;
    spo::RegionRefitRequest request;
    try {
        for (int index = 3; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--candidate-id") {
                if (++index >= argc || candidateId >= 0) return 2;
                candidateId = std::stoi(argv[index]);
            } else if (argument == "--faces") {
                if (++index >= argc || !request.selectedFaces.empty()) return 2;
                request.selectedFaces = parseIds(argv[index]);
            } else if (argument == "--locked-edges") {
                if (++index >= argc || !request.lockedEdges.empty()) return 2;
                request.lockedEdges = parseIds(argv[index]);
            } else if (argument == "--layout") {
                if (++index >= argc) return 2;
                const std::string value = argv[index];
                if (value == "adaptive") request.layoutMode = spo::RegionRefitLayoutMode::Adaptive;
                else if (value == "fixed") request.layoutMode = spo::RegionRefitLayoutMode::FixedPatchCount;
                else return 2;
            } else if (argument == "--patch-count") {
                if (++index >= argc) return 2;
                request.options.fixedPatchCount = std::stoi(argv[index]);
            } else if (argument == "--wall-time-ms") {
                if (++index >= argc) return 2;
                request.options.maximumWallClockMilliseconds = std::stoll(argv[index]);
            } else if (argument == "--max-memory-mib") {
                if (++index >= argc) return 2;
                request.options.maximumMemoryMiB = std::stoull(argv[index]);
            } else if (argument == "--constraints-only") {
                constraintsOnly = true;
            } else {
                std::cerr << "未知参数：" << argument << '\n';
                return 2;
            }
        }
    } catch (...) {
        std::cerr << "编号和预算参数必须是有效非负整数。\n";
        return 2;
    }

    if ((candidateId >= 0) == !request.selectedFaces.empty()) {
        std::cerr << "--candidate-id 与 --faces 必须且只能指定一个。\n";
        return 2;
    }
    if (request.layoutMode == spo::RegionRefitLayoutMode::FixedPatchCount &&
        request.options.fixedPatchCount <= 0) {
        std::cerr << "固定布局必须指定正数 --patch-count。\n";
        return 2;
    }

    ProbeTiming timing;
    const auto readStarted = std::chrono::steady_clock::now();
    const auto read = spo::StepReader().read(input);
    timing.inputReadMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - readStarted).count();
    if (!read.status.success()) {
        std::cerr << read.status.message() << '\n';
        return 1;
    }
    if (candidateId >= 0) {
        const auto candidateStarted = std::chrono::steady_clock::now();
        spo::MergePlannerOptions options;
        options.enable_feature_bounded_refit_candidates = true;
        options.min_feature_bounded_region_faces = 2;
        const auto features = spo::FeatureEdgeDetector().detect(read.document.topology(), 25.0, 0.0);
        const auto planned = spo::MergePlanner().plan(read.document, features, {}, options);
        const auto found = std::find_if(
            planned.candidates.begin(),
            planned.candidates.end(),
            [candidateId](const auto& candidate) { return candidate.candidate_id == candidateId; });
        if (found == planned.candidates.end()) {
            std::cerr << "没有找到指定候选区域。\n";
            return 1;
        }
        request.selectedFaces = found->faces;
        timing.candidateResolutionMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - candidateStarted).count();
    }

    const auto engineStarted = std::chrono::steady_clock::now();
    const auto result = spo::RegionalSurfaceRefitEngine().run(read.document, request);
    timing.engineMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - engineStarted).count();
    timing.coreTotalMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - coreStarted).count();
    const auto diagnostics = outputDirectory / "diagnostics";
    if (!writeJson(diagnostics / "constraints.json", constraintsToJson(result)) ||
        !writeJson(diagnostics / "performance_report.json", performanceToJson(result, request, timing))) {
        std::cerr << "无法写入区域重拟合诊断。\n";
        return 1;
    }

    std::cout << "status=" << spo::toString(result.report.status) << '\n'
              << "failure_stage=" << spo::toString(result.report.failureStage) << '\n'
              << "selected_faces=" << result.constraints.selectedFaces.size() << '\n'
              << "boundary_edges=" << result.constraints.boundaryEdges.size() << '\n'
              << "internal_edges=" << result.constraints.internalEdges.size() << '\n'
              << "total_ms=" << result.report.totalWallMilliseconds << '\n'
              << "message=" << result.report.message << '\n';

    if (constraintsOnly) {
        return !result.constraints.boundaryEdges.empty() &&
            !result.report.stages.empty() && result.report.stages.front().success ? 0 : 1;
    }
    return result.canApply() ? 0 : 1;
}
