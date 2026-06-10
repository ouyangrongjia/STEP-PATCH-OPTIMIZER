#include "stl/StlRegionExtractor.h"

#include <BRepBndLib.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_State.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include "merge/RegionBoundaryAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <string>
#include <utility>

namespace spo {

const char* toString(GeomagicFittingInputMode mode) {
    switch (mode) {
        case GeomagicFittingInputMode::LegacyStlCrop: return "legacy-stl-crop";
        case GeomagicFittingInputMode::ConservativeBoundaryBandStlCrop: return "conservative-boundary-band-stl-crop";
        case GeomagicFittingInputMode::StpSampledCandidateSurface: return "stp-sampled-candidate-surface";
    }
    return "unknown";
}

namespace {

void include_vertex(StlBoundingBox& bbox, const StlVec3& vertex) {
    bbox.min.x = std::min(bbox.min.x, vertex.x);
    bbox.min.y = std::min(bbox.min.y, vertex.y);
    bbox.min.z = std::min(bbox.min.z, vertex.z);
    bbox.max.x = std::max(bbox.max.x, vertex.x);
    bbox.max.y = std::max(bbox.max.y, vertex.y);
    bbox.max.z = std::max(bbox.max.z, vertex.z);
}

StlBoundingBox triangle_bbox(const StlTriangle& triangle) {
    StlBoundingBox bbox;
    bbox.valid = true;
    bbox.min = triangle.v0;
    bbox.max = triangle.v0;
    include_vertex(bbox, triangle.v1);
    include_vertex(bbox, triangle.v2);
    return bbox;
}

bool finite_bbox(const StlBoundingBox& bbox) {
    return bbox.valid &&
        std::isfinite(bbox.min.x) &&
        std::isfinite(bbox.min.y) &&
        std::isfinite(bbox.min.z) &&
        std::isfinite(bbox.max.x) &&
        std::isfinite(bbox.max.y) &&
        std::isfinite(bbox.max.z);
}

double bbox_diagonal(const StlBoundingBox& bbox) {
    const auto dx = bbox.max.x - bbox.min.x;
    const auto dy = bbox.max.y - bbox.min.y;
    const auto dz = bbox.max.z - bbox.min.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

StlBoundingBox expand_bbox(const StlBoundingBox& bbox, double margin) {
    StlBoundingBox expanded;
    expanded.valid = bbox.valid;
    expanded.min = {bbox.min.x - margin, bbox.min.y - margin, bbox.min.z - margin};
    expanded.max = {bbox.max.x + margin, bbox.max.y + margin, bbox.max.z + margin};
    return expanded;
}

bool intersects(const StlBoundingBox& lhs, const StlBoundingBox& rhs) {
    return lhs.valid && rhs.valid &&
        lhs.min.x <= rhs.max.x && lhs.max.x >= rhs.min.x &&
        lhs.min.y <= rhs.max.y && lhs.max.y >= rhs.min.y &&
        lhs.min.z <= rhs.max.z && lhs.max.z >= rhs.min.z;
}

bool contains(const StlBoundingBox& bbox, const gp_Pnt& point, double tolerance) {
    return bbox.valid &&
        point.X() >= bbox.min.x - tolerance && point.X() <= bbox.max.x + tolerance &&
        point.Y() >= bbox.min.y - tolerance && point.Y() <= bbox.max.y + tolerance &&
        point.Z() >= bbox.min.z - tolerance && point.Z() <= bbox.max.z + tolerance;
}

gp_Pnt triangle_centroid(const StlTriangle& triangle) {
    return gp_Pnt(
        (triangle.v0.x + triangle.v1.x + triangle.v2.x) / 3.0,
        (triangle.v0.y + triangle.v1.y + triangle.v2.y) / 3.0,
        (triangle.v0.z + triangle.v1.z + triangle.v2.z) / 3.0);
}

gp_Pnt midpoint(const StlVec3& lhs, const StlVec3& rhs) {
    return gp_Pnt(
        (lhs.x + rhs.x) * 0.5,
        (lhs.y + rhs.y) * 0.5,
        (lhs.z + rhs.z) * 0.5);
}

gp_Pnt to_point(const StlVec3& point) {
    return gp_Pnt(point.x, point.y, point.z);
}

double edge_length(const TopoDS_Edge& edge) {
    GProp_GProps properties;
    BRepGProp::LinearProperties(edge, properties);
    return std::max(0.0, properties.Mass());
}

StlRegionExtractResult fail(StlCropReport report, std::string message) {
    report.success = false;
    report.message = std::move(message);

    StlRegionExtractResult result;
    result.report = std::move(report);
    return result;
}

StlBoundingBox bbox_from_occt(const Bnd_Box& box) {
    StlBoundingBox bbox;
    if (box.IsVoid()) {
        return bbox;
    }

    double xMin = 0.0;
    double yMin = 0.0;
    double zMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    double zMax = 0.0;
    box.Get(xMin, yMin, zMin, xMax, yMax, zMax);

    bbox.valid = true;
    bbox.min = {xMin, yMin, zMin};
    bbox.max = {xMax, yMax, zMax};
    return bbox;
}

struct CandidateFaceRegion {
    TopoDS_Face face;
    Handle(Geom_Surface) surface;
    StlBoundingBox bbox;
    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
};

struct BoundarySegment {
    gp_Pnt start;
    gp_Pnt end;
};

struct BoundarySample {
    gp_Pnt point;
    EdgeId edgeId = -1;
};

struct TriangleDistanceData {
    gp_Pnt p0;
    gp_Pnt p1;
    gp_Pnt p2;
    StlBoundingBox bbox;
};

bool make_face_region(const TopoDS_Face& face, CandidateFaceRegion& region) {
    region.face = face;
    region.surface = BRep_Tool::Surface(face);
    if (region.surface.IsNull()) {
        return false;
    }
    Bnd_Box faceBox;
    BRepBndLib::Add(face, faceBox);
    region.bbox = bbox_from_occt(faceBox);
    BRepTools::UVBounds(face, region.uMin, region.uMax, region.vMin, region.vMax);
    return
        finite_bbox(region.bbox) &&
        std::isfinite(region.uMin) &&
        std::isfinite(region.uMax) &&
        std::isfinite(region.vMin) &&
        std::isfinite(region.vMax) &&
        region.uMin <= region.uMax &&
        region.vMin <= region.vMax;
}

std::vector<BoundarySegment> boundary_segments(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& boundary,
    double targetSpacing) {
    std::vector<BoundarySegment> segments;
    const auto& topology = document.topology();
    const auto spacing = std::max(targetSpacing, 1.0e-4);
    for (const auto edgeId : boundary.ordered_boundary_edges) {
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) {
            continue;
        }
        const auto& edge = topology.edge(edgeId);
        double firstParameter = 0.0;
        double lastParameter = 0.0;
        const auto curve = BRep_Tool::Curve(edge, firstParameter, lastParameter);
        if (curve.IsNull()) {
            continue;
        }

        const auto length = edge_length(edge);
        const auto count = std::clamp(static_cast<int>(std::ceil(length / spacing)) + 1, 2, 64);
        gp_Pnt previous;
        bool hasPrevious = false;
        for (int index = 0; index < count; ++index) {
            const double ratio = count == 1
                ? 0.0
                : static_cast<double>(index) / static_cast<double>(count - 1);
            const auto parameter = firstParameter + (lastParameter - firstParameter) * ratio;
            const auto point = curve->Value(parameter);
            if (hasPrevious && previous.SquareDistance(point) > 1.0e-18) {
                segments.push_back({previous, point});
            }
            previous = point;
            hasPrevious = true;
        }
    }
    return segments;
}

std::vector<BoundarySample> boundary_samples(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& boundary,
    int samplesPerEdge) {
    std::vector<BoundarySample> samples;
    const auto& topology = document.topology();
    const auto count = std::clamp(samplesPerEdge, 2, 64);
    for (const auto edgeId : boundary.ordered_boundary_edges) {
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) {
            continue;
        }
        const auto& edge = topology.edge(edgeId);
        double firstParameter = 0.0;
        double lastParameter = 0.0;
        const auto curve = BRep_Tool::Curve(edge, firstParameter, lastParameter);
        if (curve.IsNull()) {
            continue;
        }
        for (int index = 0; index < count; ++index) {
            const double ratio = count == 1
                ? 0.0
                : static_cast<double>(index) / static_cast<double>(count - 1);
            const auto parameter = firstParameter + (lastParameter - firstParameter) * ratio;
            samples.push_back({curve->Value(parameter), edgeId});
        }
    }
    return samples;
}

bool point_inside_face_region(
    const CandidateFaceRegion& region,
    const gp_Pnt& point,
    double surfaceTolerance,
    double boundaryTolerance) {
    if (!contains(region.bbox, point, surfaceTolerance)) {
        return false;
    }

    GeomAPI_ProjectPointOnSurf projector(
        point,
        region.surface,
        region.uMin,
        region.uMax,
        region.vMin,
        region.vMax,
        surfaceTolerance);
    if (!projector.IsDone() || projector.NbPoints() == 0) {
        return false;
    }

    for (int index = 1; index <= projector.NbPoints(); ++index) {
        if (projector.Distance(index) > surfaceTolerance) {
            continue;
        }

        double u = 0.0;
        double v = 0.0;
        projector.Parameters(index, u, v);
        const BRepClass_FaceClassifier classifier(
            region.face,
            gp_Pnt2d(u, v),
            boundaryTolerance,
            Standard_True);
        const auto state = classifier.State();
        if (state == TopAbs_IN || state == TopAbs_ON) {
            return true;
        }
    }

    return false;
}

bool point_inside_candidate_region(
    const std::vector<CandidateFaceRegion>& regions,
    const gp_Pnt& point,
    double surfaceTolerance,
    double boundaryTolerance) {
    for (const auto& region : regions) {
        if (point_inside_face_region(region, point, surfaceTolerance, boundaryTolerance)) {
            return true;
        }
    }
    return false;
}

bool any_vertex_inside_candidate_region(
    const std::vector<CandidateFaceRegion>& regions,
    const StlTriangle& triangle,
    double surfaceTolerance,
    double boundaryTolerance) {
    return
        point_inside_candidate_region(regions, to_point(triangle.v0), surfaceTolerance, boundaryTolerance) ||
        point_inside_candidate_region(regions, to_point(triangle.v1), surfaceTolerance, boundaryTolerance) ||
        point_inside_candidate_region(regions, to_point(triangle.v2), surfaceTolerance, boundaryTolerance);
}

bool any_edge_midpoint_inside_candidate_region(
    const std::vector<CandidateFaceRegion>& regions,
    const StlTriangle& triangle,
    double surfaceTolerance,
    double boundaryTolerance) {
    return
        point_inside_candidate_region(regions, midpoint(triangle.v0, triangle.v1), surfaceTolerance, boundaryTolerance) ||
        point_inside_candidate_region(regions, midpoint(triangle.v1, triangle.v2), surfaceTolerance, boundaryTolerance) ||
        point_inside_candidate_region(regions, midpoint(triangle.v2, triangle.v0), surfaceTolerance, boundaryTolerance);
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
        return point.SquareDistance(start);
    }

