#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "io/StlWriter.h"
#include "merge/MergePlanner.h"
#include "reduction/region_refit/RegionPatchLayout.h"
#include "reduction/region_refit/JointBSplineNetworkFitter.h"
#include "reduction/region_refit/RegionReferenceMesh.h"
#include "reduction/region_refit/RegionalSurfaceRefitEngine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <QSysInfo>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace {

struct ProbeTiming {
    std::int64_t requestedWallBudgetMilliseconds = 0;
    double inputReadMilliseconds = 0.0;
    double candidateResolutionMilliseconds = 0.0;
    double engineMilliseconds = 0.0;
    double coreTotalMilliseconds = 0.0;
};

std::string sha256File(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result().toHex().toStdString();
}

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

bool writeText(const std::filesystem::path& path, const QByteArray& contents) {
    std::filesystem::create_directories(path.parent_path());
    QSaveFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) {
        return false;
    }
    return file.commit();
}

QJsonObject layoutReportToJson(const spo::RegionRefitResult& result) {
    QJsonArray candidates;
    for (const auto& candidate : result.report.layoutCandidates) {
        candidates.append(QJsonObject {
            {"patch_count", candidate.patchCount},
            {"topology_passed", candidate.topologyPassed},
            {"parameterization_passed", candidate.parameterizationPassed},
            {"coarse_fit_passed", candidate.coarseFitPassed},
            {"selected_for_full_fit", candidate.selectedForFullFit},
            {"proxy_error", candidate.proxyError},
            {"maximum_distortion", candidate.maximumDistortion},
            {"flipped_triangles", candidate.flippedTriangleCount},
            {"matrix_rows", QString::number(candidate.matrixRows)},
            {"matrix_nonzeros", QString::number(candidate.matrixNonZeros)},
            {"estimated_solver_bytes", QString::number(candidate.estimatedSolverBytes)},
            {"wall_ms", candidate.wallMilliseconds},
            {"layout_hash", QString::fromStdString(candidate.layoutHash)},
            {"rejection_reason", QString::fromStdString(candidate.rejectionReason)}});
    }
    return {
        {"status", result.selectedLayout ? "Success" : spo::toString(result.report.status)},
        {"selected_patch_count", result.report.selectedPatchCount},
        {"selected_layout_hash", QString::fromStdString(result.report.selectedLayoutHash)},
        {"candidates", candidates}};
}

QJsonObject selectedLayoutToJson(const spo::RegionPatchLayout& layout) {
    QJsonArray assignments;
    for (const auto patch : layout.trianglePatches) {
        assignments.append(patch);
    }
    QJsonArray patches;
    for (std::size_t patchIndex = 0; patchIndex < layout.patches.size(); ++patchIndex) {
        const auto& patch = layout.patches[patchIndex];
        QJsonArray boundary;
        for (const auto vertex : patch.boundaryLoop) {
            boundary.append(QString::number(vertex));
        }
        patches.append(QJsonObject {
            {"patch", static_cast<int>(patchIndex)},
            {"triangles", QString::number(patch.triangleIndices.size())},
            {"vertices", QString::number(patch.vertexIndices.size())},
            {"boundary_loop", boundary},
            {"matrix_rows", QString::number(patch.matrixRows)},
            {"matrix_nonzeros", QString::number(patch.matrixNonZeros)},
            {"flipped_triangles", patch.flippedTriangleCount},
            {"maximum_distortion", patch.maximumDistortion}});
    }
    QJsonArray seams;
    for (const auto& seam : layout.seams) {
        seams.append(QJsonObject {
            {"first_patch", seam.firstPatch},
            {"second_patch", seam.secondPatch},
            {"mesh_edges", QString::number(seam.meshEdges.size())}});
    }
    return {
        {"schema_version", 1},
        {"patch_count", layout.patchCount},
        {"layout_hash", QString::fromStdString(layout.hash)},
        {"proxy_error", layout.proxyError},
        {"estimated_solver_bytes", QString::number(layout.estimatedSolverBytes)},
        {"triangle_patches", assignments},
        {"patches", patches},
        {"seams", seams}};
}

