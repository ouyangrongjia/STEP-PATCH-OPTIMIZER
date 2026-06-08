#include "patch/CropBoundaryDiagnostics.h"

#include "brep/BoundaryWireBuilder.h"
#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "stl/StlMesh.h"

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
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

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

struct CandidateFaceRegion {
    FaceId faceId = 0;
    TopoDS_Face face;
    Handle(Geom_Surface) surface;
    StlBoundingBox bbox;
    gp_Pnt referencePoint;
    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
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
        options.patchBoundaryTolerance >= 0.0 &&
        options.boundaryBandOffset >= 0.0 &&
        options.boundaryBandCoverageTolerance >= 0.0 &&
        options.boundaryBandTriangleTolerance >= 0.0 &&
        options.cropBboxMarginRatio >= 0.0 &&
        options.cropMinMargin >= 0.0 &&
        options.maxTriangleDecisionRecords >= 0 &&
        options.maxRejectedTriangleOverlayCount >= 0;
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

CropBoundarySamplePoint make_sample_point(const gp_Pnt& point) {
    CropBoundarySamplePoint sample;
    sample.x = point.X();
    sample.y = point.Y();
    sample.z = point.Z();
    return sample;
}

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

gp_Pnt face_reference_point(const TopoDS_Face& face, const Handle(Geom_Surface)& surface) {
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(face, properties);
    if (properties.Mass() > 1.0e-12) {
        return properties.CentreOfMass();
    }

    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    if (!surface.IsNull() &&
        std::isfinite(uMin) &&
        std::isfinite(uMax) &&
        std::isfinite(vMin) &&
        std::isfinite(vMax)) {
        return surface->Value((uMin + uMax) * 0.5, (vMin + vMax) * 0.5);
    }
    return {};
}

bool make_face_region(const TopoDS_Face& face, FaceId faceId, CandidateFaceRegion& region) {
    region.faceId = faceId;
    region.face = face;
    region.surface = BRep_Tool::Surface(face);
    if (region.surface.IsNull()) {
        return false;
    }
    Bnd_Box faceBox;
    BRepBndLib::Add(face, faceBox);
    region.bbox = bbox_from_occt(faceBox);
    BRepTools::UVBounds(face, region.uMin, region.uMax, region.vMin, region.vMax);
    region.referencePoint = face_reference_point(face, region.surface);
    return
        finite_bbox(region.bbox) &&
        std::isfinite(region.uMin) &&
        std::isfinite(region.uMax) &&
        std::isfinite(region.vMin) &&
        std::isfinite(region.vMax) &&
        region.uMin <= region.uMax &&
        region.vMin <= region.vMax;
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

std::string point_candidate_reject_reason(
    const std::vector<CandidateFaceRegion>& regions,
    const gp_Pnt& point,
    double surfaceTolerance,
    double boundaryTolerance,
    const char* prefix) {
    bool sawCandidateBbox = false;
    bool sawProjectionFailure = false;
    bool sawDistanceTooLarge = false;
    bool sawClassifierOutside = false;

    for (const auto& region : regions) {
        if (!contains(region.bbox, point, surfaceTolerance)) {
            continue;
        }
        sawCandidateBbox = true;

        GeomAPI_ProjectPointOnSurf projector(
            point,
            region.surface,
            region.uMin,
            region.uMax,
            region.vMin,
            region.vMax,
            surfaceTolerance);
        if (!projector.IsDone() || projector.NbPoints() == 0) {
            sawProjectionFailure = true;
            continue;
        }

        for (int index = 1; index <= projector.NbPoints(); ++index) {
            if (projector.Distance(index) > surfaceTolerance) {
                sawDistanceTooLarge = true;
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
                return {};
            }
            sawClassifierOutside = true;
        }
    }

    std::string reason = prefix;
    if (!sawCandidateBbox) {
        reason += " outside candidate face bbox";
    } else if (sawClassifierOutside) {
        reason += " classifier outside";
    } else if (sawProjectionFailure) {
        reason += " projection failed";
    } else if (sawDistanceTooLarge) {
        reason += " projection distance over tolerance";
    } else {
        reason += " outside candidate";
    }
    return reason;
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

std::pair<double, EdgeId> distance_to_edge_polyline_with_edge(
    const gp_Pnt& point,
    const std::vector<CropBoundaryEdgeSample>& edgeSamples) {
    auto bestSquared = std::numeric_limits<double>::infinity();
    EdgeId bestEdgeId = -1;
    for (const auto& edge : edgeSamples) {
        if (edge.samples.empty()) {
            continue;
        }
        if (edge.samples.size() == 1) {
            const auto distance = point.SquareDistance(to_point(edge.samples.front()));
            if (distance < bestSquared) {
                bestSquared = distance;
                bestEdgeId = edge.edgeId;
            }
            continue;
        }
        for (std::size_t index = 1; index < edge.samples.size(); ++index) {
            const auto distance = squared_distance_point_segment(
                point,
                to_point(edge.samples[index - 1]),
                to_point(edge.samples[index]));
            if (distance < bestSquared) {
                bestSquared = distance;
                bestEdgeId = edge.edgeId;
            }
        }
    }
    return {std::sqrt(bestSquared), bestEdgeId};
}

double distance_to_edge_polyline(
    const gp_Pnt& point,
    const std::vector<CropBoundaryEdgeSample>& edgeSamples) {
    return distance_to_edge_polyline_with_edge(point, edgeSamples).first;
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

bool build_candidate_face_regions(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    std::vector<CandidateFaceRegion>& faceRegions,
    StlBoundingBox& candidateBbox) {
    const auto& topology = document.topology();
    Bnd_Box candidateBox;
    faceRegions.clear();
    faceRegions.reserve(candidate.faces.size());
    for (const auto faceId : candidate.faces) {
        if (faceId >= topology.faceCount()) {
            return false;
        }
        const auto& face = topology.face(faceId);
        BRepBndLib::Add(face, candidateBox);
        CandidateFaceRegion region;
        if (!make_face_region(face, faceId, region)) {
            return false;
        }
        faceRegions.push_back(std::move(region));
    }
    candidateBbox = bbox_from_occt(candidateBox);
    return finite_bbox(candidateBbox);
}

const CandidateFaceRegion* boundary_edge_candidate_region(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    EdgeId edgeId,
    const std::vector<CandidateFaceRegion>& faceRegions) {
    const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
    if (adjacency == nullptr) {
        return nullptr;
    }

    for (const auto faceId : adjacency->faces) {
        if (std::find(candidate.faces.begin(), candidate.faces.end(), faceId) == candidate.faces.end()) {
            continue;
        }
        const auto it = std::find_if(faceRegions.begin(), faceRegions.end(), [faceId](const auto& region) {
            return region.faceId == faceId;
        });
        if (it != faceRegions.end()) {
            return &(*it);
        }
    }
    return nullptr;
}

gp_Pnt offset_toward_reference(const gp_Pnt& point, const gp_Pnt& reference, double offset) {
    const auto dx = reference.X() - point.X();
    const auto dy = reference.Y() - point.Y();
    const auto dz = reference.Z() - point.Z();
    const auto length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length <= 1.0e-12 || offset <= 0.0) {
        return point;
    }
    const auto scale = offset / length;
    return gp_Pnt(
        point.X() + dx * scale,
        point.Y() + dy * scale,
        point.Z() + dz * scale);
}

std::vector<CropBoundaryEdgeSample> make_boundary_band_samples(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const std::vector<CandidateFaceRegion>& faceRegions,
    const std::vector<CropBoundaryEdgeSample>& originalEdges,
    const CropBoundaryDiagnosticsOptions& options,
    double surfaceTolerance,
    double boundaryTolerance) {
    std::vector<CropBoundaryEdgeSample> bandEdges;
    bandEdges.reserve(originalEdges.size());
    for (const auto& edge : originalEdges) {
        CropBoundaryEdgeSample bandEdge;
        bandEdge.edgeId = edge.edgeId;
        bandEdge.edgeLength = edge.edgeLength;
        bandEdge.singleClosedOuterLoop = edge.singleClosedOuterLoop;

        const auto* region = boundary_edge_candidate_region(document, candidate, edge.edgeId, faceRegions);
        if (region == nullptr) {
            bandEdges.push_back(std::move(bandEdge));
            continue;
        }

        const auto offset = std::min(options.boundaryBandOffset, std::max(0.0, edge.edgeLength * 0.45));
        bandEdge.samples.reserve(edge.samples.size());
        for (const auto& sample : edge.samples) {
            const auto boundaryPoint = to_point(sample);
            gp_Pnt bandPoint = boundaryPoint;
            for (const auto scale : {1.0, 0.5, 0.25}) {
                const auto candidatePoint = offset_toward_reference(boundaryPoint, region->referencePoint, offset * scale);
                if (point_inside_face_region(*region, candidatePoint, surfaceTolerance, boundaryTolerance) ||
                    point_inside_candidate_region(faceRegions, candidatePoint, surfaceTolerance, boundaryTolerance)) {
                    bandPoint = candidatePoint;
                    break;
                }
            }

            auto bandSample = make_sample_point(bandPoint);
            bandSample.parameter = sample.parameter;
            bandSample.normalizedParameter = sample.normalizedParameter;
            bandEdge.samples.push_back(bandSample);
        }
        bandEdge.sampleCount = static_cast<int>(bandEdge.samples.size());
        bandEdges.push_back(std::move(bandEdge));
    }
    return bandEdges;
}

void append_unique_edge_ids(std::vector<EdgeId>& target, const std::vector<CropBoundaryGapSegment>& segments) {
    for (const auto& segment : segments) {
        target.push_back(segment.edgeId);
    }
    std::sort(target.begin(), target.end());
    target.erase(std::unique(target.begin(), target.end()), target.end());
}

double distance_triangle_to_boundary(
    const StlTriangle& triangle,
    const TriangleDistanceData& triangleData,
    const std::vector<CropBoundaryEdgeSample>& originalEdges,
    EdgeId& nearestEdgeId) {
    double best = std::numeric_limits<double>::infinity();
    nearestEdgeId = -1;

    const auto updatePoint = [&](const gp_Pnt& point) {
        const auto [distance, edgeId] = distance_to_edge_polyline_with_edge(point, originalEdges);
        if (distance < best) {
            best = distance;
            nearestEdgeId = edgeId;
        }
    };

    updatePoint(to_point(triangle.v0));
    updatePoint(to_point(triangle.v1));
    updatePoint(to_point(triangle.v2));
    updatePoint(midpoint(triangle.v0, triangle.v1));
    updatePoint(midpoint(triangle.v1, triangle.v2));
    updatePoint(midpoint(triangle.v2, triangle.v0));
    updatePoint(triangle_centroid(triangle));

    for (const auto& edge : originalEdges) {
        for (const auto& sample : edge.samples) {
            const auto distance = std::sqrt(squared_distance_point_triangle(to_point(sample), triangleData));
            if (distance < best) {
                best = distance;
                nearestEdgeId = edge.edgeId;
            }
        }
    }

    return best;
}

void audit_source_triangles(
    CropBoundaryDiagnosticsReport& report,
    const StlMesh& sourceMesh,
    const std::vector<CandidateFaceRegion>& faceRegions,
    const StlBoundingBox& expandedCandidateBbox,
    const std::vector<CropBoundaryEdgeSample>& originalEdges,
    const CropBoundaryDiagnosticsOptions& options,
    double surfaceTolerance,
    double boundaryTolerance) {
    report.sourceTriangleAuditEvaluated = true;
    int triangleIndex = 0;
    for (const auto& triangle : sourceMesh.triangles()) {
        StlTriangleCropDecision decision;
        decision.triangleIndex = triangleIndex++;
        decision.v0 = make_sample_point(to_point(triangle.v0));
        decision.v1 = make_sample_point(to_point(triangle.v1));
        decision.v2 = make_sample_point(to_point(triangle.v2));
        const auto centroid = triangle_centroid(triangle);
        decision.centroid = make_sample_point(centroid);
        decision.bboxIntersectsExpandedCandidate = intersects(triangle_bbox(triangle), expandedCandidateBbox);
        if (!decision.bboxIntersectsExpandedCandidate) {
            decision.rejectReason = "outside expanded candidate bbox";
            continue;
        }

        ++report.sourceTriangleAuditCount;
        decision.centroidInsideCandidate = point_inside_candidate_region(
            faceRegions,
            centroid,
            surfaceTolerance,
            boundaryTolerance);
        decision.anyVertexInsideCandidate =
            point_inside_candidate_region(faceRegions, to_point(triangle.v0), surfaceTolerance, boundaryTolerance) ||
            point_inside_candidate_region(faceRegions, to_point(triangle.v1), surfaceTolerance, boundaryTolerance) ||
            point_inside_candidate_region(faceRegions, to_point(triangle.v2), surfaceTolerance, boundaryTolerance);
        decision.anyEdgeMidpointInsideCandidate =
            point_inside_candidate_region(faceRegions, midpoint(triangle.v0, triangle.v1), surfaceTolerance, boundaryTolerance) ||
            point_inside_candidate_region(faceRegions, midpoint(triangle.v1, triangle.v2), surfaceTolerance, boundaryTolerance) ||
            point_inside_candidate_region(faceRegions, midpoint(triangle.v2, triangle.v0), surfaceTolerance, boundaryTolerance);

        const auto triangleData = make_triangle_distance_data(triangle);
        EdgeId nearestEdgeId = -1;
        const auto boundaryDistance = distance_triangle_to_boundary(triangle, triangleData, originalEdges, nearestEdgeId);
        decision.nearestBoundaryEdgeId = nearestEdgeId;
        decision.nearOriginalBoundaryBand = boundaryDistance <= options.boundaryBandTriangleTolerance;
        decision.keptByCurrentExtractor = decision.centroidInsideCandidate;
        decision.shouldKeepConservative =
            decision.centroidInsideCandidate ||
            decision.anyVertexInsideCandidate ||
            decision.anyEdgeMidpointInsideCandidate ||
            decision.nearOriginalBoundaryBand;

        if (!decision.keptByCurrentExtractor) {
            decision.rejectReason = point_candidate_reject_reason(
                faceRegions,
                centroid,
                surfaceTolerance,
                boundaryTolerance,
                "centroid");
            if (decision.shouldKeepConservative) {
                decision.rejectReason += "; conservative criteria matched";
            }
        }

        if (!decision.keptByCurrentExtractor && decision.shouldKeepConservative) {
            ++report.conservativeKeepCandidateCount;
        }
        if (!decision.keptByCurrentExtractor && decision.nearOriginalBoundaryBand) {
            ++report.rejectedNearBoundaryTriangleCount;
            if (decision.nearestBoundaryEdgeId != static_cast<EdgeId>(-1)) {
                report.suspectedCropHoleEdgeIds.push_back(decision.nearestBoundaryEdgeId);
            }
            if (static_cast<int>(report.rejectedNearBoundaryTriangles.size()) < options.maxRejectedTriangleOverlayCount) {
                report.rejectedNearBoundaryTriangles.push_back(decision);
            }
        }
        if (static_cast<int>(report.triangleDecisions.size()) < options.maxTriangleDecisionRecords) {
            report.triangleDecisions.push_back(std::move(decision));
        }
    }
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
    report.boundaryBandCoverageTolerance = options.boundaryBandCoverageTolerance;
    report.boundaryBandOffset = options.boundaryBandOffset;
    report.boundaryBandTriangleTolerance = options.boundaryBandTriangleTolerance;
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

    std::vector<CandidateFaceRegion> faceRegions;
    StlBoundingBox candidateBbox;
    StlBoundingBox expandedCandidateBbox;
    double surfaceTolerance = 1.0e-6;
    const double boundaryTolerance = 1.0e-6;
    bool candidateRegionsReady = false;
    if (input.candidate == nullptr) {
        append_warning(report, "Candidate faces are missing; boundary-band and source triangle audit were not evaluated.");
    } else if (build_candidate_face_regions(*input.document, *input.candidate, faceRegions, candidateBbox)) {
        const auto margin = std::max(
            bbox_diagonal(candidateBbox) * options.cropBboxMarginRatio,
            options.cropMinMargin);
        expandedCandidateBbox = expand_bbox(candidateBbox, margin);
        surfaceTolerance = std::max(margin, 1.0e-6);
        candidateRegionsReady = true;
    } else {
        append_warning(report, "Candidate face regions could not be built; boundary-band and source triangle audit were not evaluated.");
    }

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

        if (candidateRegionsReady) {
            report.boundaryBandEdges = make_boundary_band_samples(
                *input.document,
                *input.candidate,
                faceRegions,
                report.originalBoundaryEdges,
                options,
                surfaceTolerance,
                boundaryTolerance);
            for (const auto& edge : report.boundaryBandEdges) {
                report.boundaryBandSampleCount += edge.sampleCount;
            }

            if (report.boundaryBandSampleCount > 0) {
                const auto bandSummary = summarize_distances(
                    report.boundaryBandEdges,
                    "Band",
                    options.boundaryBandCoverageTolerance,
                    [&triangles](const CropBoundarySamplePoint& sample) {
                        return distance_to_local_stl(sample, triangles);
                    });
                report.boundaryBandEvaluated = bandSummary.evaluated;
                report.boundaryBandMissingPointCount = bandSummary.missingPointCount;
                report.boundaryBandMinDistance = bandSummary.minDistance;
                report.boundaryBandMaxDistance = bandSummary.maxDistance;
                report.boundaryBandAverageDistance = bandSummary.averageDistance;
                report.boundaryBandMissingSegments = bandSummary.gapSegments;
                append_unique_edge_ids(report.boundaryBandMissingEdgeIds, report.boundaryBandMissingSegments);
                append_unique_edge_ids(report.suspectedCropHoleEdgeIds, report.boundaryBandMissingSegments);
                report.suspectedGapSegments.insert(
                    report.suspectedGapSegments.end(),
                    bandSummary.gapSegments.begin(),
                    bandSummary.gapSegments.end());
            }
        }
    }

    if (input.sourceStlMesh == nullptr || input.sourceStlMesh->empty()) {
        append_warning(report, "Source STL mesh is missing; source triangle rejection audit was not evaluated.");
    } else if (candidateRegionsReady) {
        audit_source_triangles(
            report,
            *input.sourceStlMesh,
            faceRegions,
            expandedCandidateBbox,
            report.originalBoundaryEdges,
            options,
            surfaceTolerance,
            boundaryTolerance);
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
    std::sort(report.suspectedCropHoleEdgeIds.begin(), report.suspectedCropHoleEdgeIds.end());
    report.suspectedCropHoleEdgeIds.erase(
        std::unique(report.suspectedCropHoleEdgeIds.begin(), report.suspectedCropHoleEdgeIds.end()),
        report.suspectedCropHoleEdgeIds.end());
    if (report.suspectedGapCount > 0) {
        append_warning(report, "Boundary coverage gaps were detected; inspect the diagnostic overlay before changing repair or sewing tolerance.");
    }
    if (report.boundaryBandMissingPointCount > 0) {
        append_warning(report, "Boundary-band STL coverage gaps were detected; this can be missed by boundary-point-only diagnostics.");
    }
    if (report.rejectedNearBoundaryTriangleCount > 0) {
        append_warning(report, "Source triangle audit found near-boundary triangles rejected by the current centroid-only crop predicate.");
    }

    report.success = report.originalBoundarySampled && report.patchBoundaryEvaluated;
    report.message = report.suspectedGapCount == 0 && report.rejectedNearBoundaryTriangleCount == 0
        ? "Crop boundary diagnostics found no STL coverage or patch boundary gaps over configured tolerances."
        : "Crop boundary diagnostics found suspected boundary gap or mismatch segments.";
    return report;
}

} // namespace spo