    const auto t = std::clamp((wx * vx + wy * vy + wz * vz) / lengthSquared, 0.0, 1.0);
    const gp_Pnt projection(
        start.X() + t * vx,
        start.Y() + t * vy,
        start.Z() + t * vz);
    return point.SquareDistance(projection);
}

double squared_distance_to_boundary_segments(
    const gp_Pnt& point,
    const std::vector<BoundarySegment>& segments) {
    auto best = std::numeric_limits<double>::infinity();
    for (const auto& segment : segments) {
        best = std::min(best, squared_distance_point_segment(point, segment.start, segment.end));
    }
    return best;
}

TriangleDistanceData make_triangle_distance_data(const StlTriangle& triangle) {
    TriangleDistanceData data;
    data.p0 = to_point(triangle.v0);
    data.p1 = to_point(triangle.v1);
    data.p2 = to_point(triangle.v2);
    data.bbox = triangle_bbox(triangle);
    return data;
}

double squared_distance_to_bbox(const gp_Pnt& point, const StlBoundingBox& bbox) {
    const auto axisDistance = [](double value, double minValue, double maxValue) {
        if (value < minValue) {
            return minValue - value;
        }
        if (value > maxValue) {
            return value - maxValue;
        }
        return 0.0;
    };

    const auto dx = axisDistance(point.X(), bbox.min.x, bbox.max.x);
    const auto dy = axisDistance(point.Y(), bbox.min.y, bbox.max.y);
    const auto dz = axisDistance(point.Z(), bbox.min.z, bbox.max.z);
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

double distance_to_kept_triangles(
    const gp_Pnt& point,
    const std::vector<TriangleDistanceData>& triangles,
    const std::vector<unsigned char>& kept) {
    auto bestSquared = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < triangles.size(); ++index) {
        if (kept[index] == 0) {
            continue;
        }
        if (squared_distance_to_bbox(point, triangles[index].bbox) > bestSquared) {
            continue;
        }
        bestSquared = std::min(bestSquared, squared_distance_point_triangle(point, triangles[index]));
    }
    return std::sqrt(bestSquared);
}

