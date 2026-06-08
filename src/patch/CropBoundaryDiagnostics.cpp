#include "patch/CropBoundaryDiagnostics.h"

#include "brep/BoundaryWireBuilder.h"
#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "stl/StlMesh.h"

#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace spo {

namespace {

struct TriangleDistanceData {
    gp_Pnt p0;
    gp_Pnt p1;
    gp_Pnt p2;
    double minX = 0.0;
    double minY = 0.0;
    double minZ = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    double maxZ = 0.0;
};

struct DistanceSummary {
    bool evaluated = false;
    int missingPointCount = 0;
    double minDistance = 0.0;
    double maxDistance = 0.0;
    double averageDistance = 0.0;
    std::vector<std::vector<double>> distancesByEdge;
    std::vector<CropBoundaryGapSegment> gapSegments;
};

CropBoundaryDiagnosticsReport fail(std::string message) {
    CropBoundaryDiagnosticsReport report;
    report.message = std::move(message);
    return report;
}

void append_warning(CropBoundaryDiagnosticsReport& report, const std::string& warning) {
    if (warning.empty()) {
        return;
    }
    if (!report.warningMessage.empty()) {
        report.warningMessage += " ";
    }
    report.warningMessage += warning;
}

bool valid_options(const CropBoundaryDiagnosticsOptions& options) {
    return
        options.minSamplesPerEdge >= 2 &&
        options.maxSamplesPerEdge >= options.minSamplesPerEdge &&
        options.targetSampleSpacing > 0.0 &&
        options.stlCoverageTolerance >= 0.0 &&
        options.patchBoundaryTolerance >= 0.0;
}

double edge_length(const TopoDS_Edge& edge) {
    GProp_GProps properties;
    BRepGProp::LinearProperties(edge, properties);
    return std::max(0.0, properties.Mass());
}

int sample_count_for_edge(double length, const CropBoundaryDiagnosticsOptions& options) {
    const auto bySpacing = static_cast<int>(std::ceil(length / options.targetSampleSpacing)) + 1;
    return std::clamp(bySpacing, options.minSamplesPerEdge, options.maxSamplesPerEdge);
}

CropBoundarySamplePoint make_sample_point(
    const gp_Pnt& point,
    double parameter,
    double firstParameter,
    double lastParameter) {
    CropBoundarySamplePoint sample;
    sample.x = point.X();
    sample.y = point.Y();
    sample.z = point.Z();
    sample.parameter = parameter;
    const auto range = lastParameter - firstParameter;
    sample.normalizedParameter = std::abs(range) <= 1.0e-12
        ? 0.0
        : (parameter - firstParameter) / range;
    return sample;
}

CropBoundaryEdgeSample sample_edge(
    const TopoDS_Edge& edge,
    EdgeId edgeId,
    bool singleClosedOuterLoop,
    const CropBoundaryDiagnosticsOptions& options) {
    CropBoundaryEdgeSample edgeSample;
    edgeSample.edgeId = edgeId;
    edgeSample.singleClosedOuterLoop = singleClosedOuterLoop;
    edgeSample.edgeLength = edge_length(edge);

    double firstParameter = 0.0;
    double lastParameter = 0.0;
    const auto curve = BRep_Tool::Curve(edge, firstParameter, lastParameter);
    if (curve.IsNull()) {
        return edgeSample;
    }

    edgeSample.sampleCount = sample_count_for_edge(edgeSample.edgeLength, options);
    edgeSample.samples.reserve(static_cast<std::size_t>(edgeSample.sampleCount));
    for (int index = 0; index < edgeSample.sampleCount; ++index) {
        const double ratio = edgeSample.sampleCount == 1
            ? 0.0
            : static_cast<double>(index) / static_cast<double>(edgeSample.sampleCount - 1);
        const auto parameter = firstParameter + (lastParameter - firstParameter) * ratio;
        edgeSample.samples.push_back(make_sample_point(
            curve->Value(parameter),
            parameter,
            firstParameter,
            lastParameter));
    }

    return edgeSample;
}

std::vector<CropBoundarySamplePoint> all_original_samples(
    const std::vector<CropBoundaryEdgeSample>& edgeSamples) {
    std::vector<CropBoundarySamplePoint> samples;
    for (const auto& edge : edgeSamples) {
        samples.insert(samples.end(), edge.samples.begin(), edge.samples.end());
    }
    return samples;
}

gp_Pnt to_point(const CropBoundarySamplePoint& point) {
    return gp_Pnt(point.x, point.y, point.z);
}

gp_Pnt to_point(const StlVec3& point) {
    return gp_Pnt(point.x, point.y, point.z);
}

TriangleDistanceData make_triangle_distance_data(const StlTriangle& triangle) {
    TriangleDistanceData data;
    data.p0 = to_point(triangle.v0);
    data.p1 = to_point(triangle.v1);
    data.p2 = to_point(triangle.v2);
    data.minX = std::min({triangle.v0.x, triangle.v1.x, triangle.v2.x});
    data.minY = std::min({triangle.v0.y, triangle.v1.y, triangle.v2.y});
    data.minZ = std::min({triangle.v0.z, triangle.v1.z, triangle.v2.z});
    data.maxX = std::max({triangle.v0.x, triangle.v1.x, triangle.v2.x});
    data.maxY = std::max({triangle.v0.y, triangle.v1.y, triangle.v2.y});
    data.maxZ = std::max({triangle.v0.z, triangle.v1.z, triangle.v2.z});
    return data;
}

double squared_distance_to_bbox(const gp_Pnt& point, const TriangleDistanceData& triangle) {
    const auto axisDistance = [](double value, double minValue, double maxValue) {
        if (value < minValue) {
            return minValue - value;
        }
        if (value > maxValue) {
            return value - maxValue;
        }
        return 0.0;
    };

    const auto dx = axisDistance(point.X(), triangle.minX, triangle.maxX);
    const auto dy = axisDistance(point.Y(), triangle.minY, triangle.maxY);
    const auto dz = axisDistance(point.Z(), triangle.minZ, triangle.maxZ);
    return dx * dx + dy * dy + dz * dz;
}

double squared_distance_point_segment(const gp_Pnt& point, const gp_Pnt& start, const gp_Pnt& end) {
    const auto vx = end.X() - start.X();
    const auto vy = end.Y() - start.Y();
    const auto vz = end.Z() - start.Z();
    const auto wx = point.X() - start.X();
    const auto wy = point.Y() - start.Y();
    const auto wz = point.Z() - start.Z();
    const auto lengthSquared = vx * vx + vy * vy + vz * vz;
    if (lengthSquared <= 1.0e-24) {
        const auto dx = point.X() - start.X();
        const auto dy = point.Y() - start.Y();
        const auto dz = point.Z() - start.Z();
        return dx * dx + dy * dy + dz * dz;
    }

    const auto t = std::clamp((wx * vx + wy * vy + wz * vz) / lengthSquared, 0.0, 1.0);
    const gp_Pnt projection(
        start.X() + t * vx,
        start.Y() + t * vy,
        start.Z() + t * vz);
    const auto dx = point.X() - projection.X();
    const auto dy = point.Y() - projection.Y();
    const auto dz = point.Z() - projection.Z();
    return dx * dx + dy * dy + dz * dz;
}

double squared_distance_point_triangle(const gp_Pnt& point, const TriangleDistanceData& triangle) {
    const auto ax = triangle.p0.X();
    const auto ay = triangle.p0.Y();
    const auto az = triangle.p0.Z();
    const auto bx = triangle.p1.X();
    const auto by = triangle.p1.Y();
    const auto bz = triangle.p1.Z();
    const auto cx = triangle.p2.X();
    const auto cy = triangle.p2.Y();
    const auto cz = triangle.p2.Z();

    const auto abx = bx - ax;
    const auto aby = by - ay;
    const auto abz = bz - az;
    const auto acx = cx - ax;
    const auto acy = cy - ay;
    const auto acz = cz - az;
    const auto apx = point.X() - ax;
    const auto apy = point.Y() - ay;
    const auto apz = point.Z() - az;

    const auto d1 = abx * apx + aby * apy + abz * apz;
    const auto d2 = acx * apx + acy * apy + acz * apz;
    if (d1 <= 0.0 && d2 <= 0.0) {
        return point.SquareDistance(triangle.p0);
    }

    const auto bpx = point.X() - bx;
    const auto bpy = point.Y() - by;
    const auto bpz = point.Z() - bz;
    const auto d3 = abx * bpx + aby * bpy + abz * bpz;
    const auto d4 = acx * bpx + acy * bpy + acz * bpz;
    if (d3 >= 0.0 && d4 <= d3) {
        return point.SquareDistance(triangle.p1);
    }

    const auto vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const auto v = d1 / (d1 - d3);
        const gp_Pnt projection(ax + v * abx, ay + v * aby, az + v * abz);
        return point.SquareDistance(projection);
    }

