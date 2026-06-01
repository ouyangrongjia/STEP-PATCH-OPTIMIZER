#include "stl/StlRegionExtractor.h"

#include <BRepBndLib.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <TopAbs_State.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
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

    const auto surfaceTolerance = std::max(report.margin, 1.0e-6);
    const auto boundaryTolerance = 1.0e-6;

    StlMesh localMesh;
    for (const auto& triangle : sourceMesh.triangles()) {
        if (intersects(triangle_bbox(triangle), report.expanded_bbox) &&
            point_inside_candidate_region(faceRegions, triangle_centroid(triangle), surfaceTolerance, boundaryTolerance)) {
            localMesh.addTriangle(triangle);
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

    report.success = true;

    StlRegionExtractResult result;
    result.success = true;
    result.localMesh = std::move(localMesh);
    result.report = std::move(report);
    return result;
}

}