double squared_distance_vec3(const StlVec3& lhs, const StlVec3& rhs) {
    const auto dx = lhs.x - rhs.x;
    const auto dy = lhs.y - rhs.y;
    const auto dz = lhs.z - rhs.z;
    return dx * dx + dy * dy + dz * dz;
}

bool triangles_share_vertex(
    const StlTriangle& lhs,
    const StlTriangle& rhs,
    double tolerance) {
    const auto toleranceSquared = tolerance * tolerance;
    const StlVec3 lhsVertices[] = {lhs.v0, lhs.v1, lhs.v2};
    const StlVec3 rhsVertices[] = {rhs.v0, rhs.v1, rhs.v2};
    for (const auto& lhsVertex : lhsVertices) {
        for (const auto& rhsVertex : rhsVertices) {
            if (squared_distance_vec3(lhsVertex, rhsVertex) <= toleranceSquared) {
                return true;
            }
        }
    }
    return false;
}

bool connected_to_kept_triangles(
    std::size_t candidateIndex,
    const std::vector<StlTriangle>& triangles,
    const std::vector<unsigned char>& kept,
    double tolerance) {
    for (std::size_t index = 0; index < triangles.size(); ++index) {
        if (kept[index] == 0) {
            continue;
        }
        if (triangles_share_vertex(triangles[candidateIndex], triangles[index], tolerance)) {
            return true;
        }
    }
    return false;
}