    const auto cpx = point.X() - cx;
    const auto cpy = point.Y() - cy;
    const auto cpz = point.Z() - cz;
    const auto d5 = abx * cpx + aby * cpy + abz * cpz;
    const auto d6 = acx * cpx + acy * cpy + acz * cpz;
    if (d6 >= 0.0 && d5 <= d6) {
        return point.SquareDistance(triangle.p2);
    }

    const auto vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const auto w = d2 / (d2 - d6);
        const gp_Pnt projection(ax + w * acx, ay + w * acy, az + w * acz);
        return point.SquareDistance(projection);
    }

    const auto bcx = cx - bx;
    const auto bcy = cy - by;
    const auto bcz = cz - bz;
    const auto va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const auto w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        const gp_Pnt projection(bx + w * bcx, by + w * bcy, bz + w * bcz);
        return point.SquareDistance(projection);
    }

    const auto denom = 1.0 / (va + vb + vc);
    const auto v = vb * denom;
    const auto w = vc * denom;
    const gp_Pnt projection(
        ax + abx * v + acx * w,
        ay + aby * v + acy * w,
        az + abz * v + acz * w);
    return point.SquareDistance(projection);
}

double distance_to_local_stl(const CropBoundarySamplePoint& sample, const std::vector<TriangleDistanceData>& triangles) {
    const auto point = to_point(sample);
    auto bestSquared = std::numeric_limits<double>::infinity();
    for (const auto& triangle : triangles) {
        if (squared_distance_to_bbox(point, triangle) > bestSquared) {
            continue;
        }
        bestSquared = std::min(bestSquared, squared_distance_point_triangle(point, triangle));
    }
    return std::sqrt(bestSquared);
}

