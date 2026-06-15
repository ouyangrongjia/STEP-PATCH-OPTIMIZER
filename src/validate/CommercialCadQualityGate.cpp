#include "validate/CommercialCadQualityGate.h"

#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"
#include "feature/FeatureEdgeDetector.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <QJsonDocument>
#include <QJsonObject>
#include <Standard_Failure.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_set>
#include <utility>
#include <vector>

namespace spo {

namespace {

void append_warning(CommercialCadQualityGateReport& report, const std::string& message) {
    if (message.empty()) {
        return;
    }
    if (!report.warningMessage.empty()) {
        report.warningMessage += "\n";
    }
    report.warningMessage += message;
}

CommercialCadQualityGateReport fail_report(std::string message) {
    CommercialCadQualityGateReport report;
    report.message = std::move(message);
    return report;
}

std::vector<EdgeId> boundary_edges_for(
    const RegionBoundaryAnalysis& boundary,
    const MergeCandidate& candidate) {
    if (!boundary.ordered_boundary_edges.empty()) {
        return boundary.ordered_boundary_edges;
    }
    return candidate.boundary_edges;
}

std::vector<gp_Pnt> sample_edge_points(
    const TopologyGraph& topology,
    EdgeId edgeId,
    int requestedSamples,
    bool includeEndpoints) {
    std::vector<gp_Pnt> points;
    if (edgeId >= topology.edgeCount()) {
        return points;
    }

    const auto sampleCount = std::max(requestedSamples, includeEndpoints ? 2 : 1);
    try {
        BRepAdaptor_Curve curve(topology.edge(edgeId));
        const auto first = curve.FirstParameter();
        const auto last = curve.LastParameter();
        if (!std::isfinite(first) || !std::isfinite(last)) {
            return points;
        }

        if (sampleCount <= 1 || std::abs(last - first) < 1.0e-15) {
            points.push_back(curve.Value(first));
            return points;
        }

        points.reserve(static_cast<std::size_t>(sampleCount));
        for (int index = 0; index < sampleCount; ++index) {
            if (!includeEndpoints && (index == 0 || index == sampleCount - 1)) {
                continue;
            }
            const auto t = static_cast<double>(index) / static_cast<double>(sampleCount - 1);
            const auto parameter = first + (last - first) * t;
            points.push_back(curve.Value(parameter));
        }
    } catch (const Standard_Failure&) {
        points.clear();
    }
    return points;
}

std::vector<gp_Pnt> endpoint_anchors(
    const TopologyGraph& topology,
    const std::vector<EdgeId>& edgeIds,
    double dedupTolerance) {
    std::vector<gp_Pnt> anchors;
    const auto toleranceSquared = dedupTolerance * dedupTolerance;

    auto add_anchor = [&](const gp_Pnt& point) {
        for (const auto& existing : anchors) {
            if (existing.SquareDistance(point) <= toleranceSquared) {
                return;
            }
        }
        anchors.push_back(point);
    };

    for (const auto edgeId : edgeIds) {
        const auto endpoints = sample_edge_points(topology, edgeId, 2, true);
        for (const auto& point : endpoints) {
            add_anchor(point);
        }
    }
    return anchors;
}

double distance_to_shape(const gp_Pnt& point, const TopoDS_Shape& shape) {
    if (shape.IsNull()) {
        return std::numeric_limits<double>::infinity();
    }

    try {
        auto vertex = BRepBuilderAPI_MakeVertex(point).Vertex();
        BRepExtrema_DistShapeShape distance(vertex, shape);
        distance.Perform();
        if (!distance.IsDone()) {
            return std::numeric_limits<double>::infinity();
        }
        return distance.Value();
    } catch (const Standard_Failure&) {
        return std::numeric_limits<double>::infinity();
    }
}

CommercialCadDistanceStats summarize_distances(
    const std::vector<gp_Pnt>& points,
    const TopoDS_Shape& shape,
    double tolerance) {
    CommercialCadDistanceStats stats;
    stats.evaluated = true;
    stats.tolerance = tolerance;
    stats.samples = static_cast<int>(points.size());
    if (points.empty()) {
        return stats;
    }

    std::vector<double> distances;
    distances.reserve(points.size());
    for (const auto& point : points) {
        const auto distance = distance_to_shape(point, shape);
        distances.push_back(distance);
        if (distance > tolerance) {
            ++stats.overTolerance;
        }
    }

    stats.maxDistance = *std::max_element(distances.begin(), distances.end());
    stats.meanDistance = std::accumulate(distances.begin(), distances.end(), 0.0) /
        static_cast<double>(distances.size());

    double sumSquares = 0.0;
    for (const auto distance : distances) {
        sumSquares += distance * distance;
    }
    stats.rmsDistance = std::sqrt(sumSquares / static_cast<double>(distances.size()));

    std::sort(distances.begin(), distances.end());
    const auto p95Index = std::min(
        distances.size() - 1,
        static_cast<std::size_t>(std::ceil(static_cast<double>(distances.size()) * 0.95)) - 1);
    stats.p95Distance = distances[p95Index];
    return stats;
}

std::unordered_set<EdgeId> sharp_feature_edge_ids(const FeatureEdgeDetectionResult* featureEdges) {
    std::unordered_set<EdgeId> result;
    if (featureEdges == nullptr) {
        return result;
    }
    for (const auto& edge : featureEdges->edges) {
        if (edge.kind == FeatureEdgeKind::Sharp ||
            edge.kind == FeatureEdgeKind::Free ||
            edge.kind == FeatureEdgeKind::Multiple) {
            result.insert(edge.edge);
        }
    }
    return result;
}

bool stats_passed(const CommercialCadDistanceStats& stats) {
    return stats.evaluated && stats.samples > 0 && stats.maxDistance <= stats.tolerance;
}

bool optional_stats_passed(const CommercialCadDistanceStats& stats) {
    return !stats.evaluated || stats.samples == 0 || stats.maxDistance <= stats.tolerance;
}

QJsonObject stats_to_json(const CommercialCadDistanceStats& stats) {
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

}

CommercialCadQualityGateReport CommercialCadQualityGate::evaluate(
    const CommercialCadQualityGateInput& input) const {
    if (input.document == nullptr || !input.document->hasShape()) {
        return fail_report("CommercialCadLikeQualityGate requires a source document.");
    }
    if (input.candidate == nullptr) {
        return fail_report("CommercialCadLikeQualityGate requires a candidate.");
    }
    if (input.boundary == nullptr || !input.boundary->valid) {
        return fail_report("CommercialCadLikeQualityGate requires a valid original CAD boundary.");
    }
    if (input.patchShape == nullptr || input.patchShape->IsNull()) {
        return fail_report("CommercialCadLikeQualityGate requires an imported patch shape.");
    }

    CommercialCadQualityGateReport report;
    report.evaluated = true;
    report.candidateId = input.candidate->candidate_id;

    const auto boundaryEdges = boundary_edges_for(*input.boundary, *input.candidate);
    report.boundaryEdgeCount = static_cast<int>(boundaryEdges.size());
    if (boundaryEdges.empty()) {
        report.message = "CommercialCadLikeQualityGate failed: no original boundary edges were available.";
        return report;
    }

    std::vector<gp_Pnt> boundarySamples;
    for (const auto edgeId : boundaryEdges) {
        auto points = sample_edge_points(
            input.document->topology(),
            edgeId,
            input.options.boundarySamplesPerEdge,
            true);
        boundarySamples.insert(boundarySamples.end(), points.begin(), points.end());
    }
    report.boundary = summarize_distances(
        boundarySamples,
        *input.patchShape,
        input.options.maxBoundaryDistance);

    const auto anchors = endpoint_anchors(
        input.document->topology(),
        boundaryEdges,
        input.options.anchorDedupTolerance);
    report.cornerAnchors = summarize_distances(
        anchors,
        *input.patchShape,
        input.options.maxCornerAnchorDistance);

    const auto sharpEdges = sharp_feature_edge_ids(input.featureEdges);
    std::vector<gp_Pnt> featureSamples;
    if (!sharpEdges.empty()) {
        for (const auto edgeId : boundaryEdges) {
            if (sharpEdges.find(edgeId) == sharpEdges.end()) {
                continue;
            }
            ++report.featureBoundaryEdgeCount;
            auto points = sample_edge_points(
                input.document->topology(),
                edgeId,
                input.options.featureEdgeSamplesPerEdge,
                true);
            featureSamples.insert(featureSamples.end(), points.begin(), points.end());
        }
    }
    report.featureEdges = summarize_distances(
        featureSamples,
        *input.patchShape,
        input.options.maxFeatureEdgeDistance);
    if (input.featureEdges == nullptr) {
        append_warning(report, "Feature edge drift was evaluated without FeatureEdgeDetectionResult; feature-edge sample set is empty.");
    } else if (featureSamples.empty()) {
        append_warning(report, "No sharp/free/multiple feature boundary edges intersected this candidate boundary.");
    }

    report.sharpCornerPreservationPassed =
        stats_passed(report.boundary) &&
        stats_passed(report.cornerAnchors) &&
        optional_stats_passed(report.featureEdges);
    report.passed = report.sharpCornerPreservationPassed;

    if (report.passed) {
        report.message = "CommercialCadLikeQualityGate passed.";
    } else {
        report.message = "CommercialCadLikeQualityGate failed: boundary/corner/feature drift exceeds configured tolerance.";
    }
    return report;
}

std::string toJson(const CommercialCadQualityGateReport& report) {
    QJsonObject gate;
    gate.insert("evaluated", report.evaluated);
    gate.insert("passed", report.passed);
    gate.insert("sharp_corner_preservation_passed", report.sharpCornerPreservationPassed);
    gate.insert("candidate_id", report.candidateId);
    gate.insert("boundary_edge_count", report.boundaryEdgeCount);
    gate.insert("feature_boundary_edge_count", report.featureBoundaryEdgeCount);
    gate.insert("boundary", stats_to_json(report.boundary));
    gate.insert("corner_anchors", stats_to_json(report.cornerAnchors));
    gate.insert("feature_edges", stats_to_json(report.featureEdges));
    gate.insert("message", QString::fromStdString(report.message));
    gate.insert("warning", QString::fromStdString(report.warningMessage));

    QJsonObject root;
    root.insert("commercial_cad_like_quality_gate", gate);
    const QJsonDocument document(root);
    const auto json = document.toJson(QJsonDocument::Indented);
    return {json.constData(), static_cast<std::size_t>(json.size())};
}

}