QJsonObject fitReportToJson(const spo::RegionRefitResult& result) {
    QJsonArray attempts;
    for (const auto& attempt : result.report.fitAttempts) {
        QJsonArray iterations;
        for (const auto& iteration : attempt.iterations) {
            iterations.append(QJsonObject {
                {"iteration", iteration.iteration},
                {"u_control_count", iteration.uControlCount},
                {"v_control_count", iteration.vControlCount},
                {"matrix_rows", QString::number(iteration.matrixRows)},
                {"matrix_columns", QString::number(iteration.matrixColumns)},
                {"matrix_nonzeros", QString::number(iteration.matrixNonZeros)},
                {"estimated_solver_bytes", QString::number(iteration.estimatedSolverBytes)},
                {"symbolic_factorization_reuse_count", iteration.symbolicFactorizationReuseCount},
                {"projection_count", QString::number(iteration.projectionCount)},
                {"uv_iteration_count", QString::number(iteration.uvIterationCount)},
                {"projection_fallback_count", QString::number(iteration.projectionFallbackCount)},
                {"training_maximum_distance", iteration.trainingMaximumDistance},
                {"selection_maximum_distance", iteration.selectionMaximumDistance},
                {"holdout_maximum_distance", iteration.holdoutMaximumDistance},
                {"holdout_maximum_normal_degrees", iteration.holdoutMaximumNormalDegrees},
                {"holdout_95_distance", iteration.holdout95Distance},
                {"holdout_95_normal_degrees", iteration.holdout95NormalDegrees},
                {"holdout_worst_distance_vertex", QString::number(iteration.holdoutWorstDistanceVertex)},
                {"holdout_worst_normal_vertex", QString::number(iteration.holdoutWorstNormalVertex)},
                {"holdout_worst_distance_patch", iteration.holdoutWorstDistancePatch},
                {"holdout_worst_normal_patch", iteration.holdoutWorstNormalPatch},
                {"holdout_worst_distance_internal_seam", iteration.holdoutWorstDistanceInternalSeam},
                {"holdout_worst_normal_internal_seam", iteration.holdoutWorstNormalInternalSeam},
                {"boundary_maximum_distance", iteration.boundaryMaximumDistance},
                {"seam_maximum_distance", iteration.seamMaximumDistance},
                {"wall_ms", iteration.wallMilliseconds},
                {"stop_reason", QString::fromStdString(iteration.stopReason)}});
        }
        attempts.append(QJsonObject {
            {"patch_count", attempt.patchCount},
            {"layout_hash", QString::fromStdString(attempt.layoutHash)},
            {"success", attempt.success},
            {"wall_ms", attempt.wallMilliseconds},
            {"stop_reason", QString::fromStdString(attempt.stopReason)},
            {"iterations", iterations}});
    }
    return {
        {"status", result.fittedNetwork ? "Success" : spo::toString(result.report.status)},
        {"full_fit_count", result.report.fullFitCount},
        {"cached_fit_attempt_count", result.report.cachedFitAttemptCount},
        {"selected_patch_count", result.report.selectedPatchCount},
        {"selected_layout_hash", QString::fromStdString(result.report.selectedLayoutHash)},
        {"attempts", attempts}};
}

QJsonObject retrimReportToJson(const spo::RegionRefitResult& result) {
    return {
        {"status", result.report.retrimReplacementFaceCount > 0
            ? "Success"
            : spo::toString(result.report.status)},
        {"replacement_faces", result.report.retrimReplacementFaceCount},
        {"internal_seams", result.report.retrimInternalSeamCount},
        {"reused_original_boundary_edges", result.report.retrimReusedBoundaryEdgeCount},
        {"split_boundary_segments", result.report.retrimSplitBoundarySegmentCount},
        {"pcurve_projections", result.report.retrimPcurveProjectionCount},
        {"same_parameter_checks", result.report.retrimSameParameterCheckCount},
        {"same_parameter_failures", result.report.retrimSameParameterFailureCount},
        {"maximum_same_parameter_deviation", result.report.retrimMaximumSameParameterDeviation},
        {"assembly_passed", result.report.assemblyPassed}};
}

QJsonObject sewingReportToJson(const spo::RegionRefitResult& result) {
    return {
        {"status", result.report.repairPassed ? "Success" : spo::toString(result.report.status)},
        {"repair_passed", result.report.repairPassed},
        {"selected_sewing_tolerance", result.report.selectedSewingTolerance},
        {"sewing_attempts", result.report.sewingAttemptCount},
        {"free_edges_before_repair", result.report.freeEdgesBeforeRepair},
        {"free_edges_after_repair", result.report.freeEdgesAfterRepair},
        {"multiple_edges_before_repair", result.report.multipleEdgesBeforeRepair},
        {"multiple_edges_after_repair", result.report.multipleEdgesAfterRepair}};
}