double distance_to_edge_polyline(
    const CropBoundarySamplePoint& sample,
    const std::vector<CropBoundaryEdgeSample>& edgeSamples) {
    const auto point = to_point(sample);
    auto bestSquared = std::numeric_limits<double>::infinity();
    for (const auto& edge : edgeSamples) {
        if (edge.samples.empty()) {
            continue;
        }
        if (edge.samples.size() == 1) {
            bestSquared = std::min(bestSquared, point.SquareDistance(to_point(edge.samples.front())));
            continue;
        }
        for (std::size_t index = 1; index < edge.samples.size(); ++index) {
            bestSquared = std::min(bestSquared, squared_distance_point_segment(
                point,
                to_point(edge.samples[index - 1]),
                to_point(edge.samples[index])));
        }
    }
    return std::sqrt(bestSquared);
}

std::vector<CropBoundarySamplePoint> segment_samples_for_overlay(
    const CropBoundaryEdgeSample& edge,
    int startIndex,
    int endIndex) {
    std::vector<CropBoundarySamplePoint> samples;
    if (edge.samples.empty()) {
        return samples;
    }

    const auto clampedStart = std::clamp(startIndex, 0, static_cast<int>(edge.samples.size()) - 1);
    const auto clampedEnd = std::clamp(endIndex, clampedStart, static_cast<int>(edge.samples.size()) - 1);
    samples.insert(
        samples.end(),
        edge.samples.begin() + clampedStart,
        edge.samples.begin() + clampedEnd + 1);

    if (samples.size() == 1 && clampedStart > 0) {
        samples.insert(samples.begin(), edge.samples[static_cast<std::size_t>(clampedStart - 1)]);
    }
    if (samples.size() == 1 && clampedEnd + 1 < static_cast<int>(edge.samples.size())) {
        samples.push_back(edge.samples[static_cast<std::size_t>(clampedEnd + 1)]);
    }
    return samples;
}

