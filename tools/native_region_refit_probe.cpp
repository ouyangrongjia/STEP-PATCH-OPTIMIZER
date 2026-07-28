#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "io/StlWriter.h"
#include "merge/MergePlanner.h"
#include "reduction/region_refit/JointBSplineNetworkFitter.h"
#include "reduction/region_refit/RegionCurveNetwork.h"
#include "reduction/region_refit/RegionPatchLayout.h"
#include "reduction/region_refit/RegionReferenceMesh.h"
#include "reduction/region_refit/RegionalSurfaceRefitEngine.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <QSysInfo>
#include <QTextStream>
#include <QThread>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>

#include <algorithm>
#include <array>
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

struct StepFileComplexity {
    bool present = false;
    std::uint64_t bytes = 0;
    std::uint64_t entityCount = 0;
    std::uint64_t edgeCurveCount = 0;
    std::uint64_t surfaceCount = 0;
    std::string sha256;
};

StepFileComplexity measureStepFile(const std::filesystem::path& path) {
    StepFileComplexity result;
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly)) return result;
    result.present = true;
    result.bytes = static_cast<std::uint64_t>(file.size());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const auto line = file.readLine();
        hash.addData(line);
        const auto trimmed = line.trimmed();
        if (trimmed.startsWith('#') && trimmed.contains('=')) {
            ++result.entityCount;
            if (trimmed.contains("=EDGE_CURVE(")) ++result.edgeCurveCount;
            static const std::array<QByteArray, 10> surfaceTokens {
                "=PLANE(", "=CYLINDRICAL_SURFACE(", "=CONICAL_SURFACE(",
                "=SPHERICAL_SURFACE(", "=TOROIDAL_SURFACE(",
                "=B_SPLINE_SURFACE_WITH_KNOTS(", "=BEZIER_SURFACE(",
                "=SURFACE_OF_REVOLUTION(", "=SURFACE_OF_LINEAR_EXTRUSION(",
                "=OFFSET_SURFACE("};
            if (std::any_of(surfaceTokens.begin(), surfaceTokens.end(), [&](const auto& token) {
                    return trimmed.contains(token);
                })) {
                ++result.surfaceCount;
            }
        }
    }
    result.sha256 = hash.result().toHex().toStdString();
    return result;
}

