#include "patch/PatchTrimDiagnostics.h"

#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_State.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace spo {

namespace {

struct Box3 {
    bool valid = false;
    double minX = 0.0;
    double minY = 0.0;
    double minZ = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    double maxZ = 0.0;
};

struct EdgeSample {
    EdgeId edgeId = -1;
    gp_Pnt point;
};

struct DistanceStats {
    double max = 0.0;
    double p95 = 0.0;
    double rms = 0.0;
};

bool valid_options(const PatchTrimDiagnosticsOptions& options) {
    return options.boundarySamplesPerEdge >= 2 &&
        options.surfaceGridDivisions >= 2 &&
        (options.maxBoundarySamples == 0 || options.maxBoundarySamples >= 2) &&
        (options.maxSurfaceSamples == 0 || options.maxSurfaceSamples >= 2) &&
        options.distanceTolerance >= 0.0;
}

template <typename T>
std::vector<T> downsample_evenly(const std::vector<T>& input, int maxSamples) {
    if (maxSamples <= 0 || input.size() <= static_cast<std::size_t>(maxSamples)) {
        return input;
    }
    if (maxSamples == 1) {
        return {input.front()};
    }

    std::vector<T> output;
    output.reserve(static_cast<std::size_t>(maxSamples));
    const auto last = input.size() - 1;
    for (int index = 0; index < maxSamples; ++index) {
        const auto t = static_cast<double>(index) / static_cast<double>(maxSamples - 1);
        const auto sourceIndex = std::min(
            last,
            static_cast<std::size_t>(std::llround(t * static_cast<double>(last))));
        output.push_back(input[sourceIndex]);
    }
    return output;
}

std::vector<EdgeId> boundary_edges_for(
    const RegionBoundaryAnalysis& boundary,
    const MergeCandidate& candidate) {
    if (!boundary.ordered_boundary_edges.empty()) {
        return boundary.ordered_boundary_edges;
    }
    return candidate.boundary_edges;
}

void include_point(Box3& box, const gp_Pnt& point) {
    if (!box.valid) {
        box.valid = true;
        box.minX = box.maxX = point.X();
        box.minY = box.maxY = point.Y();
        box.minZ = box.maxZ = point.Z();
        return;
    }
    box.minX = std::min(box.minX, point.X());
    box.minY = std::min(box.minY, point.Y());
    box.minZ = std::min(box.minZ, point.Z());
    box.maxX = std::max(box.maxX, point.X());
    box.maxY = std::max(box.maxY, point.Y());
    box.maxZ = std::max(box.maxZ, point.Z());
}

double distance_outside_box(const gp_Pnt& point, const Box3& box) {
    if (!box.valid) {
        return 0.0;
    }
    const auto dx = point.X() < box.minX
        ? box.minX - point.X()
        : (point.X() > box.maxX ? point.X() - box.maxX : 0.0);
    const auto dy = point.Y() < box.minY
        ? box.minY - point.Y()
        : (point.Y() > box.maxY ? point.Y() - box.maxY : 0.0);
    const auto dz = point.Z() < box.minZ
        ? box.minZ - point.Z()
        : (point.Z() > box.maxZ ? point.Z() - box.maxZ : 0.0);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool point_inside_box(const gp_Pnt& point, const Box3& box, double tolerance) {
    return box.valid &&
        point.X() >= box.minX - tolerance &&
        point.X() <= box.maxX + tolerance &&
        point.Y() >= box.minY - tolerance &&
        point.Y() <= box.maxY + tolerance &&
        point.Z() >= box.minZ - tolerance &&
        point.Z() <= box.maxZ + tolerance;
}

std::vector<EdgeSample> sample_boundary_edges(
    const TopologyGraph& topology,
    const std::vector<EdgeId>& edgeIds,
    int samplesPerEdge) {
    std::vector<EdgeSample> samples;
    for (const auto edgeId : edgeIds) {
        if (edgeId >= topology.edgeCount()) {
            continue;
        }
        try {
            BRepAdaptor_Curve curve(topology.edge(edgeId));
            const auto first = curve.FirstParameter();
            const auto last = curve.LastParameter();
            if (!std::isfinite(first) || !std::isfinite(last)) {
                continue;
            }
            for (int index = 0; index < samplesPerEdge; ++index) {
                const auto t = samplesPerEdge <= 1
                    ? 0.0
                    : static_cast<double>(index) / static_cast<double>(samplesPerEdge - 1);
                EdgeSample sample;
                sample.edgeId = edgeId;
                sample.point = curve.Value(first + (last - first) * t);
                samples.push_back(sample);
            }
        } catch (const Standard_Failure&) {
        }
    }
    return samples;
}

double distance_to_shape(const gp_Pnt& point, const TopoDS_Shape& shape) {
    if (shape.IsNull()) {
        return std::numeric_limits<double>::infinity();
    }
    try {
        const auto vertex = BRepBuilderAPI_MakeVertex(point).Vertex();
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

DistanceStats summarize(std::vector<double> distances) {
    DistanceStats stats;
    if (distances.empty()) {
        return stats;
    }
    stats.max = *std::max_element(distances.begin(), distances.end());

    double sumSquares = 0.0;
    for (const auto distance : distances) {
        sumSquares += distance * distance;
    }
    stats.rms = std::sqrt(sumSquares / static_cast<double>(distances.size()));

    std::sort(distances.begin(), distances.end());
    const auto p95Index = std::min(
        distances.size() - 1,
        static_cast<std::size_t>(std::ceil(static_cast<double>(distances.size()) * 0.95)) - 1);
    stats.p95 = distances[p95Index];
    return stats;
}

std::vector<gp_Pnt> sample_face(const TopoDS_Face& face, int divisions) {
    std::vector<gp_Pnt> samples;
    const auto surface = BRep_Tool::Surface(face);
    if (surface.IsNull()) {
        return samples;
    }

    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    try {
        BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    } catch (const Standard_Failure&) {
        return samples;
    }
    if (!std::isfinite(uMin) || !std::isfinite(uMax) ||
        !std::isfinite(vMin) || !std::isfinite(vMax) ||
        std::abs(uMax - uMin) <= 1.0e-12 ||
        std::abs(vMax - vMin) <= 1.0e-12) {
        return samples;
    }

    for (int uIndex = 0; uIndex < divisions; ++uIndex) {
        const auto uT = divisions <= 1
            ? 0.0
            : static_cast<double>(uIndex) / static_cast<double>(divisions - 1);
        const auto u = uMin + (uMax - uMin) * uT;
        for (int vIndex = 0; vIndex < divisions; ++vIndex) {
            const auto vT = divisions <= 1
                ? 0.0
                : static_cast<double>(vIndex) / static_cast<double>(divisions - 1);
            const auto v = vMin + (vMax - vMin) * vT;
            try {
                BRepClass_FaceClassifier classifier(face, gp_Pnt2d(u, v), 1.0e-7);
                const auto state = classifier.State();
                if (state != TopAbs_IN && state != TopAbs_ON) {
                    continue;
                }
                samples.push_back(surface->Value(u, v));
            } catch (const Standard_Failure&) {
            }
        }
    }
    return samples;
}

std::vector<gp_Pnt> sample_shape_faces(const TopoDS_Shape& shape, int divisions) {
    std::vector<gp_Pnt> samples;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        auto faceSamples = sample_face(TopoDS::Face(explorer.Current()), divisions);
        samples.insert(samples.end(), faceSamples.begin(), faceSamples.end());
    }
    return samples;
}

std::vector<gp_Pnt> sample_candidate_faces(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    int divisions) {
    std::vector<gp_Pnt> samples;
    for (const auto faceId : candidate.faces) {
        if (faceId >= document.topology().faceCount()) {
            continue;
        }
        auto faceSamples = sample_face(document.topology().face(faceId), divisions);
        samples.insert(samples.end(), faceSamples.begin(), faceSamples.end());
    }
    return samples;
}

int count_shapes(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
    int count = 0;
    if (shape.IsNull()) {
        return count;
    }
    for (TopExp_Explorer explorer(shape, type); explorer.More(); explorer.Next()) {
        ++count;
    }
    return count;
}

double edge_length(const TopoDS_Edge& edge) {
    GProp_GProps properties;
    BRepGProp::LinearProperties(edge, properties);
    return std::max(0.0, properties.Mass());
}

gp_Pnt edge_midpoint(const TopoDS_Edge& edge) {
    try {
        BRepAdaptor_Curve curve(edge);
        return curve.Value((curve.FirstParameter() + curve.LastParameter()) * 0.5);
    } catch (const Standard_Failure&) {
        return {};
    }
}

int count_wires(const TopoDS_Face& face) {
    int count = 0;
    for (TopExp_Explorer explorer(face, TopAbs_WIRE); explorer.More(); explorer.Next()) {
        ++count;
    }
    return count;
}

double min_distance_to_boundary(const gp_Pnt& point, const std::vector<EdgeSample>& boundarySamples) {
    double best = std::numeric_limits<double>::infinity();
    for (const auto& sample : boundarySamples) {
        best = std::min(best, point.Distance(sample.point));
    }
    return std::isfinite(best) ? best : 0.0;
}

void fill_trim_wire_stats(PatchTrimDiagnosticsReport& report, const TopoDS_Shape& shape) {
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const auto face = TopoDS::Face(explorer.Current());
        if (count_wires(face) == 0 || !BRepCheck_Analyzer(face).IsValid()) {
            ++report.trimWireInvalidCount;
        }
    }
}

void fill_over_cover(
    PatchTrimDiagnosticsReport& report,
    const std::vector<gp_Pnt>& replacementSamples,
    const Box3& candidateBox,
    double tolerance) {
    report.overCoverTotalSampleCount = static_cast<int>(replacementSamples.size());
    for (const auto& point : replacementSamples) {
        const auto distance = distance_outside_box(point, candidateBox);
        if (distance > tolerance) {
            ++report.overCoverSampleCount;
            report.overCoverMaxDistance = std::max(report.overCoverMaxDistance, distance);
        }
    }
    if (report.overCoverTotalSampleCount > 0) {
        report.overCoverRatio =
            static_cast<double>(report.overCoverSampleCount) /
            static_cast<double>(report.overCoverTotalSampleCount);
    }
}

void fill_under_cover(
    PatchTrimDiagnosticsReport& report,
    const std::vector<gp_Pnt>& candidateSamples,
    const TopoDS_Shape& replacementShape,
    const Box3& replacementBox,
    double tolerance) {
    report.underCoverTotalSampleCount = static_cast<int>(candidateSamples.size());
    for (const auto& point : candidateSamples) {
        const auto boxDistance = distance_outside_box(point, replacementBox);
        const auto distance = boxDistance > tolerance
            ? boxDistance
            : distance_to_shape(point, replacementShape);
        if (distance > tolerance) {
            ++report.underCoverSampleCount;
            if (std::isfinite(distance)) {
                report.underCoverMaxDistance = std::max(report.underCoverMaxDistance, distance);
            }
        }
    }
}

void fill_boundary_gap(
    PatchTrimDiagnosticsReport& report,
    const std::vector<EdgeSample>& boundarySamples,
    const TopoDS_Shape& replacementShape) {
    std::vector<double> distances;
    distances.reserve(boundarySamples.size());
    for (const auto& sample : boundarySamples) {
        const auto distance = distance_to_shape(sample.point, replacementShape);
        distances.push_back(std::isfinite(distance) ? distance : 0.0);
        if (std::isfinite(distance) && distance > report.boundaryGapMax) {
            report.boundaryGapMax = distance;
            report.worstBoundaryEdgeId = static_cast<int>(sample.edgeId);
        }
    }

    const auto stats = summarize(std::move(distances));
    report.boundaryGapMax = stats.max;
    report.boundaryGapP95 = stats.p95;
    report.boundaryGapRms = stats.rms;
}

void fill_internal_seams(
    PatchTrimDiagnosticsReport& report,
    const TopoDS_Shape& replacementShape,
    const Box3& candidateBox,
    const std::vector<EdgeSample>& boundarySamples,
    double tolerance) {
    const ShapeDocument replacementDocument(replacementShape, {});
    std::vector<std::pair<int, gp_Pnt>> internalMidpoints;
    for (EdgeId edgeId = 0; edgeId < replacementDocument.topology().edgeCount(); ++edgeId) {
        const auto* adjacency = replacementDocument.topology().adjacencyForEdge(edgeId);
        if (adjacency == nullptr || adjacency->faces.size() != 1) {
            continue;
        }
        const auto midpoint = edge_midpoint(replacementDocument.topology().edge(edgeId));
        if (!point_inside_box(midpoint, candidateBox, tolerance)) {
            continue;
        }
        if (min_distance_to_boundary(midpoint, boundarySamples) <= tolerance * 2.0) {
            continue;
        }
        internalMidpoints.emplace_back(static_cast<int>(edgeId), midpoint);
    }

    std::vector<double> gaps;
    gaps.reserve(internalMidpoints.size());
    for (std::size_t index = 0; index < internalMidpoints.size(); ++index) {
        double nearest = std::numeric_limits<double>::infinity();
        for (std::size_t other = 0; other < internalMidpoints.size(); ++other) {
            if (index == other) {
                continue;
            }
            nearest = std::min(nearest, internalMidpoints[index].second.Distance(internalMidpoints[other].second));
        }
        if (!std::isfinite(nearest)) {
            nearest = edge_length(replacementDocument.topology().edge(static_cast<EdgeId>(internalMidpoints[index].first)));
        }
        gaps.push_back(nearest);
        if (nearest > report.internalSeamGapMax) {
            report.internalSeamGapMax = nearest;
            report.worstInternalEdgeId = internalMidpoints[index].first;
        }
    }

    const auto stats = summarize(std::move(gaps));
    report.internalSeamGapMax = stats.max;
    report.internalSeamGapP95 = stats.p95;
    report.internalSeamGapRms = stats.rms;
}

bool shape_bbox_changed(const TopoDS_Shape& lhs, const TopoDS_Shape& rhs, double tolerance) {
    const auto leftSamples = sample_shape_faces(lhs, 3);
    const auto rightSamples = sample_shape_faces(rhs, 3);
    Box3 leftBox;
    Box3 rightBox;
    for (const auto& point : leftSamples) {
        include_point(leftBox, point);
    }
    for (const auto& point : rightSamples) {
        include_point(rightBox, point);
    }
    if (!leftBox.valid || !rightBox.valid) {
        return false;
    }
    return std::abs(leftBox.minX - rightBox.minX) > tolerance ||
        std::abs(leftBox.minY - rightBox.minY) > tolerance ||
        std::abs(leftBox.minZ - rightBox.minZ) > tolerance ||
        std::abs(leftBox.maxX - rightBox.maxX) > tolerance ||
        std::abs(leftBox.maxY - rightBox.maxY) > tolerance ||
        std::abs(leftBox.maxZ - rightBox.maxZ) > tolerance;
}

}

PatchTrimDiagnosticsReport PatchTrimDiagnostics::analyze(
    const PatchTrimDiagnosticsInput& input,
    const PatchTrimDiagnosticsOptions& options) const {
    PatchTrimDiagnosticsReport report;
    if (!valid_options(options) ||
        input.beforeDocument == nullptr ||
        !input.beforeDocument->hasShape() ||
        input.candidate == nullptr ||
        input.boundary == nullptr ||
        !input.boundary->valid ||
        input.replacementShape == nullptr ||
        input.replacementShape->IsNull()) {
        return report;
    }

    const auto boundaryEdges = boundary_edges_for(*input.boundary, *input.candidate);
    const auto boundarySamples = downsample_evenly(
        sample_boundary_edges(
            input.beforeDocument->topology(),
            boundaryEdges,
            options.boundarySamplesPerEdge),
        options.maxBoundarySamples);
    if (boundarySamples.empty()) {
        return report;
    }

    Box3 candidateBox;
    for (const auto& sample : boundarySamples) {
        include_point(candidateBox, sample.point);
    }

    report.captured = true;
    report.replacementFaceCount = count_shapes(*input.replacementShape, TopAbs_FACE);
    fill_trim_wire_stats(report, *input.replacementShape);

    const auto replacementSamples = downsample_evenly(
        sample_shape_faces(
            *input.replacementShape,
            options.surfaceGridDivisions),
        options.maxSurfaceSamples);
    Box3 replacementBox;
    for (const auto& sample : replacementSamples) {
        include_point(replacementBox, sample);
    }
    fill_over_cover(report, replacementSamples, candidateBox, options.distanceTolerance);

    const auto candidateSamples = downsample_evenly(
        sample_candidate_faces(
            *input.beforeDocument,
            *input.candidate,
            options.surfaceGridDivisions),
        options.maxSurfaceSamples);
    fill_under_cover(
        report,
        candidateSamples,
        *input.replacementShape,
        replacementBox,
        options.distanceTolerance);
    fill_boundary_gap(report, boundarySamples, *input.replacementShape);
    fill_internal_seams(
        report,
        *input.replacementShape,
        candidateBox,
        boundarySamples,
        options.distanceTolerance);

    if (input.roundtripShape != nullptr && !input.roundtripShape->IsNull()) {
        report.roundtripCompared = true;
        report.roundtripChanged = shape_bbox_changed(
            *input.replacementShape,
            *input.roundtripShape,
            options.distanceTolerance);
    }

    return report;
}

}