struct BoundaryCoverageSummary {
    int sampleCount = 0;
    int missingPointCount = 0;
    double maxDistance = 0.0;
    double averageDistance = 0.0;
    std::vector<EdgeId> missingEdgeIds;
};

void append_unique_edge_id(std::vector<EdgeId>& edgeIds, EdgeId edgeId) {
    if (edgeId < 0) {
        return;
    }
    if (std::find(edgeIds.begin(), edgeIds.end(), edgeId) == edgeIds.end()) {
        edgeIds.push_back(edgeId);
    }
}

BoundaryCoverageSummary evaluate_boundary_loop_coverage(
    const std::vector<BoundarySample>& samples,
    const std::vector<TriangleDistanceData>& triangles,
    const std::vector<unsigned char>& kept,
    double tolerance) {
    BoundaryCoverageSummary summary;
    summary.sampleCount = static_cast<int>(samples.size());
    if (samples.empty()) {
        return summary;
    }

    double totalDistance = 0.0;
    for (const auto& sample : samples) {
        const auto distance = distance_to_kept_triangles(sample.point, triangles, kept);
        summary.maxDistance = std::max(summary.maxDistance, distance);
        totalDistance += distance;
        if (distance > tolerance) {
            ++summary.missingPointCount;
            append_unique_edge_id(summary.missingEdgeIds, sample.edgeId);
        }
    }
    summary.averageDistance = totalDistance / static_cast<double>(samples.size());
    return summary;
}

std::size_t closest_repair_triangle(
    const BoundarySample& sample,
    const std::vector<StlTriangle>& sourceTriangles,
    const std::vector<TriangleDistanceData>& triangles,
    const std::vector<unsigned char>& kept,
    const std::vector<unsigned char>& repairEligible,
    const StlBoundingBox& expandedBbox,
    double maxRepairDistance,
    double connectivityTolerance,
    int& orphanCandidateCount) {
    auto bestIndex = triangles.size();
    auto bestSquared = maxRepairDistance * maxRepairDistance;
    for (std::size_t index = 0; index < triangles.size(); ++index) {
        if (kept[index] != 0 || repairEligible[index] == 0 || !intersects(triangles[index].bbox, expandedBbox)) {
            continue;
        }
        if (!connected_to_kept_triangles(index, sourceTriangles, kept, connectivityTolerance)) {
            ++orphanCandidateCount;
            continue;
        }
        if (squared_distance_to_bbox(sample.point, triangles[index].bbox) > bestSquared) {
            continue;
        }
        const auto distanceSquared = squared_distance_point_triangle(sample.point, triangles[index]);
        if (distanceSquared < bestSquared) {
            bestSquared = distanceSquared;
            bestIndex = index;
        }
    }
    return bestIndex;
}