void append_gap_segments(
    DistanceSummary& summary,
    const std::vector<CropBoundaryEdgeSample>& originalEdges,
    const std::string& source,
    double tolerance) {
    for (std::size_t edgeIndex = 0; edgeIndex < originalEdges.size(); ++edgeIndex) {
        const auto& edge = originalEdges[edgeIndex];
        const auto& distances = summary.distancesByEdge[edgeIndex];
        int index = 0;
        while (index < static_cast<int>(distances.size())) {
            if (distances[static_cast<std::size_t>(index)] <= tolerance) {
                ++index;
                continue;
            }

            const auto start = index;
            auto maxDistance = distances[static_cast<std::size_t>(index)];
            while (index + 1 < static_cast<int>(distances.size()) &&
                   distances[static_cast<std::size_t>(index + 1)] > tolerance) {
                ++index;
                maxDistance = std::max(maxDistance, distances[static_cast<std::size_t>(index)]);
            }
            const auto end = index;

            CropBoundaryGapSegment segment;
            segment.source = source;
            segment.edgeId = edge.edgeId;
            segment.startSampleIndex = start;
            segment.endSampleIndex = end;
            segment.startParameter = edge.samples[static_cast<std::size_t>(start)].parameter;
            segment.endParameter = edge.samples[static_cast<std::size_t>(end)].parameter;
            segment.startNormalizedParameter = edge.samples[static_cast<std::size_t>(start)].normalizedParameter;
            segment.endNormalizedParameter = edge.samples[static_cast<std::size_t>(end)].normalizedParameter;
            segment.maxDistance = maxDistance;
            segment.samples = segment_samples_for_overlay(edge, start, end);
            summary.gapSegments.push_back(std::move(segment));
            ++index;
        }
    }
}

DistanceSummary summarize_distances(
    const std::vector<CropBoundaryEdgeSample>& originalEdges,
    const std::string& source,
    double tolerance,
    const std::function<double(const CropBoundarySamplePoint&)>& distanceFunction) {
    DistanceSummary summary;
    summary.evaluated = true;
    summary.minDistance = std::numeric_limits<double>::infinity();

    double totalDistance = 0.0;
    int totalSamples = 0;
    summary.distancesByEdge.reserve(originalEdges.size());
    for (const auto& edge : originalEdges) {
        std::vector<double> edgeDistances;
        edgeDistances.reserve(edge.samples.size());
        for (const auto& sample : edge.samples) {
            const auto distance = distanceFunction(sample);
            edgeDistances.push_back(distance);
            summary.minDistance = std::min(summary.minDistance, distance);
            summary.maxDistance = std::max(summary.maxDistance, distance);
            totalDistance += distance;
            ++totalSamples;
            if (distance > tolerance) {
                ++summary.missingPointCount;
            }
        }
        summary.distancesByEdge.push_back(std::move(edgeDistances));
    }

    if (totalSamples == 0) {
        summary.minDistance = 0.0;
        summary.maxDistance = 0.0;
        summary.averageDistance = 0.0;
        return summary;
    }

    summary.averageDistance = totalDistance / static_cast<double>(totalSamples);
    append_gap_segments(summary, originalEdges, source, tolerance);
    return summary;
}