QJsonObject strictTopologyReportToJson(const spo::RegionRefitResult& result) {
    return {
        {"passed", result.report.strictTopologyPassed},
        {"failure_reason", QString::fromStdString(result.report.strictTopologyFailureReason)},
        {"after_brep_check_valid", result.report.afterBRepCheckValid},
        {"step_export_ok", result.report.stepExportOk},
        {"step_roundtrip_ok", result.report.stepRoundtripOk},
        {"roundtrip_brep_check_valid", result.report.roundtripBRepCheckValid},
        {"before_solids", result.report.beforeSolidCount},
        {"after_solids", result.report.afterSolidCount},
        {"roundtrip_solids", result.report.roundtripSolidCount},
        {"before_faces", result.report.beforeFaceCount},
        {"after_faces", result.report.afterFaceCount},
        {"roundtrip_faces", result.report.roundtripFaceCount},
        {"before_free_edges", result.report.beforeFreeEdgeCount},
        {"after_free_edges", result.report.afterFreeEdgeCount},
        {"roundtrip_free_edges", result.report.roundtripFreeEdgeCount},
        {"before_multiple_edges", result.report.beforeMultipleEdgeCount},
        {"after_multiple_edges", result.report.afterMultipleEdgeCount},
        {"roundtrip_multiple_edges", result.report.roundtripMultipleEdgeCount}};
}

std::array<int, 3> patchColor(const int patch) {
    static constexpr std::array<std::array<int, 3>, 12> colors {{
        {{31, 119, 180}}, {{255, 127, 14}}, {{44, 160, 44}}, {{214, 39, 40}},
        {{148, 103, 189}}, {{140, 86, 75}}, {{227, 119, 194}}, {{127, 127, 127}},
        {{188, 189, 34}}, {{23, 190, 207}}, {{57, 106, 177}}, {{218, 124, 48}}}};
    return colors[static_cast<std::size_t>(patch) % colors.size()];
}

QByteArray layoutPly(
    const spo::RegionReferenceMesh& mesh,
    const spo::RegionPatchLayout& layout) {
    QByteArray output;
    QTextStream stream(&output, QIODevice::WriteOnly);
    stream << "ply\nformat ascii 1.0\n"
           << "element vertex " << mesh.vertices.size() << "\n"
           << "property double x\nproperty double y\nproperty double z\n"
           << "element face " << mesh.triangles.size() << "\n"
           << "property list uchar int vertex_indices\n"
           << "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
    stream.setRealNumberPrecision(17);
    for (const auto& vertex : mesh.vertices) {
        stream << vertex.point.X() << ' ' << vertex.point.Y() << ' ' << vertex.point.Z() << '\n';
    }
    for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
        const auto& triangle = mesh.triangles[index];
        const auto color = patchColor(layout.trianglePatches[index]);
        stream << "3 " << triangle.vertices[0] << ' ' << triangle.vertices[1] << ' '
               << triangle.vertices[2] << ' ' << color[0] << ' ' << color[1] << ' '
               << color[2] << '\n';
    }
    return output;
}

