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
    if (conservativeCrop && options.includeBoundaryBandTriangles) {
        const auto boundary = RegionBoundaryAnalyzer().analyze(document, candidate);
        if (boundary.valid && !boundary.ordered_boundary_edges.empty()) {
            boundaryBandSegments = boundary_segments(
                document,
                boundary,
                std::max(options.boundaryBandTolerance * 0.5, 0.05));
        } else {
            report.warning_message = "Candidate boundary analysis failed; boundary-band triangle inclusion was skipped.";
        }
    }

    StlMesh localMesh;
    for (const auto& triangle : sourceMesh.triangles()) {
        if (!intersects(triangle_bbox(triangle), report.expanded_bbox)) {
            ++report.rejected_outside_bbox_count;
            continue;
        }

        const auto centroidInside = point_inside_candidate_region(
            faceRegions,
            triangle_centroid(triangle),
            surfaceTolerance,
            boundaryTolerance);
        if (centroidInside) {
            ++report.centroid_keep_triangle_count;
            localMesh.addTriangle(triangle);
            continue;
        }

        const auto vertexInside = conservativeCrop &&
            options.includeVertexInsideTriangles &&
            any_vertex_inside_candidate_region(faceRegions, triangle, surfaceTolerance, boundaryTolerance);
        if (vertexInside) {
            ++report.vertex_keep_triangle_count;
            ++report.conservative_keep_triangle_count;
            localMesh.addTriangle(triangle);
            continue;
        }

        const auto edgeMidpointInside = conservativeCrop &&
            options.includeEdgeMidpointInsideTriangles &&
            any_edge_midpoint_inside_candidate_region(faceRegions, triangle, surfaceTolerance, boundaryTolerance);
        if (edgeMidpointInside) {
            ++report.edge_midpoint_keep_triangle_count;
            ++report.conservative_keep_triangle_count;
            localMesh.addTriangle(triangle);
            continue;
        }

        const auto nearBoundaryBand = conservativeCrop &&
            options.includeBoundaryBandTriangles &&
            triangle_near_boundary_band(triangle, boundaryBandSegments, options.boundaryBandTolerance);
        if (nearBoundaryBand) {
            ++report.boundary_band_keep_triangle_count;
            ++report.conservative_keep_triangle_count;
            localMesh.addTriangle(triangle);
            continue;
        }

        ++report.rejected_outside_candidate_count;
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