bool triangle_near_boundary_band(
    const StlTriangle& triangle,
    const std::vector<BoundarySegment>& segments,
    double tolerance) {
    if (segments.empty() || tolerance < 0.0) {
        return false;
    }

    const auto toleranceSquared = tolerance * tolerance;
    const auto nearPoint = [&](const gp_Pnt& point) {
        return squared_distance_to_boundary_segments(point, segments) <= toleranceSquared;
    };

    return
        nearPoint(to_point(triangle.v0)) ||
        nearPoint(to_point(triangle.v1)) ||
        nearPoint(to_point(triangle.v2)) ||
        nearPoint(midpoint(triangle.v0, triangle.v1)) ||
        nearPoint(midpoint(triangle.v1, triangle.v2)) ||
        nearPoint(midpoint(triangle.v2, triangle.v0)) ||
        nearPoint(triangle_centroid(triangle));
}

bool bbox_within(const StlBoundingBox& inner, const StlBoundingBox& outer, double tolerance) {
    return inner.valid && outer.valid &&
        inner.min.x >= outer.min.x - tolerance &&
        inner.min.y >= outer.min.y - tolerance &&
        inner.min.z >= outer.min.z - tolerance &&
        inner.max.x <= outer.max.x + tolerance &&
        inner.max.y <= outer.max.y + tolerance &&
        inner.max.z <= outer.max.z + tolerance;
}

}