std::vector<TriangleDistanceData> make_triangle_distance_data(const StlMesh& mesh) {
    std::vector<TriangleDistanceData> data;
    data.reserve(mesh.triangleCount());
    for (const auto& triangle : mesh.triangles()) {
        data.push_back(make_triangle_distance_data(triangle));
    }
    return data;
}

std::vector<TopoDS_Edge> patch_outer_edges(const TopoDS_Shape& patchShape) {
    std::vector<TopoDS_Edge> outerEdges;
    if (patchShape.IsNull()) {
        return outerEdges;
    }

    TopTools_IndexedDataMapOfShapeListOfShape edgeToFaces;
    TopExp::MapShapesAndAncestors(patchShape, TopAbs_EDGE, TopAbs_FACE, edgeToFaces);
    outerEdges.reserve(static_cast<std::size_t>(edgeToFaces.Extent()));
    for (int index = 1; index <= edgeToFaces.Extent(); ++index) {
        if (edgeToFaces.FindFromIndex(index).Extent() <= 1) {
            outerEdges.push_back(TopoDS::Edge(edgeToFaces.FindKey(index)));
        }
    }
    return outerEdges;
}

void add_suspected_gap_ids(CropBoundaryDiagnosticsReport& report) {
    for (const auto& segment : report.suspectedGapSegments) {
        report.suspectedGapEdgeIds.push_back(segment.edgeId);
    }
    std::sort(report.suspectedGapEdgeIds.begin(), report.suspectedGapEdgeIds.end());
    report.suspectedGapEdgeIds.erase(
        std::unique(report.suspectedGapEdgeIds.begin(), report.suspectedGapEdgeIds.end()),
        report.suspectedGapEdgeIds.end());
    report.suspectedGapCount = static_cast<int>(report.suspectedGapSegments.size());
}

bool boundary_is_single_closed_outer_loop(const RegionBoundaryAnalysis& boundary) {
    return boundary.valid &&
        boundary.connected_component_count == 1 &&
        boundary.outer_wire_count == 1 &&
        boundary.inner_wire_count == 0 &&
        boundary.boundary_closed &&
        !boundary.has_holes &&
        !boundary.has_non_manifold_edges &&
        !boundary.has_branching_boundary &&
        !boundary.ordered_boundary_edges.empty();
}

}