QByteArray layoutSvg(
    const spo::RegionReferenceMesh& mesh,
    const spo::RegionPatchLayout& layout) {
    std::array<double, 3> minimum {
        mesh.vertices.front().point.X(), mesh.vertices.front().point.Y(), mesh.vertices.front().point.Z()};
    auto maximum = minimum;
    for (const auto& vertex : mesh.vertices) {
        const std::array<double, 3> point {vertex.point.X(), vertex.point.Y(), vertex.point.Z()};
        for (int axis = 0; axis < 3; ++axis) {
            minimum[static_cast<std::size_t>(axis)] = std::min(minimum[static_cast<std::size_t>(axis)], point[static_cast<std::size_t>(axis)]);
            maximum[static_cast<std::size_t>(axis)] = std::max(maximum[static_cast<std::size_t>(axis)], point[static_cast<std::size_t>(axis)]);
        }
    }
    std::array<int, 3> axes {0, 1, 2};
    std::sort(axes.begin(), axes.end(), [&](const auto lhs, const auto rhs) {
        return maximum[static_cast<std::size_t>(lhs)] - minimum[static_cast<std::size_t>(lhs)] >
            maximum[static_cast<std::size_t>(rhs)] - minimum[static_cast<std::size_t>(rhs)];
    });
    const auto xAxis = axes[0];
    const auto yAxis = axes[1];
    const auto coordinate = [](const gp_Pnt& point, const int axis) {
        return axis == 0 ? point.X() : (axis == 1 ? point.Y() : point.Z());
    };
    const auto width = std::max(1.0e-12, maximum[static_cast<std::size_t>(xAxis)] - minimum[static_cast<std::size_t>(xAxis)]);
    const auto height = std::max(1.0e-12, maximum[static_cast<std::size_t>(yAxis)] - minimum[static_cast<std::size_t>(yAxis)]);
    const auto scale = std::min(960.0 / width, 960.0 / height);
    const auto project = [&](const gp_Pnt& point) {
        return std::array<double, 2> {
            20.0 + (coordinate(point, xAxis) - minimum[static_cast<std::size_t>(xAxis)]) * scale,
            980.0 - (coordinate(point, yAxis) - minimum[static_cast<std::size_t>(yAxis)]) * scale};
    };
    std::map<std::array<std::size_t, 2>, std::vector<std::size_t>> edgeTriangles;
    QByteArray output;
    QTextStream stream(&output, QIODevice::WriteOnly);
    stream.setRealNumberPrecision(8);
    stream << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"1000\" viewBox=\"0 0 1000 1000\">\n"
           << "<rect width=\"1000\" height=\"1000\" fill=\"white\"/>\n";
    for (std::size_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
        const auto& triangle = mesh.triangles[triangleIndex];
        const auto color = patchColor(layout.trianglePatches[triangleIndex]);
        stream << "<polygon points=\"";
        for (const auto vertex : triangle.vertices) {
            const auto point = project(mesh.vertices[vertex].point);
            stream << point[0] << ',' << point[1] << ' ';
        }
        stream << "\" fill=\"rgb(" << color[0] << ',' << color[1] << ',' << color[2]
               << ")\" fill-opacity=\"0.70\" stroke=\"none\"/>\n";
        for (int edge = 0; edge < 3; ++edge) {
            const auto first = triangle.vertices[static_cast<std::size_t>(edge)];
            const auto second = triangle.vertices[static_cast<std::size_t>((edge + 1) % 3)];
            edgeTriangles[{std::min(first, second), std::max(first, second)}].push_back(triangleIndex);
        }
    }
    for (const auto& [edge, triangles] : edgeTriangles) {
        const bool outer = triangles.size() == 1;
        const bool seam = triangles.size() == 2 &&
            layout.trianglePatches[triangles[0]] != layout.trianglePatches[triangles[1]];
        if (!outer && !seam) continue;
        const auto first = project(mesh.vertices[edge[0]].point);
        const auto second = project(mesh.vertices[edge[1]].point);
        stream << "<line x1=\"" << first[0] << "\" y1=\"" << first[1]
               << "\" x2=\"" << second[0] << "\" y2=\"" << second[1]
               << "\" stroke=\"" << (outer ? "#000000" : "#ffffff")
               << "\" stroke-width=\"" << (outer ? "2.2" : "1.4") << "\"/>\n";
    }
    std::vector<std::array<double, 2>> centers(static_cast<std::size_t>(layout.patchCount));
    std::vector<std::size_t> counts(static_cast<std::size_t>(layout.patchCount));
    for (std::size_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
        const auto patch = static_cast<std::size_t>(layout.trianglePatches[triangleIndex]);
        const auto& triangle = mesh.triangles[triangleIndex];
        gp_Pnt centroid(
            (mesh.vertices[triangle.vertices[0]].point.X() + mesh.vertices[triangle.vertices[1]].point.X() + mesh.vertices[triangle.vertices[2]].point.X()) / 3.0,
            (mesh.vertices[triangle.vertices[0]].point.Y() + mesh.vertices[triangle.vertices[1]].point.Y() + mesh.vertices[triangle.vertices[2]].point.Y()) / 3.0,
            (mesh.vertices[triangle.vertices[0]].point.Z() + mesh.vertices[triangle.vertices[1]].point.Z() + mesh.vertices[triangle.vertices[2]].point.Z()) / 3.0);
        const auto point = project(centroid);
        centers[patch][0] += point[0];
        centers[patch][1] += point[1];
        ++counts[patch];
    }
    for (std::size_t patch = 0; patch < centers.size(); ++patch) {
        if (counts[patch] == 0) continue;
        stream << "<text x=\"" << centers[patch][0] / counts[patch]
               << "\" y=\"" << centers[patch][1] / counts[patch]
               << "\" text-anchor=\"middle\" font-family=\"sans-serif\" font-size=\"18\""
                  " font-weight=\"bold\" fill=\"#111\" stroke=\"white\" stroke-width=\"3\" paint-order=\"stroke\">"
               << patch << "</text>\n";
    }
    stream << "</svg>\n";
    return output;
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
    const ProbeTiming& timing,
    const std::string& runIdentity,
    const std::string& commit) {
    QJsonArray stages;
    for (const auto& stage : result.report.stages) {
        stages.append(stageToJson(stage));
    }
    QJsonArray cacheEvents;
    for (const auto& event : result.report.cacheEvents) {
        cacheEvents.append(QJsonObject {
            {"layer", QString::fromStdString(event.layer)},
            {"hit", event.hit},
            {"negative_entry", event.negativeEntry},
            {"read_ms", event.readMilliseconds},
            {"write_ms", event.writeMilliseconds},
            {"message", QString::fromStdString(event.message)}});
    }
    return {
        {"schema_version", 1},
        {"run_identity", QString::fromStdString(runIdentity)},
        {"input_step_sha256", QString::fromStdString(request.options.inputStepSha256)},
        {"selected_faces", idsToJson(request.selectedFaces)},
        {"commit", QString::fromStdString(commit)},
#ifdef NDEBUG
        {"build_configuration", "Release"},
#else
        {"build_configuration", "Debug"},
#endif
        {"hardware", QJsonObject {
            {"cpu_architecture", QSysInfo::currentCpuArchitecture()},
            {"logical_threads", QThread::idealThreadCount()},
            {"operating_system", QSysInfo::prettyProductName()}}},
        {"algorithm_fingerprint", QString::fromStdString(request.options.algorithmFingerprint)},
        {"status", spo::toString(result.report.status)},
        {"failure_stage", spo::toString(result.report.failureStage)},
        {"layout_mode", spo::toString(request.layoutMode)},
        {"wall_budget_ms", QString::number(timing.requestedWallBudgetMilliseconds)},
        {"engine_wall_budget_ms", QString::number(request.options.maximumWallClockMilliseconds)},
        {"input_read_budget_ms", QString::number(request.options.inputReadBudgetMilliseconds)},
        {"memory_budget_mib", QString::number(request.options.maximumMemoryMiB)},
        {"cache_policy", spo::toString(request.options.cachePolicy)},
        {"cache_key", QString::fromStdString(result.report.cacheKey)},
        {"input_read_ms", timing.inputReadMilliseconds},
        {"candidate_resolution_ms", timing.candidateResolutionMilliseconds},
        {"engine_ms", timing.engineMilliseconds},
        {"core_total_ms", timing.coreTotalMilliseconds},
        {"total_wall_ms", result.report.totalWallMilliseconds},
        {"peak_working_set_bytes", QString::number(result.report.peakWorkingSetBytes)},
        {"peak_private_bytes", QString::number(result.report.peakPrivateBytes)},
        {"full_fit_count", result.report.fullFitCount},
        {"cached_fit_attempt_count", result.report.cachedFitAttemptCount},
        {"sample_vertices", QString::number(result.report.sampleVertexCount)},
        {"sample_triangles", QString::number(result.report.sampleTriangleCount)},
        {"boundary_samples", QString::number(result.report.boundarySampleCount)},
        {"welded_internal_seam_vertices", QString::number(result.report.weldedInternalSeamVertexCount)},
        {"normal_discontinuity_vertices", QString::number(result.report.normalDiscontinuityVertexCount)},
        {"reference_mesh_fingerprint", QString::fromStdString(result.report.referenceMeshFingerprint)},
        {"selected_patch_count", result.report.selectedPatchCount},
        {"selected_layout_hash", QString::fromStdString(result.report.selectedLayoutHash)},
        {"geometry_metrics", QJsonObject {
            {"holdout_maximum_distance", result.fittedNetwork
                ? result.fittedNetwork->holdoutMaximumDistance : 0.0},
            {"holdout_maximum_normal_degrees", result.fittedNetwork
                ? result.fittedNetwork->holdoutMaximumNormalDegrees : 0.0},
            {"boundary_maximum_distance", result.fittedNetwork
                ? result.fittedNetwork->boundaryMaximumDistance : 0.0},
            {"seam_maximum_distance", result.fittedNetwork
                ? result.fittedNetwork->seamMaximumDistance : 0.0},
            {"output_faces", result.report.outputFaceCount},
            {"after_solids", result.report.afterSolidCount},
            {"after_free_edges", result.report.afterFreeEdgeCount},
            {"after_multiple_edges", result.report.afterMultipleEdgeCount},
            {"step_roundtrip_ok", result.report.stepRoundtripOk}}},
        {"remaining_wall_budget_ms", std::max(
            0.0,
            static_cast<double>(request.options.maximumWallClockMilliseconds) -
                result.report.totalWallMilliseconds)},
        {"stop_reason", QString::fromStdString(result.report.message)},
        {"layout", layoutReportToJson(result)},
        {"fit", fitReportToJson(result)},
        {"cache", cacheEvents},
        {"stages", stages}};
}