StlRegionExtractResult StlRegionExtractor::extract(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const StlMesh& sourceMesh,
    const StlRegionExtractorOptions& options) const {
    StlCropReport report;
    report.candidate_id = candidate.candidate_id;
    report.source_triangle_count = static_cast<int>(sourceMesh.triangleCount());

    if (options.bboxMarginRatio < 0.0) {
        return fail(report, "bboxMarginRatio must not be negative.");
    }
    if (options.minMargin < 0.0) {
        return fail(report, "minMargin must not be negative.");
    }
    if (options.boundaryBandTolerance < 0.0) {
        return fail(report, "boundaryBandTolerance must not be negative.");
    }
    if (options.surfaceToleranceMultiplier <= 0.0) {
        return fail(report, "surfaceToleranceMultiplier must be positive.");
    }
    if (options.maxConservativeLeakRatio < 1.0) {
        return fail(report, "maxConservativeLeakRatio must be at least 1.0.");
    }
    if (options.boundaryLoopSamplesPerEdge < 2) {
        return fail(report, "boundaryLoopSamplesPerEdge must be at least 2.");
    }
    if (options.boundaryLoopCoverageTolerance < 0.0) {
        return fail(report, "boundaryLoopCoverageTolerance must not be negative.");
    }
    if (options.boundaryLoopConnectivityTolerance < 0.0) {
        return fail(report, "boundaryLoopConnectivityTolerance must not be negative.");
    }
    if (options.maxBoundaryLoopRepairTriangles < 0) {
        return fail(report, "maxBoundaryLoopRepairTriangles must not be negative.");
    }
    if (!document.hasShape()) {
        return fail(report, "STL region extraction requires a loaded shape.");
    }
    if (sourceMesh.empty()) {
        return fail(report, "Source STL mesh is empty.");
    }
    if (candidate.faces.empty()) {
        return fail(report, "Candidate has no faces.");
    }

    const auto& topology = document.topology();
    Bnd_Box candidateBox;
    std::vector<CandidateFaceRegion> faceRegions;
    faceRegions.reserve(candidate.faces.size());
    for (const auto faceId : candidate.faces) {
        if (faceId >= topology.faceCount()) {
            return fail(report, "Candidate references a missing face.");
        }
        const auto& face = topology.face(faceId);
        BRepBndLib::Add(face, candidateBox);
        CandidateFaceRegion region;
        if (!make_face_region(face, region)) {
            return fail(report, "Candidate face has invalid surface bounds.");
        }
        faceRegions.push_back(std::move(region));
    }

    report.candidate_bbox = bbox_from_occt(candidateBox);
    if (!finite_bbox(report.candidate_bbox)) {
        return fail(report, "Candidate bbox is invalid.");
    }

    report.margin = std::max(bbox_diagonal(report.candidate_bbox) * options.bboxMarginRatio, options.minMargin);
    report.expanded_bbox = expand_bbox(report.candidate_bbox, report.margin);

    const auto surfaceTolerance = std::max(report.margin * options.surfaceToleranceMultiplier, 1.0e-6);
    const auto boundaryTolerance = 1.0e-6;

    const bool conservativeCrop = options.mode == StlCropMode::ConservativeBoundaryBand;
    std::vector<BoundarySegment> boundaryBandSegments;
    std::vector<BoundarySample> cadBoundarySamples;
    const auto boundary = RegionBoundaryAnalyzer().analyze(document, candidate);
    if (options.repairBoundaryLoopCoverage) {
        report.boundary_loop_coverage_evaluated = true;
        report.boundary_loop_coverage_tolerance = options.boundaryLoopCoverageTolerance;
        if (boundary.valid && !boundary.ordered_boundary_edges.empty()) {
            cadBoundarySamples = boundary_samples(document, boundary, options.boundaryLoopSamplesPerEdge);
            report.boundary_loop_sample_count = static_cast<int>(cadBoundarySamples.size());
        } else {
            report.warning_message = "Candidate boundary analysis failed; boundary-loop coverage repair was skipped.";
        }
    }
    if (conservativeCrop && options.includeBoundaryBandTriangles) {
        if (boundary.valid && !boundary.ordered_boundary_edges.empty()) {
            boundaryBandSegments = boundary_segments(
                document,
                boundary,
                std::max(options.boundaryBandTolerance * 0.5, 0.05));
        } else {
            report.warning_message = "Candidate boundary analysis failed; boundary-band triangle inclusion was skipped.";
        }
    }

    const auto& sourceTriangles = sourceMesh.triangles();
    std::vector<unsigned char> kept(sourceTriangles.size(), 0);
    std::vector<unsigned char> rejectedReason(sourceTriangles.size(), 0);
    std::vector<unsigned char> boundaryRepairEligible(sourceTriangles.size(), 0);
    std::vector<TriangleDistanceData> triangleDistanceData;
    triangleDistanceData.reserve(sourceTriangles.size());
    for (const auto& triangle : sourceTriangles) {
        triangleDistanceData.push_back(make_triangle_distance_data(triangle));
    }

    const auto keepTriangle = [&](std::size_t index, StlMesh& localMesh) {
        if (kept[index] == 0) {
            kept[index] = 1;
            localMesh.addTriangle(sourceTriangles[index]);
        }
    };

    StlMesh localMesh;
    for (std::size_t triangleIndex = 0; triangleIndex < sourceTriangles.size(); ++triangleIndex) {
        const auto& triangle = sourceTriangles[triangleIndex];
        if (!intersects(triangle_bbox(triangle), report.expanded_bbox)) {
            ++report.rejected_outside_bbox_count;
            rejectedReason[triangleIndex] = 1;
            continue;
        }

        const auto centroidInside = point_inside_candidate_region(
            faceRegions,
            triangle_centroid(triangle),
            surfaceTolerance,
            boundaryTolerance);
        if (centroidInside) {
            ++report.centroid_keep_triangle_count;
            keepTriangle(triangleIndex, localMesh);
            continue;
        }

        const auto anyVertexInside = any_vertex_inside_candidate_region(
            faceRegions,
            triangle,
            surfaceTolerance,
            boundaryTolerance);
        const auto vertexInside = conservativeCrop &&
            options.includeVertexInsideTriangles &&
            anyVertexInside;
        if (vertexInside) {
            ++report.vertex_keep_triangle_count;
            ++report.conservative_keep_triangle_count;
            keepTriangle(triangleIndex, localMesh);
            continue;
        }

        const auto anyEdgeMidpointInside = any_edge_midpoint_inside_candidate_region(
            faceRegions,
            triangle,
            surfaceTolerance,
            boundaryTolerance);
        const auto edgeMidpointInside = conservativeCrop &&
            options.includeEdgeMidpointInsideTriangles &&
            anyEdgeMidpointInside;
        if (edgeMidpointInside) {
            ++report.edge_midpoint_keep_triangle_count;
            ++report.conservative_keep_triangle_count;
            keepTriangle(triangleIndex, localMesh);
            continue;
        }

        const auto nearBoundaryBand = conservativeCrop &&
            options.includeBoundaryBandTriangles &&
            triangle_near_boundary_band(triangle, boundaryBandSegments, options.boundaryBandTolerance);
        if (nearBoundaryBand) {
            ++report.boundary_band_keep_triangle_count;
            ++report.conservative_keep_triangle_count;
            keepTriangle(triangleIndex, localMesh);
            continue;
        }

        ++report.rejected_outside_candidate_count;
        rejectedReason[triangleIndex] = 2;
        boundaryRepairEligible[triangleIndex] = (anyVertexInside || anyEdgeMidpointInside) ? 1 : 0;
    }

    if (options.repairBoundaryLoopCoverage && !cadBoundarySamples.empty()) {
        const auto before = evaluate_boundary_loop_coverage(
            cadBoundarySamples,
            triangleDistanceData,
            kept,
            options.boundaryLoopCoverageTolerance);
        report.boundary_loop_missing_point_count_before = before.missingPointCount;
        report.boundary_loop_max_distance_before = before.maxDistance;
        report.boundary_loop_average_distance_before = before.averageDistance;
        report.boundary_loop_missing_edge_ids_before = before.missingEdgeIds;

        const auto maxRepairDistance = std::max(
            options.boundaryLoopCoverageTolerance * 3.0,
            report.margin + options.boundaryLoopCoverageTolerance);
        for (const auto& sample : cadBoundarySamples) {
            if (report.boundary_loop_repair_triangle_count >= options.maxBoundaryLoopRepairTriangles) {
                break;
            }
            const auto distance = distance_to_kept_triangles(sample.point, triangleDistanceData, kept);
            if (distance <= options.boundaryLoopCoverageTolerance) {
                continue;
            }
            const auto repairIndex = closest_repair_triangle(
                sample,
                sourceTriangles,
                triangleDistanceData,
                kept,
                boundaryRepairEligible,
                report.expanded_bbox,
                maxRepairDistance,
                options.boundaryLoopConnectivityTolerance,
                report.boundary_loop_orphan_repair_candidate_count);
            if (repairIndex >= sourceTriangles.size()) {
                continue;
            }
            if (rejectedReason[repairIndex] == 1 && report.rejected_outside_bbox_count > 0) {
                --report.rejected_outside_bbox_count;
            } else if (rejectedReason[repairIndex] == 2 && report.rejected_outside_candidate_count > 0) {
                --report.rejected_outside_candidate_count;
            }
            rejectedReason[repairIndex] = 0;
            keepTriangle(repairIndex, localMesh);
            ++report.boundary_loop_repair_triangle_count;
            report.boundary_loop_coverage_repair_applied = true;
        }

        const auto after = evaluate_boundary_loop_coverage(
            cadBoundarySamples,
            triangleDistanceData,
            kept,
            options.boundaryLoopCoverageTolerance);
        report.boundary_loop_missing_point_count_after = after.missingPointCount;
        report.boundary_loop_max_distance_after = after.maxDistance;
        report.boundary_loop_average_distance_after = after.averageDistance;
        report.boundary_loop_missing_edge_ids_after = after.missingEdgeIds;
        if (after.missingPointCount > 0) {
            if (!report.warning_message.empty()) {
                report.warning_message += " ";
            }
            report.warning_message += "Boundary-loop coverage repair could not cover all original CAD boundary samples.";
        }
    }

    report.output_triangle_count = static_cast<int>(localMesh.triangleCount());
    if (localMesh.empty()) {
        return fail(report, "STL crop produced no triangles.");
    }

    report.output_bbox = localMesh.boundingBox();
    if (!finite_bbox(report.output_bbox) || !intersects(report.output_bbox, report.expanded_bbox)) {
        return fail(report, "Output STL bbox is invalid.");
    }
    const auto leakTolerance = std::max(report.margin, options.boundaryBandTolerance);
    if (!bbox_within(report.output_bbox, report.expanded_bbox, leakTolerance)) {
        return fail(report, "Conservative STL crop exceeded expanded bbox leak guard.");
    }
    if (report.centroid_keep_triangle_count > 0) {
        const auto ratio = static_cast<double>(report.output_triangle_count) /
            static_cast<double>(report.centroid_keep_triangle_count);
        if (ratio > options.maxConservativeLeakRatio) {
            return fail(report, "Conservative STL crop exceeded triangle-count leak guard.");
        }
    }

    report.success = true;

    StlRegionExtractResult result;
    result.success = true;
    result.localMesh = std::move(localMesh);
    result.report = std::move(report);
    return result;
}

}