CropBoundaryDiagnosticsReport CropBoundaryDiagnostics::analyze(
    const CropBoundaryDiagnosticsInput& input,
    const CropBoundaryDiagnosticsOptions& options) const {
    if (!valid_options(options)) {
        return fail("Crop boundary diagnostics options are invalid.");
    }
    if (input.document == nullptr || !input.document->hasShape()) {
        return fail("Crop boundary diagnostics requires a loaded document.");
    }
    if (input.boundary == nullptr) {
        return fail("Crop boundary diagnostics requires boundary analysis.");
    }

    CropBoundaryDiagnosticsReport report;
    report.stlCoverageTolerance = options.stlCoverageTolerance;
    report.patchBoundaryTolerance = options.patchBoundaryTolerance;
    report.singleClosedOuterLoop = boundary_is_single_closed_outer_loop(*input.boundary);
    if (!report.singleClosedOuterLoop) {
        report.message = "Crop boundary diagnostics requires one closed original CAD outer boundary loop.";
        return report;
    }

    const auto wire = BoundaryWireBuilder().buildOuterWire(*input.document, *input.boundary);
    if (!wire.success) {
        report.message = wire.message;
        return report;
    }

    const auto& topology = input.document->topology();
    report.originalBoundaryEdges.reserve(input.boundary->ordered_boundary_edges.size());
    for (const auto edgeId : input.boundary->ordered_boundary_edges) {
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) {
            report.message = "Crop boundary diagnostics boundary analysis references a missing edge.";
            return report;
        }
        auto edgeSample = sample_edge(topology.edge(edgeId), edgeId, true, options);
        if (edgeSample.samples.empty()) {
            report.message = "Crop boundary diagnostics could not sample one original boundary edge.";
            return report;
        }
        report.originalBoundarySampleCount += edgeSample.sampleCount;
        report.originalBoundaryEdges.push_back(std::move(edgeSample));
    }
    report.originalBoundarySampled = report.originalBoundarySampleCount > 0;

    if (input.localStlMesh == nullptr || input.localStlMesh->empty()) {
        append_warning(report, "Local STL crop mesh is missing; STL coverage distances were not evaluated.");
    } else {
        const auto triangles = make_triangle_distance_data(*input.localStlMesh);
        const auto stlSummary = summarize_distances(
            report.originalBoundaryEdges,
            "STL",
            options.stlCoverageTolerance,
            [&triangles](const CropBoundarySamplePoint& sample) {
                return distance_to_local_stl(sample, triangles);
            });
        report.stlCoverageEvaluated = stlSummary.evaluated;
        report.stlCoverageMissingPointCount = stlSummary.missingPointCount;
        report.stlCoverageMinDistance = stlSummary.minDistance;
        report.stlCoverageMaxDistance = stlSummary.maxDistance;
        report.stlCoverageAverageDistance = stlSummary.averageDistance;
        report.suspectedGapSegments.insert(
            report.suspectedGapSegments.end(),
            stlSummary.gapSegments.begin(),
            stlSummary.gapSegments.end());
    }

    if (input.importedPatchShape == nullptr || input.importedPatchShape->IsNull()) {
        report.message = "Crop boundary diagnostics requires an imported patch shape.";
        return report;
    }

    const auto patchEdges = patch_outer_edges(*input.importedPatchShape);
    report.patchOuterEdgeCount = static_cast<int>(patchEdges.size());
    if (patchEdges.empty()) {
        report.message = "Imported patch contains no outer edges for boundary diagnostics.";
        return report;
    }

    report.patchOuterEdges.reserve(patchEdges.size());
    for (std::size_t index = 0; index < patchEdges.size(); ++index) {
        auto edgeSample = sample_edge(
            patchEdges[index],
            static_cast<EdgeId>(index),
            false,
            options);
        if (!edgeSample.samples.empty()) {
            report.patchOuterEdges.push_back(std::move(edgeSample));
        }
    }
    if (report.patchOuterEdges.empty()) {
        report.message = "Imported patch outer edges could not be sampled.";
        return report;
    }

    const auto patchSummary = summarize_distances(
        report.originalBoundaryEdges,
        "Patch",
        options.patchBoundaryTolerance,
        [&report](const CropBoundarySamplePoint& sample) {
            return distance_to_edge_polyline(sample, report.patchOuterEdges);
        });
    report.patchBoundaryEvaluated = patchSummary.evaluated;
    report.patchBoundaryMissingPointCount = patchSummary.missingPointCount;
    report.patchBoundaryMinDistance = patchSummary.minDistance;
    report.patchBoundaryMaxDistance = patchSummary.maxDistance;
    report.patchBoundaryAverageDistance = patchSummary.averageDistance;
    report.suspectedGapSegments.insert(
        report.suspectedGapSegments.end(),
        patchSummary.gapSegments.begin(),
        patchSummary.gapSegments.end());

    add_suspected_gap_ids(report);
    if (report.suspectedGapCount > 0) {
        append_warning(report, "Boundary coverage gaps were detected; inspect the diagnostic overlay before changing repair or sewing tolerance.");
    }

    report.success = report.originalBoundarySampled && report.patchBoundaryEvaluated;
    report.message = report.suspectedGapCount == 0
        ? "Crop boundary diagnostics found no STL coverage or patch boundary gaps over configured tolerances."
        : "Crop boundary diagnostics found suspected boundary gap or mismatch segments.";
    return report;
}

} // namespace spo