QJsonObject outputComplexityToJson(
    const StepFileComplexity& baseline,
    const StepFileComplexity& candidate,
    const spo::RegionRefitResult& result,
    const int maximumControlPointsPerSide) {
    const auto byteIncrease = baseline.bytes > 0 && candidate.present
        ? (static_cast<double>(candidate.bytes) - static_cast<double>(baseline.bytes)) /
            static_cast<double>(baseline.bytes)
        : 0.0;
    const auto entityIncrease = baseline.entityCount > 0 && candidate.present
        ? (static_cast<double>(candidate.entityCount) - static_cast<double>(baseline.entityCount)) /
            static_cast<double>(baseline.entityCount)
        : 0.0;
    auto observedMaximumControlSide = 0;
    for (const auto& attempt : result.report.fitAttempts) {
        for (const auto& iteration : attempt.iterations) {
            observedMaximumControlSide = std::max({
                observedMaximumControlSide,
                iteration.uControlCount,
                iteration.vControlCount});
        }
    }
    const auto replacementFaceCountMatches = result.report.selectedPatchCount > 0 &&
        result.report.retrimReplacementFaceCount == result.report.selectedPatchCount;
    const auto duplicateTopology = std::any_of(
        result.report.edgeConsistency.begin(), result.report.edgeConsistency.end(), [](const auto& edge) {
            return edge.multipleEdge || edge.actualFaceUseCount > edge.expectedFaceUseCount;
        });
    const auto edgeEntityIncrease = candidate.edgeCurveCount > baseline.edgeCurveCount
        ? candidate.edgeCurveCount - baseline.edgeCurveCount
        : 0;
    const auto surfaceEntityIncrease = candidate.surfaceCount > baseline.surfaceCount
        ? candidate.surfaceCount - baseline.surfaceCount
        : 0;
    const auto unexpectedEntityDuplication = edgeEntityIncrease >
            static_cast<std::uint64_t>(std::max(0, result.report.selectedPatchCount)) * 8ull ||
        surfaceEntityIncrease >
            static_cast<std::uint64_t>(std::max(0, result.report.selectedPatchCount));
    const auto passed = baseline.present && candidate.present &&
        byteIncrease <= 0.10 && entityIncrease <= 0.10 &&
        replacementFaceCountMatches && !duplicateTopology && !unexpectedEntityDuplication &&
        observedMaximumControlSide <= maximumControlPointsPerSide;
    return {
        {"schema_version", 1},
        {"baseline_role", "NoModificationStepRoundtripWithSameWriter"},
        {"baseline_temporary_file_deleted", true},
        {"baseline", QJsonObject {
            {"present", baseline.present},
            {"bytes", QString::number(baseline.bytes)},
            {"step_entity_count", QString::number(baseline.entityCount)},
            {"edge_curve_count", QString::number(baseline.edgeCurveCount)},
            {"surface_count", QString::number(baseline.surfaceCount)},
            {"sha256", QString::fromStdString(baseline.sha256)}}},
        {"candidate", QJsonObject {
            {"present", candidate.present},
            {"bytes", QString::number(candidate.bytes)},
            {"step_entity_count", QString::number(candidate.entityCount)},
            {"edge_curve_count", QString::number(candidate.edgeCurveCount)},
            {"surface_count", QString::number(candidate.surfaceCount)},
            {"sha256", QString::fromStdString(candidate.sha256)}}},
        {"byte_increase_ratio", byteIncrease},
        {"entity_increase_ratio", entityIncrease},
        {"replacement_face_count", result.report.retrimReplacementFaceCount},
        {"actual_patch_count", result.report.selectedPatchCount},
        {"replacement_face_count_matches", replacementFaceCountMatches},
        {"edge_curve_count_increase", QString::number(edgeEntityIncrease)},
        {"surface_count_increase", QString::number(surfaceEntityIncrease)},
        {"unexpected_duplicate_edges_or_surfaces", duplicateTopology || unexpectedEntityDuplication},
        {"observed_maximum_control_points_per_side", observedMaximumControlSide},
        {"maximum_control_points_per_side", maximumControlPointsPerSide},
        {"passed", passed}};
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

QJsonArray pointToJson(const gp_Pnt& point) {
    return {point.X(), point.Y(), point.Z()};
}

QJsonObject curveNetworkToJson(
    const spo::RegionRefitResult& result,
    const int sampleCount) {
    QJsonArray nodes;
    QJsonArray edges;
    QJsonArray loops;
    if (result.selectedCurveNetwork) {
        const auto& network = *result.selectedCurveNetwork;
        for (const auto& node : network.nodes) {
            QJsonArray incidentEdges;
            for (const auto edge : node.incidentEdges) {
                incidentEdges.append(QString::number(edge));
            }
            nodes.append(QJsonObject {
                {"id", QString::number(node.id)},
                {"mesh_vertex", QString::number(node.meshVertex)},
                {"point", pointToJson(node.point)},
                {"original_boundary", node.originalBoundary},
                {"incident_edges", incidentEdges}});
        }
        for (const auto& edge : network.edges) {
            QJsonArray curveSamples;
            const auto count = std::max(2, sampleCount);
            for (int sample = 0; sample < count; ++sample) {
                const auto alpha = static_cast<double>(sample) /
                    static_cast<double>(count - 1);
                const auto parameter = edge.firstParameter +
                    alpha * (edge.lastParameter - edge.firstParameter);
                curveSamples.append(QJsonObject {
                    {"t", parameter},
                    {"point", pointToJson(edge.curve->Value(parameter))}});
            }
            QJsonArray meshVertices;
            for (const auto vertex : edge.meshVertices) {
                meshVertices.append(QString::number(vertex));
            }
            QJsonArray meshParameters;
            for (const auto parameter : edge.meshParameters) {
                meshParameters.append(parameter);
            }
            edges.append(QJsonObject {
                {"id", QString::number(edge.id)},
                {"start_node", QString::number(edge.startNode)},
                {"end_node", QString::number(edge.endNode)},
                {"first_patch", edge.firstPatch},
                {"second_patch", edge.secondPatch},
                {"requested_continuity",
                    spo::toString(edge.requestedContinuity)},
                {"continuity", spo::toString(edge.continuity)},
                {"construction_mode",
                    spo::toString(edge.constructionMode)},
                {"product_quality_passed",
                    edge.productQualityPassed},
                {"validation_sample_count",
                    edge.validationSampleCount},
                {"control_point_count", edge.controlPointCount},
                {"maximum_sample_turn_degrees",
                    edge.maximumSampleTurnDegrees},
                {"bending_energy", edge.bendingEnergy},
                {"self_intersection_count",
                    edge.selfIntersectionCount},
                {"original_boundary", edge.originalBoundary},
                {"has_original_edge", edge.hasOriginalEdge},
                {"original_edge_id", edge.hasOriginalEdge
                    ? QString::number(edge.originalEdgeId) : QString()},
                {"first_parameter", edge.firstParameter},
                {"last_parameter", edge.lastParameter},
                {"mesh_vertices", meshVertices},
                {"mesh_parameters", meshParameters},
                {"reuses_original_edge", edge.reusesOriginalEdge},
                {"splits_original_edge", edge.splitsOriginalEdge},
                {"frozen", edge.frozen},
                {"maximum_projection_error", edge.maximumProjectionError},
                {"samples", curveSamples}});
        }
        for (const auto& loop : network.patchLoops) {
            QJsonArray uses;
            for (const auto& use : loop.edges) {
                uses.append(QJsonObject {
                    {"edge", QString::number(use.edgeIndex)},
                    {"forward", use.forward}});
            }
            loops.append(QJsonObject {
                {"patch", loop.patch},
                {"closed", network.report.closurePassed},
                {"edges", uses}});
        }
    }
    return {
        {"schema_version", 1},
        {"status", result.selectedCurveNetwork &&
                result.selectedCurveNetwork->valid()
            ? "Success" : spo::toString(result.report.status)},
        {"hash", QString::fromStdString(result.report.selectedCurveNetworkHash)},
        {"node_count", QString::number(result.report.curveNetworkNodeCount)},
        {"edge_count", QString::number(result.report.curveNetworkEdgeCount)},
        {"internal_edge_count", QString::number(result.report.curveNetworkInternalEdgeCount)},
        {"boundary_edge_count", QString::number(result.report.curveNetworkBoundaryEdgeCount)},
        {"g0_edge_count", QString::number(result.report.curveNetworkG0EdgeCount)},
        {"g1_edge_count", QString::number(result.report.curveNetworkG1EdgeCount)},
        {"g2_edge_count", QString::number(result.report.curveNetworkG2EdgeCount)},
        {"regularized_cubic_edge_count",
            QString::number(
                result.report.curveNetworkRegularizedCubicEdgeCount)},
        {"diagnostic_polyline_edge_count",
            QString::number(
                result.report.curveNetworkDiagnosticPolylineEdgeCount)},
        {"continuity_downgrade_count",
            QString::number(
                result.report.curveNetworkContinuityDowngradeCount)},
        {"maximum_projection_error", result.report.curveNetworkMaximumProjectionError},
        {"maximum_sample_turn_degrees",
            result.report.curveNetworkMaximumSampleTurnDegrees},
        {"maximum_bending_energy",
            result.report.curveNetworkMaximumBendingEnergy},
        {"self_intersection_count",
            result.report.curveNetworkSelfIntersectionCount},
        {"unexpected_curve_intersection_count",
            result.report.curveNetworkUnexpectedIntersectionCount},
        {"closure_passed", result.report.curveNetworkClosurePassed},
        {"ownership_passed", result.report.curveNetworkOwnershipPassed},
        {"projection_passed", result.report.curveNetworkProjectionPassed},
        {"orientation_passed",
            result.report.curveNetworkOrientationPassed},
        {"intersection_passed",
            result.report.curveNetworkIntersectionPassed},
        {"product_quality_passed",
            result.report.curveNetworkProductQualityPassed},
        {"frozen", result.report.curveNetworkFrozen},
        {"nodes", nodes},
        {"edges", edges},
        {"patch_loops", loops}};
}

QByteArray curveNetworkPly(
    const spo::RegionCurveNetwork& network,
    const int sampleCount) {
    struct ColoredPoint {
        gp_Pnt point;
        std::array<int, 3> color;
    };
    struct ColoredLine {
        std::size_t first = 0;
        std::size_t second = 0;
        std::array<int, 3> color;
    };
    std::vector<ColoredPoint> points;
    std::vector<ColoredLine> lines;
    points.reserve(network.nodes.size() +
        network.edges.size() * static_cast<std::size_t>(sampleCount));
    for (const auto& node : network.nodes) {
        points.push_back({node.point, {220, 30, 30}});
    }
    const auto count = std::max(2, sampleCount);
    for (const auto& edge : network.edges) {
        const auto color = edge.originalBoundary
            ? std::array<int, 3> {20, 20, 20}
            : std::array<int, 3> {0, 130, 210};
        const auto begin = points.size();
        for (int sample = 0; sample < count; ++sample) {
            const auto alpha = static_cast<double>(sample) /
                static_cast<double>(count - 1);
            const auto parameter = edge.firstParameter +
                alpha * (edge.lastParameter - edge.firstParameter);
            points.push_back({edge.curve->Value(parameter), color});
            if (sample > 0) {
                lines.push_back({
                    begin + static_cast<std::size_t>(sample - 1),
                    begin + static_cast<std::size_t>(sample),
                    color});
            }
        }
    }
    QByteArray output;
    QTextStream stream(&output, QIODevice::WriteOnly);
    stream << "ply\nformat ascii 1.0\n"
           << "element vertex " << points.size() << "\n"
           << "property double x\nproperty double y\nproperty double z\n"
           << "property uchar red\nproperty uchar green\nproperty uchar blue\n"
           << "element edge " << lines.size() << "\n"
           << "property int vertex1\nproperty int vertex2\n"
           << "property uchar red\nproperty uchar green\nproperty uchar blue\n"
           << "end_header\n";
    stream.setRealNumberPrecision(17);
    for (const auto& point : points) {
        stream << point.point.X() << ' ' << point.point.Y() << ' '
               << point.point.Z() << ' ' << point.color[0] << ' '
               << point.color[1] << ' ' << point.color[2] << '\n';
    }
    for (const auto& line : lines) {
        stream << line.first << ' ' << line.second << ' '
               << line.color[0] << ' ' << line.color[1] << ' '
               << line.color[2] << '\n';
    }
    return output;
}

TopoDS_Shape curveNetworkShape(const spo::RegionCurveNetwork& network) {
    try {
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        for (const auto& node : network.nodes) {
            builder.Add(
                compound,
                BRepBuilderAPI_MakeVertex(node.point).Vertex());
        }
        for (const auto& edge : network.edges) {
            if (edge.hasOriginalEdge &&
                edge.reusesOriginalEdge &&
                !edge.originalEdge.IsNull()) {
                builder.Add(compound, edge.originalEdge);
                continue;
            }
            if (edge.curve.IsNull() ||
                !(edge.lastParameter > edge.firstParameter)) {
                return {};
            }
            auto curveEdge = BRepBuilderAPI_MakeEdge(
                edge.curve,
                edge.firstParameter,
                edge.lastParameter);
            if (!curveEdge.IsDone()) return {};
            builder.Add(compound, curveEdge.Edge());
        }
        return compound;
    } catch (const Standard_Failure&) {
        return {};
    }
}

QJsonObject layoutReportToJson(const spo::RegionRefitResult& result) {
    QJsonArray candidates;
    for (const auto& candidate : result.report.layoutCandidates) {
        candidates.append(QJsonObject {
            {"patch_count", candidate.patchCount},
            {"topology_passed", candidate.topologyPassed},
            {"parameterization_passed", candidate.parameterizationPassed},
            {"constructible_parameterization", candidate.constructibleParameterization},
            {"coarse_fit_passed", candidate.coarseFitPassed},
            {"selected_for_full_fit", candidate.selectedForFullFit},
            {"proxy_error", candidate.proxyError},
            {"maximum_distortion", candidate.maximumDistortion},
            {"flipped_triangles", candidate.flippedTriangleCount},
            {"pcurve_self_intersections", candidate.pcurveSelfIntersectionCount},
            {"pcurve_nonadjacent_intersections",
                candidate.pcurveNonAdjacentIntersectionCount},
            {"domain_type", QString::fromStdString(candidate.domainType)},
            {"boundary_chain_count", QString::number(candidate.boundaryChainCount)},
            {"trim_corner_count", candidate.trimCornerCount},
            {"minimum_signed_uv_area", candidate.minimumSignedUvArea},
            {"minimum_to_median_uv_area_ratio",
                candidate.minimumToMedianUvAreaRatio},
            {"boundary_representability_residual",
                candidate.boundaryRepresentabilityResidual},
            {"matrix_rows", QString::number(candidate.matrixRows)},
            {"matrix_nonzeros", QString::number(candidate.matrixNonZeros)},
            {"estimated_solver_bytes", QString::number(candidate.estimatedSolverBytes)},
            {"estimated_output_entities", QString::number(candidate.estimatedOutputEntities)},
            {"wall_ms", candidate.wallMilliseconds},
            {"layout_hash", QString::fromStdString(candidate.layoutHash)},
            {"rejection_reason", QString::fromStdString(candidate.rejectionReason)}});
    }
    return {
        {"status", result.selectedLayout ? "Success" : spo::toString(result.report.status)},
        {"requested_patch_count", result.report.requestedPatchCount},
        {"selected_patch_count", result.report.selectedPatchCount},
        {"actual_patch_count", result.report.selectedPatchCount},
        {"patch_count_deviation", result.report.patchCountDeviation},
        {"exact_target_reached", result.report.exactTargetReached},
        {"minimum_qualified_proven", result.report.minimumQualifiedProven},
        {"selection_outcome", QString::fromStdString(result.report.selectionOutcome)},
        {"unattainable_reason", QString::fromStdString(result.report.unattainableReason)},
        {"selected_curve_network_hash",
            QString::fromStdString(result.report.selectedCurveNetworkHash)},
        {"curve_network_gate_passed", result.selectedCurveNetwork &&
            result.selectedCurveNetwork->valid()},
        {"selected_layout_hash", QString::fromStdString(result.report.selectedLayoutHash)},
        {"diagnostic_candidate_patch_count", result.report.diagnosticCandidatePatchCount},
        {"diagnostic_selection_reason", QString::fromStdString(result.report.diagnosticSelectionReason)},
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
        QJsonArray edgePcurves;
        for (const auto& pcurve : patch.edgePcurves) {
            QJsonArray samples;
            for (std::size_t sample = 0;
                 sample < pcurve.parameters.size(); ++sample) {
                samples.append(QJsonObject {
                    {"t", pcurve.parameters[sample]},
                    {"uv", QJsonArray {
                        pcurve.uvPoints[sample][0],
                        pcurve.uvPoints[sample][1]}}});
            }
            edgePcurves.append(QJsonObject {
                {"edge_index", QString::number(pcurve.edgeIndex)},
                {"forward", pcurve.forward},
                {"first_parameter", pcurve.firstParameter},
                {"last_parameter", pcurve.lastParameter},
                {"start_node", QString::number(pcurve.startNode)},
                {"end_node", QString::number(pcurve.endNode)},
                {"loop_order", QString::number(pcurve.loopOrder)},
                {"iso_side", static_cast<int>(pcurve.isoSide)},
                {"side_first_parameter", pcurve.sideFirstParameter},
                {"side_last_parameter", pcurve.sideLastParameter},
                {"hash", QString::fromStdString(pcurve.hash)},
                {"samples", samples}});
        }
        QJsonArray logicalSides;
        for (const auto& side : patch.logicalSides) {
            QJsonArray orders;
            for (const auto order : side.edgeUseOrders) {
                orders.append(QString::number(order));
            }
            logicalSides.append(QJsonObject {
                {"iso_side", static_cast<int>(side.isoSide)},
                {"curve_length", side.curveLength},
                {"edge_use_orders", orders}});
        }
        QJsonArray boundaryChains;
        for (const auto& chain : patch.boundaryChains) {
            QJsonArray vertices;
            for (const auto vertex : chain.vertices) vertices.append(QString::number(vertex));
            QJsonArray edgeIds;
            for (const auto edgeId : chain.originalEdgeIds) edgeIds.append(QString::number(edgeId));
            boundaryChains.append(QJsonObject {
                {"adjacent_patch", chain.adjacentPatch},
                {"original_boundary", chain.originalBoundary},
                {"vertices", vertices},
                {"original_edge_ids", edgeIds}});
        }
        QJsonArray trimCorners;
        for (const auto corner : patch.trimCorners) trimCorners.append(QString::number(corner));
        patches.append(QJsonObject {
            {"patch", static_cast<int>(patchIndex)},
            {"triangles", QString::number(patch.triangleIndices.size())},
            {"vertices", QString::number(patch.vertexIndices.size())},
            {"boundary_loop", boundary},
            {"edge_pcurves", edgePcurves},
            {"logical_sides", logicalSides},
            {"boundary_chains", boundaryChains},
            {"boundary_chain_count", QString::number(patch.boundaryChainCount)},
            {"trim_corners", trimCorners},
            {"trim_corner_count", patch.trimCornerCount},
            {"domain_type", QString::fromStdString(patch.domainType)},
            {"rectangular_iso_boundary", patch.rectangularIsoBoundary},
            {"disk_topology_passed", patch.diskTopologyPassed},
            {"pcurve_loop_closed", patch.pcurveLoopClosed},
            {"pcurve_orientation_consistent",
                patch.pcurveOrientationConsistent},
            {"pcurve_self_intersection_count",
                patch.pcurveSelfIntersectionCount},
            {"pcurve_nonadjacent_intersection_count",
                patch.pcurveNonAdjacentIntersectionCount},
            {"matrix_rows", QString::number(patch.matrixRows)},
            {"matrix_nonzeros", QString::number(patch.matrixNonZeros)},
            {"flipped_triangles", patch.flippedTriangleCount},
            {"minimum_signed_uv_area", patch.minimumSignedUvArea},
            {"minimum_to_median_uv_area_ratio",
                patch.minimumToMedianUvAreaRatio},
            {"boundary_representability_residual",
                patch.boundaryRepresentabilityResidual},
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
        {"schema_version", 2},
        {"patch_count", layout.patchCount},
        {"layout_hash", QString::fromStdString(layout.hash)},
        {"proxy_error", layout.proxyError},
        {"estimated_solver_bytes", QString::number(layout.estimatedSolverBytes)},
        {"estimated_output_entities", QString::number(layout.estimatedOutputEntities)},
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
                {"boundary_hard_constraint_count", QString::number(iteration.boundaryHardConstraintCount)},
                {"seam_g0_constraint_count", QString::number(iteration.seamG0ConstraintCount)},
                {"seam_g1_soft_constraint_count", QString::number(iteration.seamG1SoftConstraintCount)},
                {"seam_g2_soft_constraint_count", QString::number(iteration.seamG2SoftConstraintCount)},
                {"adjacent_normal_soft_constraint_count", QString::number(iteration.adjacentNormalSoftConstraintCount)},
                {"constraint_rows_before", QString::number(iteration.constraintRowsBefore)},
                {"constraint_rows_after", QString::number(iteration.constraintRowsAfter)},
                {"constraint_estimated_rank", QString::number(iteration.constraintEstimatedRank)},
                {"nonzero_boundary_rhs_count", QString::number(iteration.nonZeroBoundaryRhsCount)},
                {"boundary_target_minimum", QJsonArray {
                    iteration.boundaryTargetMinimum[0],
                    iteration.boundaryTargetMinimum[1],
                    iteration.boundaryTargetMinimum[2]}},
                {"boundary_target_maximum", QJsonArray {
                    iteration.boundaryTargetMaximum[0],
                    iteration.boundaryTargetMaximum[1],
                    iteration.boundaryTargetMaximum[2]}},
                {"maximum_hard_constraint_residual", iteration.maximumHardConstraintResidual},
                {"minimum_full_hard_constraint_residual", iteration.minimumFullHardConstraintResidual},
                {"boundary_feasibility_refinement_rounds",
                    iteration.boundaryFeasibilityRefinementRounds},
                {"boundary_feasibility_residual",
                    iteration.boundaryFeasibilityResidual},
                {"hard_constraint_feasible", iteration.hardConstraintFeasible},
                {"hard_constraint_status", QString::fromStdString(iteration.hardConstraintStatus)},
                {"initial_control_prior_scale", iteration.initialControlPriorScale},
                {"constraint_correction_anchor_scale", iteration.constraintCorrectionAnchorScale},
                {"affine_baseline_training_maximum_distance", iteration.affineBaselineTrainingMaximumDistance},
                {"affine_baseline_boundary_maximum_distance", iteration.affineBaselineBoundaryMaximumDistance},
                {"initial_support_blend_alpha", iteration.initialSupportBlendAlpha},
                {"constraint_correction_blend_alpha", iteration.constraintCorrectionBlendAlpha},
                {"initial_training_maximum_distance", iteration.initialTrainingMaximumDistance},
                {"initial_boundary_maximum_distance", iteration.initialBoundaryMaximumDistance},
                {"corrected_training_maximum_distance", iteration.correctedTrainingMaximumDistance},
                {"corrected_boundary_maximum_distance", iteration.correctedBoundaryMaximumDistance},
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
                {"seam_maximum_curve_deviation", iteration.seamMaximumCurveDeviation},
                {"seam_maximum_normal_degrees", iteration.seamMaximumNormalDegrees},
                {"seam_maximum_curvature_difference",
                    iteration.seamMaximumCurvatureDifference},
                {"seam_continuity_gate_passed", iteration.seamContinuityGatePassed},
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
        {"status", result.report.fitGatePassed ? "Success" : spo::toString(result.report.status)},
        {"layout_gate_passed", result.report.layoutGatePassed},
        {"fit_gate_passed", result.report.fitGatePassed},
        {"boundary_feasibility_refinement_rounds",
            result.report.boundaryFeasibilityRefinementRounds},
        {"boundary_feasibility_residual",
            result.report.boundaryFeasibilityResidual},
        {"maximum_u_control_count",
            result.report.fittedMaximumUControlCount},
        {"maximum_v_control_count",
            result.report.fittedMaximumVControlCount},
        {"best_effort_used", result.report.bestEffortUsed},
        {"force_diagnostic_sewing_requested", result.report.forceDiagnosticSewingRequested},
        {"force_diagnostic_sewing_attempted", result.report.forceDiagnosticSewingAttempted},
        {"sewable_geometry_available", result.report.sewableGeometryAvailable},
        {"product_validated", result.report.productValidated},
        {"first_product_failure_gate", QString::fromStdString(result.report.firstProductFailureGate)},
        {"full_fit_count", result.report.fullFitCount},
        {"cached_fit_attempt_count", result.report.cachedFitAttemptCount},
        {"selected_patch_count", result.report.selectedPatchCount},
        {"selected_curve_network_hash",
            QString::fromStdString(result.report.selectedCurveNetworkHash)},
        {"selected_layout_hash", QString::fromStdString(result.report.selectedLayoutHash)},
        {"attempts", attempts}};
}

QJsonObject retrimReportToJson(const spo::RegionRefitResult& result) {
    return {
        {"status", result.report.retrimValidationPassed
            ? "Success"
            : spo::toString(result.report.status)},
        {"attempted", result.report.retrimAttempted},
        {"validation_passed", result.report.retrimValidationPassed},
        {"replacement_faces", result.report.retrimReplacementFaceCount},
        {"internal_seams", result.report.retrimInternalSeamCount},
        {"reused_original_boundary_edges", result.report.retrimReusedBoundaryEdgeCount},
        {"split_boundary_segments", result.report.retrimSplitBoundarySegmentCount},
        {"pcurve_constructions",
            result.report.retrimStoredPcurveAttachmentCount},
        {"stored_pcurve_attachment_count",
            result.report.retrimStoredPcurveAttachmentCount},
        {"diagnostic_parameter_pcurve_fallback_count",
            result.report.retrimDiagnosticPcurveFallbackCount},
        {"same_parameter_checks", result.report.retrimSameParameterCheckCount},
        {"same_parameter_failures", result.report.retrimSameParameterFailureCount},
        {"maximum_same_parameter_deviation", result.report.retrimMaximumSameParameterDeviation},
        {"unexpected_face_intersection_count",
            result.report.unexpectedFaceIntersectionCount},
        {"unexpected_face_intersection_total_length",
            result.report.unexpectedFaceIntersectionTotalLength},
        {"replacement_quilt_internal_free_edge_count",
            result.report.replacementQuiltInternalFreeEdgeCount},
        {"assembly_passed", result.report.assemblyPassed}};
}

QJsonObject edgeConsistencyToJson(
    const spo::RegionRefitResult& result,
    const double curveOnSurfaceTolerance) {
    QJsonArray edges;
    for (const auto& edge : result.report.edgeConsistency) {
        QJsonArray deflections;
        for (const auto value : edge.triangulationDeflections) deflections.append(value);
        QJsonArray separations;
        for (const auto value : edge.triangulationPolylineSeparations) separations.append(value);
        edges.append(QJsonObject {
            {"edge_index", edge.edgeIndex},
            {"internal_seam", edge.internalSeam},
            {"shared_same_edge", edge.sharedSameEdge},
            {"expected_face_use_count", edge.expectedFaceUseCount},
            {"actual_face_use_count", edge.actualFaceUseCount},
            {"free_edge", edge.freeEdge},
            {"multiple_edge", edge.multipleEdge},
            {"same_range", edge.sameRange},
            {"same_parameter", edge.sameParameter},
            {"curve_on_surface_maximum_deviation", edge.curveOnSurfaceMaximumDeviation},
            {"curve_on_surface_p95_deviation", edge.curveOnSurfaceP95Deviation},
            {"curve_on_surface_rms_deviation", edge.curveOnSurfaceRmsDeviation},
            {"adjacent_surface_maximum_separation", edge.adjacentSurfaceMaximumSeparation},
            {"adjacent_surface_maximum_normal_degrees", edge.adjacentSurfaceMaximumNormalDegrees},
            {"worst_parameter", edge.worstParameter},
            {"triangulation_deflections", deflections},
            {"triangulation_polyline_separations", separations},
            {"classification", QString::fromStdString(edge.classification)}});
    }
    return {
        {"schema_version", 1},
        {"curve_on_surface_tolerance", curveOnSurfaceTolerance},
        {"edges", edges}};
}

QJsonObject triangulationToJson(const spo::RegionRefitResult& result) {
    QJsonArray edges;
    for (const auto& edge : result.report.edgeConsistency) {
        QJsonArray samples;
        const auto count = std::min(
            edge.triangulationDeflections.size(), edge.triangulationPolylineSeparations.size());
        for (std::size_t index = 0; index < count; ++index) {
            samples.append(QJsonObject {
                {"deflection", edge.triangulationDeflections[index]},
                {"polyline_separation", edge.triangulationPolylineSeparations[index]}});
        }
        edges.append(QJsonObject {
            {"edge_index", edge.edgeIndex},
            {"classification", QString::fromStdString(edge.classification)},
            {"samples", samples}});
    }
    return {{"schema_version", 1}, {"edges", edges}};
}

QJsonObject sewingReportToJson(const spo::RegionRefitResult& result) {
    return {
        {"status", result.report.repairPassed ? "Success" : spo::toString(result.report.status)},
        {"attempted", result.report.sewingAttempted},
        {"force_diagnostic_sewing_requested", result.report.forceDiagnosticSewingRequested},
        {"force_diagnostic_sewing_attempted", result.report.forceDiagnosticSewingAttempted},
        {"diagnostic_candidate_count", result.report.diagnosticSewingCandidateCount},
        {"diagnostic_candidate_patch_count", result.report.diagnosticCandidatePatchCount},
        {"diagnostic_selection_reason", QString::fromStdString(result.report.diagnosticSelectionReason)},
        {"sewable_geometry_available", result.report.sewableGeometryAvailable},
        {"product_validated", result.report.productValidated},
        {"first_product_failure_gate", QString::fromStdString(result.report.firstProductFailureGate)},
        {"constructed_shape", result.report.sewingConstructedShape},
        {"diagnostic_pipeline_completed", result.report.diagnosticPipelineCompleted},
        {"continuation_stop_reason", QString::fromStdString(result.report.continuationStopReason)},
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
    QByteArray output;
    QTextStream stream(&output, QIODevice::WriteOnly);
    stream.setRealNumberPrecision(8);
    stream << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"1000\" viewBox=\"0 0 1000 1000\">\n"
           << "<rect width=\"1000\" height=\"1000\" fill=\"white\"/>\n";
    const auto columns = std::max(
        1, static_cast<int>(std::ceil(
            std::sqrt(static_cast<double>(layout.patches.size())))));
    const auto rows = std::max(
        1, static_cast<int>((layout.patches.size() +
            static_cast<std::size_t>(columns) - 1) /
            static_cast<std::size_t>(columns)));
    const auto cellWidth = 960.0 / columns;
    const auto cellHeight = 920.0 / rows;
    for (std::size_t patchIndex = 0;
         patchIndex < layout.patches.size(); ++patchIndex) {
        const auto& patch = layout.patches[patchIndex];
        const auto column = static_cast<int>(patchIndex) % columns;
        const auto row = static_cast<int>(patchIndex) / columns;
        const auto cellSize = std::min(cellWidth, cellHeight);
        const auto originX = 20.0 + column * cellWidth +
            (cellWidth - cellSize) * 0.5;
        const auto originY = 55.0 + row * cellHeight +
            (cellHeight - cellSize) * 0.5;
        const auto projectUv = [&](const std::array<double, 2>& uv) {
            return std::array<double, 2> {
                originX + 0.08 * cellSize + uv[0] * 0.84 * cellSize,
                originY + 0.92 * cellSize - uv[1] * 0.84 * cellSize};
        };
        std::unordered_map<std::size_t, std::size_t> localByGlobal;
        for (std::size_t local = 0;
             local < patch.vertexIndices.size(); ++local) {
            localByGlobal.emplace(patch.vertexIndices[local], local);
        }
        const auto color = patchColor(static_cast<int>(patchIndex));
        for (const auto triangleIndex : patch.triangleIndices) {
            if (triangleIndex >= mesh.triangles.size()) continue;
            const auto& triangle = mesh.triangles[triangleIndex];
            stream << "<polygon points=\"";
            for (const auto vertex : triangle.vertices) {
                const auto found = localByGlobal.find(vertex);
                if (found == localByGlobal.end()) continue;
                const auto point = projectUv(patch.parameters[found->second]);
                stream << point[0] << ',' << point[1] << ' ';
            }
            stream << "\" fill=\"rgb(" << color[0] << ',' << color[1] << ','
                   << color[2] << ")\" fill-opacity=\"0.45\" stroke=\"#d8d8d8\""
                   << " stroke-width=\"0.35\"/>\n";
        }
        for (const auto& pcurve : patch.edgePcurves) {
            stream << "<polyline points=\"";
            if (pcurve.forward) {
                for (const auto& uv : pcurve.uvPoints) {
                    const auto point = projectUv(uv);
                    stream << point[0] << ',' << point[1] << ' ';
                }
            } else {
                for (auto uv = pcurve.uvPoints.rbegin();
                     uv != pcurve.uvPoints.rend(); ++uv) {
                    const auto point = projectUv(*uv);
                    stream << point[0] << ',' << point[1] << ' ';
                }
            }
            stream << "\" fill=\"none\" stroke=\"#111\" stroke-width=\"2.2\""
                   << " data-edge-index=\"" << pcurve.edgeIndex
                   << "\" data-pcurve-hash=\""
                   << QString::fromStdString(pcurve.hash)
                   << "\"/>\n";
        }
        stream << "<text x=\"" << originX + cellSize * 0.5
               << "\" y=\"" << originY + 18.0
               << "\" text-anchor=\"middle\" font-family=\"sans-serif\""
               << " font-size=\"16\" font-weight=\"bold\">patch "
               << patchIndex << "</text>\n";
    }
    auto totalFlips = 0;
    auto pcurveIntersections = 0;
    auto maximumDistortion = 0.0;
    for (const auto& patch : layout.patches) {
        totalFlips += patch.flippedTriangleCount;
        pcurveIntersections += patch.pcurveSelfIntersectionCount +
            patch.pcurveNonAdjacentIntersectionCount;
        maximumDistortion = std::max(maximumDistortion, patch.maximumDistortion);
    }
    stream << "<rect x=\"8\" y=\"8\" width=\"650\" height=\"28\" fill=\"white\" fill-opacity=\"0.86\"/>"
           << "<text x=\"14\" y=\"28\" font-family=\"sans-serif\" font-size=\"14\" fill=\"#111\">"
           << "domain=curve-consistent-convex-polygon; pcurve intersections="
           << pcurveIntersections << "; UV flips=" << totalFlips
           << "; max distortion=" << maximumDistortion
           << "</text>\n";
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
        {"adaptive_objective", spo::toString(request.options.adaptiveObjective)},
        {"requested_patch_count", result.report.requestedPatchCount},
        {"actual_patch_count", result.report.selectedPatchCount},
        {"patch_count_deviation", result.report.patchCountDeviation},
        {"selection_outcome", QString::fromStdString(result.report.selectionOutcome)},
        {"minimum_qualified_proven", result.report.minimumQualifiedProven},
        {"sampling_mode", spo::toString(request.options.samplingMode)},
        {"layout_carrier_sampling_mode", spo::toString(request.options.layoutCarrierSamplingMode)},
        {"execution_mode", spo::toString(request.executionMode)},
        {"force_diagnostic_sewing", request.forceDiagnosticSewing},
        {"force_diagnostic_sewing_requested", result.report.forceDiagnosticSewingRequested},
        {"force_diagnostic_sewing_attempted", result.report.forceDiagnosticSewingAttempted},
        {"sewable_geometry_available", result.report.sewableGeometryAvailable},
        {"product_validated", result.report.productValidated},
        {"first_product_failure_gate", QString::fromStdString(result.report.firstProductFailureGate)},
        {"layout_gate_passed", result.report.layoutGatePassed},
        {"fit_gate_passed", result.report.fitGatePassed},
        {"best_effort_used", result.report.bestEffortUsed},
        {"retrim_attempted", result.report.retrimAttempted},
        {"sewing_attempted", result.report.sewingAttempted},
        {"sewing_constructed_shape", result.report.sewingConstructedShape},
        {"diagnostic_pipeline_completed", result.report.diagnosticPipelineCompleted},
        {"continuation_stop_reason", QString::fromStdString(result.report.continuationStopReason)},
        {"can_apply", result.canApply()},
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
        {"layout_carrier_mesh_fingerprint", QString::fromStdString(result.report.layoutCarrierMeshFingerprint)},
        {"curve_network_hash", QString::fromStdString(result.report.selectedCurveNetworkHash)},
        {"curve_network", curveNetworkToJson(
            result, request.options.curveValidationSampleCount)},
        {"sampling_quality", QJsonObject {
            {"geometric_limits_passed", result.report.samplingGeometricLimitsPassed},
            {"edge_length_p01", result.report.samplingEdgeLengthP01},
            {"edge_length_p50", result.report.samplingEdgeLengthP50},
            {"edge_length_p99", result.report.samplingEdgeLengthP99},
            {"edge_length_p99_p01_ratio", result.report.samplingEdgeLengthP99P01Ratio},
            {"minimum_angle_p05_degrees", result.report.samplingMinimumAngleP05Degrees},
            {"minimum_angle_p50_degrees", result.report.samplingMinimumAngleP50Degrees},
            {"aspect_ratio_p50", result.report.samplingAspectRatioP50},
            {"aspect_ratio_p95", result.report.samplingAspectRatioP95},
            {"aspect_ratio_p99", result.report.samplingAspectRatioP99},
            {"normalized_quality_p05", result.report.samplingNormalizedQualityP05},
            {"normalized_quality_p50", result.report.samplingNormalizedQualityP50},
            {"zero_area_triangles", QString::number(result.report.samplingZeroAreaTriangleCount)},
            {"non_manifold_edges", QString::number(result.report.samplingNonManifoldEdgeCount)},
            {"remesh_splits", QString::number(result.report.samplingRemeshSplitCount)},
            {"remesh_collapses", QString::number(result.report.samplingRemeshCollapseCount)},
            {"remesh_flips", QString::number(result.report.samplingRemeshFlipCount)},
            {"remesh_relocations", QString::number(result.report.samplingRemeshRelocationCount)}}},
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
            {"seam_maximum_curve_deviation", result.fittedNetwork
                ? result.fittedNetwork->seamMaximumCurveDeviation : 0.0},
            {"seam_maximum_normal_degrees", result.fittedNetwork
                ? result.fittedNetwork->seamMaximumNormalDegrees : 0.0},
            {"seam_maximum_curvature_difference", result.fittedNetwork
                ? result.fittedNetwork->seamMaximumCurvatureDifference : 0.0},
            {"seam_continuity_gate_passed", result.fittedNetwork
                ? result.fittedNetwork->seamContinuityGatePassed : false},
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
    const auto samplingStage = std::find_if(
        result.report.stages.begin(), result.report.stages.end(), [](const auto& stage) {
            return stage.stage == spo::RegionRefitStage::Sampling;
        });
    const auto layoutCarrierHardGatesPassed =
        result.report.layoutCarrierZeroAreaTriangleCount == 0 &&
        result.report.layoutCarrierNonManifoldEdgeCount == 0;
    const auto layoutCarrierSucceeded = result.layoutCarrierMesh &&
        layoutCarrierHardGatesPassed && samplingStage != result.report.stages.end() &&
        samplingStage->success;
    const QJsonObject fittingReference {
        {"role", "FittingReference"},
        {"status", result.referenceMesh ? "Success" : spo::toString(result.report.status)},
        {"vertices", QString::number(result.report.sampleVertexCount)},
        {"triangles", QString::number(result.report.sampleTriangleCount)},
        {"boundary_samples", QString::number(result.report.boundarySampleCount)},
        {"welded_internal_seam_vertices", QString::number(result.report.weldedInternalSeamVertexCount)},
        {"normal_discontinuity_vertices", QString::number(result.report.normalDiscontinuityVertexCount)},
        {"maximum_interior_chord_error", result.report.maximumSamplingChordError},
        {"maximum_interior_normal_error_degrees", result.report.maximumSamplingNormalErrorDegrees},
        {"maximum_boundary_chord_error", result.report.maximumBoundaryChordError},
        {"geometric_limits_passed", result.report.samplingGeometricLimitsPassed},
        {"edge_length_p01", result.report.samplingEdgeLengthP01},
        {"edge_length_p50", result.report.samplingEdgeLengthP50},
        {"edge_length_p99", result.report.samplingEdgeLengthP99},
        {"edge_length_p99_p01_ratio", result.report.samplingEdgeLengthP99P01Ratio},
        {"minimum_angle_p05_degrees", result.report.samplingMinimumAngleP05Degrees},
        {"minimum_angle_p50_degrees", result.report.samplingMinimumAngleP50Degrees},
        {"aspect_ratio_p50", result.report.samplingAspectRatioP50},
        {"aspect_ratio_p95", result.report.samplingAspectRatioP95},
        {"aspect_ratio_p99", result.report.samplingAspectRatioP99},
        {"normalized_quality_p05", result.report.samplingNormalizedQualityP05},
        {"normalized_quality_p50", result.report.samplingNormalizedQualityP50},
        {"zero_area_triangles", QString::number(result.report.samplingZeroAreaTriangleCount)},
        {"non_manifold_edges", QString::number(result.report.samplingNonManifoldEdgeCount)},
        {"remesh_splits", QString::number(result.report.samplingRemeshSplitCount)},
        {"remesh_collapses", QString::number(result.report.samplingRemeshCollapseCount)},
        {"remesh_flips", QString::number(result.report.samplingRemeshFlipCount)},
        {"remesh_relocations", QString::number(result.report.samplingRemeshRelocationCount)},
        {"mesh_fingerprint", QString::fromStdString(result.report.referenceMeshFingerprint)}};
    const QJsonObject layoutCarrier {
        {"role", "LayoutCarrier"},
        {"status", layoutCarrierSucceeded ? "Success" : spo::toString(result.report.status)},
        {"vertices", QString::number(result.report.layoutCarrierVertexCount)},
        {"triangles", QString::number(result.report.layoutCarrierTriangleCount)},
        {"boundary_samples", QString::number(result.report.layoutCarrierBoundarySampleCount)},
        {"minimum_angle_p05_degrees", result.report.layoutCarrierMinimumAngleP05Degrees},
        {"aspect_ratio_p95", result.report.layoutCarrierAspectRatioP95},
        {"zero_area_triangles", QString::number(result.report.layoutCarrierZeroAreaTriangleCount)},
        {"non_manifold_edges", QString::number(result.report.layoutCarrierNonManifoldEdgeCount)},
        {"hard_gates_passed", layoutCarrierHardGatesPassed},
        {"mesh_fingerprint", QString::fromStdString(result.report.layoutCarrierMeshFingerprint)}};
    return {
        {"schema_version", 2},
        {"step_truth", "OriginalFaceIdUvAndBRepEdges"},
        {"reference_stl_quality_is_diagnostic_only", true},
        {"fitting_reference", fittingReference},
        {"layout_carrier", layoutCarrier}};
}

}

int main(int argc, char* argv[]) {
    const auto coreStarted = std::chrono::steady_clock::now();
    if (argc < 5) {
        std::cerr << "用法：native_region_refit_probe <输入.step> <输出目录> "
                     "(--candidate-id <编号> | --faces <编号,编号,...>) "
                     "[--locked-edges <编号,编号,...>] [--patch-mode <adaptive|target>] "
                     "[--requested-patch-count <数量>] [--patch-count-search-radius <数量>] "
                     "[--adaptive-objective <minimum-qualified>] [--adaptive-safety-cap <数量>] "
                     "[--layout <adaptive|fixed>] [--patch-count <数量>] [--wall-time-ms <毫秒>] "
                     "[--sampling-mode <adaptive-occt|quality-balanced|uv-uniform>] "
                     "[--layout-carrier-sampling-mode <adaptive-occt|quality-balanced|uv-uniform>] "
                     "[--target-sample-triangles <数量>] [--maximum-full-fit-count <数量>] "
                     "[--target-layout-carrier-triangles <数量>] "
                     "[--best-effort-through-sewing] [--force-diagnostic-sewing] "
                     "[--max-memory-mib <MiB>] [--cache-dir <目录>] "
                     "[--input-read-budget-ms <毫秒>] "
                     "[--cache-policy <disabled|read-write|read-only|refresh>] "
                     "[--run-identity <名称>] [--commit <提交>] "
                     "[--constraints-only | --sampling-only | --layout-only | --fit-only]\n";
        return 2;
    }

    const std::filesystem::path input = argv[1];
    const std::filesystem::path outputDirectory = argv[2];
    const auto finalDirectory = outputDirectory / "artifacts" / "final";
    const auto artifactStatusPath = finalDirectory / "artifact_status.json";
    for (const auto& directory : {
            outputDirectory / "artifacts" / "curve_network",
            outputDirectory / "artifacts" / "layout",
            outputDirectory / "artifacts" / "fit",
            outputDirectory / "artifacts" / "brep",
            finalDirectory,
            outputDirectory / "artifacts" / "sampling",
            outputDirectory / "diagnostics",
            outputDirectory / "logs",
            outputDirectory / "validation",
            outputDirectory / "external_cad"}) {
        std::filesystem::create_directories(directory);
    }
    if (!std::filesystem::exists(artifactStatusPath) &&
        !writeJson(artifactStatusPath, QJsonObject {
            {"schema_version", 1},
            {"status", "Unavailable"},
            {"product_validated", false},
            {"diagnostic_only", false},
            {"can_apply", false},
            {"constructible_geometry", false},
            {"sewn_step", QJsonValue::Null},
            {"sha256", QJsonValue::Null},
            {"first_product_failure_gate", QJsonValue::Null},
            {"unavailable_reason",
                "The run has not produced constructible sewn geometry."}})) {
        std::cerr << "无法初始化永久产物状态目录。\n";
        return 1;
    }
    int candidateId = -1;
    bool constraintsOnly = false;
    bool samplingOnly = false;
    bool layoutOnly = false;
    bool fitOnly = false;
    bool performanceOnly = false;
    bool cachePolicySpecified = false;
    bool algorithmFingerprintSpecified = false;
    std::string runIdentity = "single";
    std::string commit;
    spo::RegionRefitRequest request;
    request.options.algorithmFingerprint =
        "native-regional-refit-v39-constructible-layout";
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
            } else if (argument == "--patch-mode") {
                if (++index >= argc) return 2;
                const std::string value = argv[index];
                if (value == "adaptive") request.layoutMode = spo::RegionRefitLayoutMode::Adaptive;
                else if (value == "target") request.layoutMode = spo::RegionRefitLayoutMode::TargetPatchCount;
                else return 2;
            } else if (argument == "--patch-count") {
                if (++index >= argc) return 2;
                request.options.fixedPatchCount = std::stoi(argv[index]);
            } else if (argument == "--requested-patch-count") {
                if (++index >= argc) return 2;
                request.options.requestedPatchCount = std::stoi(argv[index]);
            } else if (argument == "--patch-count-search-radius") {
                if (++index >= argc) return 2;
                request.options.patchCountSearchRadius = std::stoi(argv[index]);
            } else if (argument == "--adaptive-objective") {
                if (++index >= argc || std::string(argv[index]) != "minimum-qualified") return 2;
                request.options.adaptiveObjective = spo::RegionRefitAdaptiveObjective::MinimumQualified;
            } else if (argument == "--adaptive-safety-cap") {
                if (++index >= argc) return 2;
                request.options.maximumAdaptivePatchCount = std::stoi(argv[index]);
            } else if (argument == "--sampling-mode") {
                if (++index >= argc) return 2;
                const std::string value = argv[index];
                if (value == "adaptive-occt") {
                    request.options.samplingMode = spo::RegionRefitSamplingMode::AdaptiveOcct;
                } else if (value == "quality-balanced") {
                    request.options.samplingMode = spo::RegionRefitSamplingMode::QualityBalanced;
                } else if (value == "uv-uniform") {
                    request.options.samplingMode = spo::RegionRefitSamplingMode::UniformUv;
                } else {
                    return 2;
                }
            } else if (argument == "--layout-carrier-sampling-mode") {
                if (++index >= argc) return 2;
                const std::string value = argv[index];
                if (value == "adaptive-occt") {
                    request.options.layoutCarrierSamplingMode = spo::RegionRefitSamplingMode::AdaptiveOcct;
                } else if (value == "quality-balanced") {
                    request.options.layoutCarrierSamplingMode = spo::RegionRefitSamplingMode::QualityBalanced;
                } else if (value == "uv-uniform") {
                    request.options.layoutCarrierSamplingMode = spo::RegionRefitSamplingMode::UniformUv;
                } else {
                    return 2;
                }
            } else if (argument == "--target-sample-triangles") {
                if (++index >= argc) return 2;
                request.options.targetSampleTriangles = std::stoull(argv[index]);
            } else if (argument == "--target-layout-carrier-triangles") {
                if (++index >= argc) return 2;
                request.options.targetLayoutCarrierTriangles = std::stoull(argv[index]);
            } else if (argument == "--maximum-full-fit-count") {
                if (++index >= argc) return 2;
                request.options.maximumFullFitCount = std::stoi(argv[index]);
            } else if (argument == "--best-effort-through-sewing") {
                request.executionMode = spo::RegionRefitExecutionMode::BestEffortThroughSewing;
            } else if (argument == "--force-diagnostic-sewing") {
                request.forceDiagnosticSewing = true;
            } else if (argument == "--wall-time-ms") {
                if (++index >= argc) return 2;
                request.options.maximumWallClockMilliseconds = std::stoll(argv[index]);
            } else if (argument == "--max-memory-mib") {
                if (++index >= argc) return 2;
                request.options.maximumMemoryMiB = std::stoull(argv[index]);
                request.options.solverMemoryPreflightMiB = request.options.maximumMemoryMiB > 1024
                    ? request.options.maximumMemoryMiB - 1024
                    : request.options.maximumMemoryMiB;
            } else if (argument == "--input-read-budget-ms") {
                if (++index >= argc) return 2;
                request.options.inputReadBudgetMilliseconds = std::stoll(argv[index]);
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
                algorithmFingerprintSpecified = true;
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

    if (!algorithmFingerprintSpecified) {
        request.options.algorithmFingerprint = std::string(
            "native-regional-refit-v39-constructible-layout-") +
            spo::toString(request.options.samplingMode) + "-" +
            spo::toString(request.options.layoutCarrierSamplingMode) + "-" +
            spo::toString(request.layoutMode);
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
    if (request.layoutMode == spo::RegionRefitLayoutMode::TargetPatchCount &&
        (request.options.requestedPatchCount <= 0 || request.options.patchCountSearchRadius < 0)) {
        std::cerr << "指定面数模式必须提供正数 --requested-patch-count 和非负搜索半径。\n";
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
        writeJson(
            diagnostics / "curve_network.json",
            curveNetworkToJson(result, request.options.curveValidationSampleCount)) &&
        writeJson(diagnostics / "layout_report.json", layoutReportToJson(result)) &&
        writeJson(diagnostics / "fit_report.json", fitReportToJson(result)) &&
        writeJson(
            diagnostics / "edge_consistency_report.json",
            edgeConsistencyToJson(result, request.options.curveOnSurfaceTolerance)) &&
        writeJson(diagnostics / "triangulation_report.json", triangulationToJson(result)) &&
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
        if (result.layoutCarrierMesh) {
            const auto carrierWrite = spo::StlWriter().write(
                result.layoutCarrierMesh->toStlMesh(),
                samplingDirectory / "layout_carrier.stl");
            if (!carrierWrite.success) {
                std::cerr << carrierWrite.message << '\n';
                return 1;
            }
        }
    }
    if (!performanceOnly && result.layoutCarrierMesh && !result.generatedLayouts.empty()) {
        const auto candidateDirectory =
            outputDirectory / "artifacts" / "layout" / "candidates";
        for (const auto& layout : result.generatedLayouts) {
            if (!layout) continue;
            const auto stem = std::string("k_") + std::to_string(layout->patchCount);
            if (!writeJson(
                    candidateDirectory / (stem + ".json"),
                    selectedLayoutToJson(*layout)) ||
                !writeText(
                    candidateDirectory / (stem + ".svg"),
                    layoutSvg(*result.layoutCarrierMesh, *layout))) {
                std::cerr << "无法写入布局候选证据。\n";
                return 1;
            }
        }
    }
    if (!performanceOnly && result.selectedCurveNetwork) {
        const auto curveNetworkDirectory =
            outputDirectory / "artifacts" / "curve_network";
        const auto curveShape =
            curveNetworkShape(*result.selectedCurveNetwork);
        if (curveShape.IsNull()) {
            std::cerr << "无法构造冻结曲线网络 STEP compound。\n";
            return 1;
        }
        if (!writeJson(
                curveNetworkDirectory / "selected_curve_network.json",
                curveNetworkToJson(
                    result, request.options.curveValidationSampleCount)) ||
            !writeText(
                curveNetworkDirectory / "selected_curve_network.ply",
                curveNetworkPly(
                    *result.selectedCurveNetwork,
                    request.options.curveValidationSampleCount))) {
            std::cerr << "无法写入冻结曲线网络证据。\n";
            return 1;
        }
        const auto curveWrite = spo::StepWriter().write(
            spo::ShapeDocument(curveShape, {}),
            curveNetworkDirectory / "selected_curve_network.step");
        if (!curveWrite.success()) {
            std::cerr << curveWrite.message() << '\n';
            return 1;
        }
    }
    if (!performanceOnly && result.layoutCarrierMesh && result.selectedLayout) {
        const auto layoutDirectory = outputDirectory / "artifacts" / "layout";
        if (!writeJson(layoutDirectory / "selected_parameter_domain.json", selectedLayoutToJson(*result.selectedLayout)) ||
            !writeText(layoutDirectory / "selected_parameter_domain.ply", layoutPly(*result.layoutCarrierMesh, *result.selectedLayout)) ||
            !writeText(layoutDirectory / "selected_parameter_domain.svg", layoutSvg(*result.layoutCarrierMesh, *result.selectedLayout))) {
            std::cerr << "无法写入布局证据。\n";
            return 1;
        }
    }
    if (!performanceOnly && result.fittedNetwork && !result.fittedPatchNetwork.IsNull()) {
        const auto fitDirectory = outputDirectory / "artifacts" / "fit";
        std::filesystem::create_directories(fitDirectory);
        const auto fitName = result.report.fitGatePassed
            ? std::filesystem::path("fitted_support_surfaces.step")
            : std::filesystem::path("best_effort_support_surfaces.step");
        const auto write = spo::StepWriter().write(
            spo::ShapeDocument(result.fittedPatchNetwork, {}),
            fitDirectory / fitName);
        if (!write.success()) {
            std::cerr << write.message() << '\n';
            return 1;
        }
    }
    if (!performanceOnly && !result.replacementQuilt.IsNull()) {
        const auto quiltPath =
            outputDirectory / "artifacts" / "brep" / "replacement_quilt.step";
        const auto write = spo::StepWriter().write(
            spo::ShapeDocument(result.replacementQuilt, {}),
            quiltPath);
        if (!write.success()) {
            std::cerr << write.message() << '\n';
            return 1;
        }
    }
    std::filesystem::path assemblyOutputPath;
    if (!performanceOnly && (result.canApply() || !result.diagnosticSewnShape.IsNull())) {
        const auto brepDirectory = outputDirectory / "artifacts" / "brep";
        assemblyOutputPath = brepDirectory / "sewn_attempt.step";
        const auto& assemblyShape = result.canApply()
            ? result.finalShape
            : result.diagnosticSewnShape;
        const auto write = spo::StepWriter().write(
            spo::ShapeDocument(assemblyShape, {}),
            assemblyOutputPath);
        if (!write.success()) {
            std::cerr << write.message() << '\n';
            return 1;
        }
    }
    auto outputComplexityPassed = performanceOnly;
    if (!performanceOnly) {
        const auto noOpPath = diagnostics / ".no_op_roundtrip.step";
        const auto noOpWrite = spo::StepWriter().write(read.document, noOpPath);
        const auto baselineComplexity = noOpWrite.success()
            ? measureStepFile(noOpPath)
            : StepFileComplexity {};
        const auto candidateComplexity = assemblyOutputPath.empty()
            ? StepFileComplexity {}
            : measureStepFile(assemblyOutputPath);
        const auto complexityReport = outputComplexityToJson(
            baselineComplexity,
            candidateComplexity,
            result,
            request.options.maximumControlPointsPerSide);
        outputComplexityPassed = complexityReport.value("passed").toBool();
        if (!writeJson(diagnostics / "output_complexity_report.json", complexityReport)) {
            std::cerr << "无法写入输出复杂度报告。\n";
            return 1;
        }
        std::error_code removeError;
        std::filesystem::remove(noOpPath, removeError);
        if (removeError) {
            std::cerr << "无法删除临时无修改往返 STEP。\n";
            return 1;
        }
        if (result.canApply() && !outputComplexityPassed && !assemblyOutputPath.empty()) {
            const auto diagnosticPath = assemblyOutputPath.parent_path() / "sewn_attempt.step";
            if (assemblyOutputPath != diagnosticPath) {
                std::error_code renameError;
                std::filesystem::rename(assemblyOutputPath, diagnosticPath, renameError);
                if (renameError) {
                    std::cerr << "无法把复杂度门失败结果降级为诊断产物。\n";
                    return 1;
                }
            }
            assemblyOutputPath = diagnosticPath;
            result.report.status = spo::RegionRefitStatus::Failed;
            result.report.failureStage = spo::RegionRefitStage::StepRoundtrip;
            result.report.selectionOutcome = "NoProductValidatedResult";
            result.report.productValidated = false;
            result.report.firstProductFailureGate = "OutputComplexity";
            result.report.unattainableReason =
                "The sewn candidate failed the output-complexity product gate.";
            result.report.message =
                "The sewn candidate passed geometry and topology gates but failed output complexity.";
            result.finalShape.Nullify();
            if (!writeJson(diagnostics / "layout_report.json", layoutReportToJson(result)) ||
                !writeJson(diagnostics / "fit_report.json", fitReportToJson(result)) ||
                !writeJson(diagnostics / "edge_consistency_report.json",
                    edgeConsistencyToJson(result, request.options.curveOnSurfaceTolerance)) ||
                !writeJson(diagnostics / "triangulation_report.json", triangulationToJson(result)) ||
                !writeJson(diagnostics / "retrim_report.json", retrimReportToJson(result)) ||
                !writeJson(diagnostics / "sewing_report.json", sewingReportToJson(result)) ||
                !writeJson(diagnostics / "strict_topology_report.json", strictTopologyReportToJson(result))) {
                std::cerr << "无法把复杂度门失败写回最终诊断。\n";
                return 1;
            }
        }
        auto finalPerformance = performanceToJson(result, request, timing, runIdentity, commit);
        finalPerformance.insert("output_complexity_passed", outputComplexityPassed);
        finalPerformance.insert("product_validated", result.canApply() && outputComplexityPassed);
        if (!writeJson(diagnostics / performanceName, finalPerformance)) {
            std::cerr << "无法更新最终性能与产品门状态。\n";
            return 1;
        }
    }

    if (!performanceOnly) {
        const auto productValidated = result.canApply() && outputComplexityPassed;
        const auto constructible = productValidated ||
            !result.diagnosticSewnShape.IsNull();
        auto finalStatus = QString("Unavailable");
        auto finalStepName = std::filesystem::path {};
        auto finalSha256 = std::string {};
        if (constructible) {
            finalStatus = productValidated ? "ProductValidated" : "DiagnosticOnly";
            finalStepName = productValidated
                ? std::filesystem::path("final_sewn.step")
                : std::filesystem::path("diagnostic_sewn.step");
            const auto& shape = productValidated
                ? result.finalShape : result.diagnosticSewnShape;
            const auto finalWrite = spo::StepWriter().write(
                spo::ShapeDocument(shape, {}),
                finalDirectory / finalStepName);
            if (!finalWrite.success()) {
                std::cerr << finalWrite.message() << '\n';
                return 1;
            }
            finalSha256 = sha256File(finalDirectory / finalStepName);
            if (finalSha256.empty()) {
                std::cerr << "无法计算永久 STEP 产物的 SHA-256。\n";
                return 1;
            }
        }
        const auto unavailableReason = constructible
            ? QString()
            : QString::fromStdString(
                result.report.continuationStopReason.empty()
                    ? result.report.message
                    : result.report.continuationStopReason);
        if (!writeJson(artifactStatusPath, QJsonObject {
                {"schema_version", 1},
                {"status", finalStatus},
                {"product_validated", productValidated},
                {"diagnostic_only", constructible && !productValidated},
                {"can_apply", productValidated},
                {"constructible_geometry", constructible},
                {"sewn_step", constructible
                    ? QJsonValue(QString::fromStdWString(finalStepName.wstring()))
                    : QJsonValue(QJsonValue::Null)},
                {"sha256", constructible
                    ? QJsonValue(QString::fromStdString(finalSha256))
                    : QJsonValue(QJsonValue::Null)},
                {"first_product_failure_gate",
                    result.report.firstProductFailureGate.empty()
                        ? QJsonValue(QJsonValue::Null)
                        : QJsonValue(QString::fromStdString(
                            result.report.firstProductFailureGate))},
                {"unavailable_reason", constructible
                    ? QJsonValue(QJsonValue::Null)
                    : QJsonValue(unavailableReason)},
                {"message", QString::fromStdString(result.report.message)}})) {
            std::cerr << "无法写入永久产物状态。\n";
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
    return result.canApply() && outputComplexityPassed ? 0 : 1;
}