QJsonObject samplingToJson(const spo::RegionRefitResult& result) {
    return {
        {"status", result.referenceMesh ? "Success" : spo::toString(result.report.status)},
        {"vertices", QString::number(result.report.sampleVertexCount)},
        {"triangles", QString::number(result.report.sampleTriangleCount)},
        {"boundary_samples", QString::number(result.report.boundarySampleCount)},
        {"welded_internal_seam_vertices", QString::number(result.report.weldedInternalSeamVertexCount)},
        {"normal_discontinuity_vertices", QString::number(result.report.normalDiscontinuityVertexCount)},
        {"maximum_interior_chord_error", result.report.maximumSamplingChordError},
        {"maximum_interior_normal_error_degrees", result.report.maximumSamplingNormalErrorDegrees},
        {"maximum_boundary_chord_error", result.report.maximumBoundaryChordError},
        {"mesh_fingerprint", QString::fromStdString(result.report.referenceMeshFingerprint)}};
}

}

int main(int argc, char* argv[]) {
    const auto coreStarted = std::chrono::steady_clock::now();
    if (argc < 5) {
        std::cerr << "用法：native_region_refit_probe <输入.step> <输出目录> "
                     "(--candidate-id <编号> | --faces <编号,编号,...>) "
                     "[--locked-edges <编号,编号,...>] [--layout <adaptive|fixed>] "
                     "[--patch-count <数量>] [--wall-time-ms <毫秒>] "
                     "[--max-memory-mib <MiB>] [--cache-dir <目录>] "
                     "[--cache-policy <disabled|read-write|read-only|refresh>] "
                     "[--run-identity <名称>] [--commit <提交>] "
                     "[--constraints-only | --sampling-only | --layout-only | --fit-only]\n";
        return 2;
    }

    const std::filesystem::path input = argv[1];
    const std::filesystem::path outputDirectory = argv[2];
    int candidateId = -1;
    bool constraintsOnly = false;
    bool samplingOnly = false;
    bool layoutOnly = false;
    bool fitOnly = false;
    bool performanceOnly = false;
    bool cachePolicySpecified = false;
    std::string runIdentity = "single";
    std::string commit;
    spo::RegionRefitRequest request;
    request.options.algorithmFingerprint = "native-regional-refit-v1";
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
                request.options.solverMemoryPreflightMiB = request.options.maximumMemoryMiB > 1024
                    ? request.options.maximumMemoryMiB - 1024
                    : request.options.maximumMemoryMiB;
            } else if (argument == "--cache-dir") {
                if (++index >= argc) return 2;
                request.options.cacheDirectory = std::filesystem::path(argv[index]);
                if (!cachePolicySpecified) {
                    request.options.cachePolicy = spo::RegionRefitCachePolicy::ReadWrite;
                }
            } else if (argument == "--cache-policy") {
                if (++index >= argc) return 2;
                const std::string value = argv[index];
                if (value == "disabled") request.options.cachePolicy = spo::RegionRefitCachePolicy::Disabled;
                else if (value == "read-write") request.options.cachePolicy = spo::RegionRefitCachePolicy::ReadWrite;
                else if (value == "read-only") request.options.cachePolicy = spo::RegionRefitCachePolicy::ReadOnly;
                else if (value == "refresh") request.options.cachePolicy = spo::RegionRefitCachePolicy::Refresh;
                else return 2;
                cachePolicySpecified = true;
            } else if (argument == "--algorithm-fingerprint") {
                if (++index >= argc) return 2;
                request.options.algorithmFingerprint = argv[index];
            } else if (argument == "--run-identity") {
                if (++index >= argc) return 2;
                runIdentity = argv[index];
            } else if (argument == "--commit") {
                if (++index >= argc) return 2;
                commit = argv[index];
            } else if (argument == "--constraints-only") {
                constraintsOnly = true;
            } else if (argument == "--sampling-only") {
                samplingOnly = true;
            } else if (argument == "--layout-only") {
                layoutOnly = true;
            } else if (argument == "--fit-only") {
                fitOnly = true;
            } else if (argument == "--performance-only") {
                performanceOnly = true;
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
    if (static_cast<int>(constraintsOnly) + static_cast<int>(samplingOnly) +
            static_cast<int>(layoutOnly) + static_cast<int>(fitOnly) > 1) {
        std::cerr << "阶段诊断选项不能同时使用。\n";
        return 2;
    }
    if (request.layoutMode == spo::RegionRefitLayoutMode::FixedPatchCount &&
        request.options.fixedPatchCount <= 0) {
        std::cerr << "固定布局必须指定正数 --patch-count。\n";
        return 2;
    }

    ProbeTiming timing;
    timing.requestedWallBudgetMilliseconds = request.options.maximumWallClockMilliseconds;
    request.options.inputStepSha256 = sha256File(input);
    if (request.options.inputStepSha256.empty()) {
        std::cerr << "无法计算输入 STEP 的 SHA-256。\n";
        return 1;
    }
    if (request.options.cachePolicy != spo::RegionRefitCachePolicy::Disabled &&
        request.options.cacheDirectory.empty()) {
        std::cerr << "启用缓存时必须指定 --cache-dir。\n";
        return 2;
    }
    const auto readStarted = std::chrono::steady_clock::now();
    const auto read = spo::StepReader().read(input);
    timing.inputReadMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - readStarted).count();
    if (!read.status.success()) {
        std::cerr << read.status.message() << '\n';
        return 1;
    }
    const auto inputReadBudgetExceeded = timing.inputReadMilliseconds >
        static_cast<double>(request.options.inputReadBudgetMilliseconds);
    if (candidateId >= 0 && !inputReadBudgetExceeded) {
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
    if (constraintsOnly) {
        request.diagnosticStopAfterStage = spo::RegionRefitStage::Constraints;
    } else if (samplingOnly) {
        request.diagnosticStopAfterStage = spo::RegionRefitStage::Sampling;
    } else if (layoutOnly) {
        request.diagnosticStopAfterStage = spo::RegionRefitStage::Layout;
    } else if (fitOnly) {
        request.diagnosticStopAfterStage = spo::RegionRefitStage::Fitting;
    }
    const auto spentBeforeEngine = std::chrono::duration_cast<std::chrono::milliseconds>(
        engineStarted - coreStarted).count();
    spo::RegionRefitResult result;
    if (inputReadBudgetExceeded) {
        result.report.status = spo::RegionRefitStatus::TimeBudgetExceeded;
        result.report.failureStage = spo::RegionRefitStage::Constraints;
        result.report.message = "STEP input read exceeded its wall-clock budget.";
    } else if (spentBeforeEngine >= timing.requestedWallBudgetMilliseconds) {
        result.report.status = spo::RegionRefitStatus::TimeBudgetExceeded;
        result.report.failureStage = spo::RegionRefitStage::Constraints;
        result.report.message = "STEP read and candidate resolution exhausted the total wall-clock budget.";
    } else {
        request.options.maximumWallClockMilliseconds =
            timing.requestedWallBudgetMilliseconds - spentBeforeEngine;
        result = spo::RegionalSurfaceRefitEngine().run(read.document, request);
    }
    timing.engineMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - engineStarted).count();
    timing.coreTotalMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - coreStarted).count();
    const auto diagnostics = outputDirectory / "diagnostics";
    const auto performanceName = runIdentity == "single"
        ? std::filesystem::path("performance_report.json")
        : std::filesystem::path("performance_" + runIdentity + ".json");
    const auto performanceWritten = writeJson(
        diagnostics / performanceName,
        performanceToJson(result, request, timing, runIdentity, commit));
    const auto diagnosticsWritten = performanceOnly || (
        writeJson(diagnostics / "constraints.json", constraintsToJson(result)) &&
        writeJson(diagnostics / "sampling_report.json", samplingToJson(result)) &&
        writeJson(diagnostics / "layout_report.json", layoutReportToJson(result)) &&
        writeJson(diagnostics / "fit_report.json", fitReportToJson(result)) &&
        writeJson(diagnostics / "retrim_report.json", retrimReportToJson(result)) &&
        writeJson(diagnostics / "sewing_report.json", sewingReportToJson(result)) &&
        writeJson(diagnostics / "strict_topology_report.json", strictTopologyReportToJson(result)));
    if (!performanceWritten || !diagnosticsWritten) {
        std::cerr << "无法写入区域重拟合诊断。\n";
        return 1;
    }
    if (!performanceOnly && result.referenceMesh) {
        const auto samplingDirectory = outputDirectory / "artifacts" / "sampling";
        std::filesystem::create_directories(samplingDirectory);
        const auto write = spo::StlWriter().write(
            result.referenceMesh->toStlMesh(),
            samplingDirectory / "adaptive_reference.stl");
        if (!write.success) {
            std::cerr << write.message << '\n';
            return 1;
        }
    }
    if (!performanceOnly && result.referenceMesh && result.selectedLayout) {
        const auto layoutDirectory = outputDirectory / "artifacts" / "layout";
        if (!writeJson(layoutDirectory / "selected_layout.json", selectedLayoutToJson(*result.selectedLayout)) ||
            !writeText(layoutDirectory / "selected_layout.ply", layoutPly(*result.referenceMesh, *result.selectedLayout)) ||
            !writeText(layoutDirectory / "selected_layout.svg", layoutSvg(*result.referenceMesh, *result.selectedLayout))) {
            std::cerr << "无法写入布局证据。\n";
            return 1;
        }
    }
    if (!performanceOnly && result.fittedNetwork && !result.fittedPatchNetwork.IsNull()) {
        const auto fitDirectory = outputDirectory / "artifacts" / "fit";
        std::filesystem::create_directories(fitDirectory);
        const auto write = spo::StepWriter().write(
            spo::ShapeDocument(result.fittedPatchNetwork, {}),
            fitDirectory / "fitted_patch_network.step");
        if (!write.success()) {
            std::cerr << write.message() << '\n';
            return 1;
        }
    }
    if (!performanceOnly && result.canApply()) {
        const auto assemblyDirectory = outputDirectory / "artifacts" / "assembly";
        std::filesystem::create_directories(assemblyDirectory);
        const auto write = spo::StepWriter().write(
            spo::ShapeDocument(result.finalShape, {}),
            assemblyDirectory / "final_sewn.step");
        if (!write.success()) {
            std::cerr << write.message() << '\n';
            return 1;
        }
    }

    std::cout << "status=" << spo::toString(result.report.status) << '\n'
              << "failure_stage=" << spo::toString(result.report.failureStage) << '\n'
              << "selected_faces=" << result.constraints.selectedFaces.size() << '\n'
              << "boundary_edges=" << result.constraints.boundaryEdges.size() << '\n'
              << "internal_edges=" << result.constraints.internalEdges.size() << '\n'
              << "sample_vertices=" << result.report.sampleVertexCount << '\n'
              << "sample_triangles=" << result.report.sampleTriangleCount << '\n'
              << "reference_mesh_fingerprint=" << result.report.referenceMeshFingerprint << '\n'
              << "selected_patch_count=" << result.report.selectedPatchCount << '\n'
              << "selected_layout_hash=" << result.report.selectedLayoutHash << '\n'
              << "total_ms=" << result.report.totalWallMilliseconds << '\n'
              << "message=" << result.report.message << '\n';

    if (constraintsOnly) {
        return !result.constraints.boundaryEdges.empty() &&
            !result.report.stages.empty() && result.report.stages.front().success ? 0 : 1;
    }
    if (samplingOnly) {
        return result.referenceMesh && !result.referenceMesh->empty() ? 0 : 1;
    }
    if (layoutOnly) {
        return result.selectedLayout && !result.selectedLayout->trianglePatches.empty() ? 0 : 1;
    }
    if (fitOnly) {
        return result.fittedNetwork && !result.fittedPatchNetwork.IsNull() ? 0 : 1;
    }
    return result.canApply() ? 0 : 1;
}
