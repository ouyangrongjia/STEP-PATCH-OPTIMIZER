#include "stl/StpSampledFittingMeshBuilder.h"

#include <BRepBndLib.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_UniformAbscissa.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <TopExp_Explorer.hxx>

#include "merge/RegionBoundaryAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace spo {

namespace {

struct FaceSurfaceInfo {
    TopoDS_Face face;
    Handle(Geom_Surface) surface;
    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    StlBoundingBox bbox;
    bool valid = false;
};

struct GridSample {
    int uIndex = 0;
    int vIndex = 0;
    gp_Pnt point;
    bool inside = true;
};

double edge_length(const TopoDS_Edge& edge) {
    GProp_GProps props;
    BRepGProp::LinearProperties(edge, props);
    return std::max(0.0, props.Mass());
}

double bbox_diagonal(const StlBoundingBox& bbox) {
    const auto dx = bbox.max.x - bbox.min.x;
    const auto dy = bbox.max.y - bbox.min.y;
    const auto dz = bbox.max.z - bbox.min.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

StlVec3 point_to_vec3(const gp_Pnt& p) {
    return {p.X(), p.Y(), p.Z()};
}

gp_Pnt vec3_to_point(const StlVec3& v) {
    return gp_Pnt(v.x, v.y, v.z);
}

StlBoundingBox bbox_from_occt(const Bnd_Box& box) {
    StlBoundingBox bbox;
    if (box.IsVoid()) {
        return bbox;
    }
    double xMin = 0.0, yMin = 0.0, zMin = 0.0;
    double xMax = 0.0, yMax = 0.0, zMax = 0.0;
    box.Get(xMin, yMin, zMin, xMax, yMax, zMax);
    bbox.valid = true;
    bbox.min = {xMin, yMin, zMin};
    bbox.max = {xMax, yMax, zMax};
    return bbox;
}

bool finite_bbox(const StlBoundingBox& bbox) {
    return bbox.valid &&
        std::isfinite(bbox.min.x) && std::isfinite(bbox.min.y) && std::isfinite(bbox.min.z) &&
        std::isfinite(bbox.max.x) && std::isfinite(bbox.max.y) && std::isfinite(bbox.max.z);
}

void include_vertex(StlBoundingBox& bbox, const StlVec3& v) {
    bbox.min.x = std::min(bbox.min.x, v.x);
    bbox.min.y = std::min(bbox.min.y, v.y);
    bbox.min.z = std::min(bbox.min.z, v.z);
    bbox.max.x = std::max(bbox.max.x, v.x);
    bbox.max.y = std::max(bbox.max.y, v.y);
    bbox.max.z = std::max(bbox.max.z, v.z);
}

double triangle_area(const StlTriangle& t) {
    const auto ax = t.v1.x - t.v0.x;
    const auto ay = t.v1.y - t.v0.y;
    const auto az = t.v1.z - t.v0.z;
    const auto bx = t.v2.x - t.v0.x;
    const auto by = t.v2.y - t.v0.y;
    const auto bz = t.v2.z - t.v0.z;
    const auto cx = ay * bz - az * by;
    const auto cy = az * bx - ax * bz;
    const auto cz = ax * by - ay * bx;
    return 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
}

FaceSurfaceInfo extract_face_info(const TopoDS_Face& face) {
    FaceSurfaceInfo info;
    info.face = face;
    info.surface = BRep_Tool::Surface(face);
    if (info.surface.IsNull()) {
        return info;
    }
    Bnd_Box faceBox;
    BRepBndLib::Add(face, faceBox);
    info.bbox = bbox_from_occt(faceBox);
    BRepTools::UVBounds(face, info.uMin, info.uMax, info.vMin, info.vMax);
    info.valid = finite_bbox(info.bbox) &&
        std::isfinite(info.uMin) && std::isfinite(info.uMax) &&
        std::isfinite(info.vMin) && std::isfinite(info.vMax) &&
        info.uMin <= info.uMax && info.vMin <= info.vMax;
    return info;
}

bool point_inside_face(const FaceSurfaceInfo& info, const gp_Pnt2d& uv, double tol) {
    BRepClass_FaceClassifier classifier(info.face, uv, tol, Standard_True);
    const auto state = classifier.State();
    return state == TopAbs_IN || state == TopAbs_ON;
}

std::vector<GridSample> sample_face_grid(
    const FaceSurfaceInfo& info,
    int divisionsU,
    int divisionsV) {
    std::vector<GridSample> samples;
    samples.reserve(static_cast<std::size_t>((divisionsU + 1) * (divisionsV + 1)));

    const auto du = (info.uMax - info.uMin) / static_cast<double>(divisionsU);
    const auto dv = (info.vMax - info.vMin) / static_cast<double>(divisionsV);

    for (int i = 0; i <= divisionsU; ++i) {
        const auto u = info.uMin + du * static_cast<double>(i);
        for (int j = 0; j <= divisionsV; ++j) {
            const auto v = info.vMin + dv * static_cast<double>(j);
            const auto pnt = info.surface->Value(u, v);
            GridSample sample;
            sample.uIndex = i;
            sample.vIndex = j;
            sample.point = pnt;
            sample.inside = point_inside_face(info, gp_Pnt2d(u, v), 1.0e-6);
            samples.push_back(sample);
        }
    }
    return samples;
}

struct BoundarySample {
    gp_Pnt point;
    int edgeId = -1;
    int faceIndex = -1;
};

std::vector<BoundarySample> sample_boundary_edges(
    const ShapeDocument& document,
    const std::vector<EdgeId>& orderedEdges,
    int minSamplesPerEdge,
    int maxSamplesPerEdge,
    double /*chordError*/,
    std::vector<int>& edgeSampleCounts) {
    std::vector<BoundarySample> samples;
    const auto& topology = document.topology();

    for (const auto edgeId : orderedEdges) {
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) {
            edgeSampleCounts.push_back(0);
            continue;
        }
        const auto& edge = topology.edge(edgeId);
        double firstParam = 0.0, lastParam = 0.0;
        const auto curve = BRep_Tool::Curve(edge, firstParam, lastParam);
        if (curve.IsNull()) {
            edgeSampleCounts.push_back(0);
            continue;
        }

        const auto length = edge_length(edge);
        // Use uniform arc length sampling for robustness
        auto numSamples = std::max(static_cast<int>(std::ceil(length / 0.25)), minSamplesPerEdge);
        numSamples = std::clamp(numSamples, minSamplesPerEdge, maxSamplesPerEdge);

        // Try uniform abscissa for better distribution on curved edges
        BRepAdaptor_Curve adaptor(edge);
        GCPnts_UniformAbscissa uniformSampler(adaptor, length / static_cast<double>(numSamples));
        if (uniformSampler.IsDone() && uniformSampler.NbPoints() > 0) {
            numSamples = std::clamp(uniformSampler.NbPoints(), minSamplesPerEdge, maxSamplesPerEdge);
            edgeSampleCounts.push_back(numSamples);
            for (int i = 1; i <= numSamples; ++i) {
                BoundarySample sample;
                sample.point = adaptor.Value(uniformSampler.Parameter(i));
                sample.edgeId = edgeId;
                samples.push_back(sample);
            }
        } else {
            edgeSampleCounts.push_back(numSamples);
            for (int i = 0; i < numSamples; ++i) {
                const auto ratio = numSamples <= 1
                    ? 0.0
                    : static_cast<double>(i) / static_cast<double>(numSamples - 1);
                const auto param = firstParam + (lastParam - firstParam) * ratio;
                BoundarySample sample;
                sample.point = curve->Value(param);
                sample.edgeId = edgeId;
                samples.push_back(sample);
            }
        }
    }
    return samples;
}

StlTriangle make_stl_triangle(
    const gp_Pnt& p0,
    const gp_Pnt& p1,
    const gp_Pnt& p2) {
    StlTriangle t;
    t.v0 = point_to_vec3(p0);
    t.v1 = point_to_vec3(p1);
    t.v2 = point_to_vec3(p2);
    t.normal = {0.0, 0.0, 1.0};
    return t;
}

bool is_degenerate(const StlTriangle& t, double minArea) {
    return triangle_area(t) < minArea;
}

double adaptive_grid_spacing(
    const StlBoundingBox& bbox,
    int maxDivisions,
    int maxTotalSamples,
    int faceCount) {
    const auto diagonal = bbox_diagonal(bbox);
    const auto baseSpacing = diagonal / static_cast<double>(std::max(maxDivisions, 1));
    const auto maxSamplesPerFace = std::max(100, maxTotalSamples / std::max(faceCount, 1));
    const auto targetDivisions = std::max(4, static_cast<int>(std::sqrt(static_cast<double>(maxSamplesPerFace))));
    return std::max(diagonal / static_cast<double>(targetDivisions), baseSpacing);
}

void triangulate_face_grid(
    const std::vector<GridSample>& grid,
    int divisionsU,
    int divisionsV,
    std::vector<StlTriangle>& outTriangles,
    int& interiorTriangleCount) {
    const auto cols = divisionsU + 1;
    for (int i = 0; i < divisionsU; ++i) {
        for (int j = 0; j < divisionsV; ++j) {
            const auto idx00 = static_cast<std::size_t>(i * cols + j);
            const auto idx10 = static_cast<std::size_t>((i + 1) * cols + j);
            const auto idx01 = static_cast<std::size_t>(i * cols + j + 1);
            const auto idx11 = static_cast<std::size_t>((i + 1) * cols + j + 1);

            if (idx00 >= grid.size() || idx10 >= grid.size() ||
                idx01 >= grid.size() || idx11 >= grid.size()) {
                continue;
            }

            const auto inside00 = grid[idx00].inside;
            const auto inside10 = grid[idx10].inside;
            const auto inside01 = grid[idx01].inside;
            const auto inside11 = grid[idx11].inside;

            // Triangle 1: (i,j) -> (i+1,j) -> (i,j+1)
            if (inside00 || inside10 || inside01) {
                auto tri = make_stl_triangle(
                    grid[idx00].point, grid[idx10].point, grid[idx01].point);
                if (!is_degenerate(tri, 1.0e-15)) {
                    outTriangles.push_back(tri);
                    ++interiorTriangleCount;
                }
            }

            // Triangle 2: (i+1,j) -> (i+1,j+1) -> (i,j+1)
            if (inside10 || inside11 || inside01) {
                auto tri = make_stl_triangle(
                    grid[idx10].point, grid[idx11].point, grid[idx01].point);
                if (!is_degenerate(tri, 1.0e-15)) {
                    outTriangles.push_back(tri);
                    ++interiorTriangleCount;
                }
            }
        }
    }
}

StpSampledFittingReport fail_report(
    StpSampledFittingReport report,
    std::string message) {
    report.success = false;
    report.message = std::move(message);
    return report;
}

}

StpSampledFittingReport StpSampledFittingMeshBuilder::build(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const StpSampledFittingOptions& options,
    StlMesh& outMesh) const {
    StpSampledFittingReport report;
    report.candidateId = candidate.candidate_id;
    report.sourceFaceCount = static_cast<int>(candidate.faces.size());

    // Validate inputs
    if (!document.hasShape()) {
        return fail_report(report, "No shape document loaded.");
    }
    if (candidate.faces.empty()) {
        return fail_report(report, "Candidate has no source faces.");
    }

    // Extract face surface info
    const auto& topology = document.topology();
    std::vector<FaceSurfaceInfo> faceInfos;
    faceInfos.reserve(candidate.faces.size());
    for (const auto faceId : candidate.faces) {
        if (faceId >= topology.faceCount()) {
            return fail_report(report, "Candidate references a missing face.");
        }
        const auto& face = topology.face(faceId);
        auto info = extract_face_info(face);
        if (!info.valid) {
            return fail_report(report, "Candidate face has invalid surface bounds.");
        }
        faceInfos.push_back(std::move(info));
    }

    // Analyze boundary (for reporting and validation only)
    auto boundary = RegionBoundaryAnalyzer().analyze(document, candidate);
    if (!boundary.valid || boundary.ordered_boundary_edges.empty()) {
        return fail_report(report, "Candidate boundary analysis failed; outer boundary is not a closed loop.");
    }
    if (!boundary.boundary_closed) {
        return fail_report(report, "Candidate boundary is not closed; cannot generate fitting STL.");
    }

    report.boundaryEdgeCount = static_cast<int>(boundary.ordered_boundary_edges.size());

    // Compute adaptive grid spacing
    StlBoundingBox candidateBBox;
    candidateBBox.valid = true;
    for (const auto& info : faceInfos) {
        include_vertex(candidateBBox, info.bbox.min);
        include_vertex(candidateBBox, info.bbox.max);
    }
    report.bbox = candidateBBox;

    const auto diagonal = bbox_diagonal(candidateBBox);
    // Base spacing from max divisions, but also cap by max total samples
    const auto maxSamplesPerFace = std::max(100, options.maxTotalSamples / std::max(static_cast<int>(faceInfos.size()), 1));
    const auto targetDivs = std::max(8, static_cast<int>(std::sqrt(static_cast<double>(maxSamplesPerFace))));
    auto divisionsPerFace = std::min(targetDivs, options.maxInteriorDivisions);
    divisionsPerFace = std::clamp(divisionsPerFace, 8, options.maxInteriorDivisions);
    report.samplingSpacing = diagonal / static_cast<double>(divisionsPerFace);
    report.boundarySpacing = report.samplingSpacing * 0.5;

    // Sample boundary edges for report statistics only
    std::vector<int> edgeSampleCounts;
    auto boundarySamples = sample_boundary_edges(
        document,
        boundary.ordered_boundary_edges,
        options.minBoundarySamplesPerEdge,
        options.boundarySamplesPerEdge,
        options.chordError,
        edgeSampleCounts);
    report.boundarySampleCount = static_cast<int>(boundarySamples.size());

    // Sample each face interior on uniform UV grid
    int totalInteriorSamples = 0;
    std::vector<std::vector<GridSample>> allFaceGrids;
    allFaceGrids.reserve(faceInfos.size());

    for (const auto& info : faceInfos) {
        auto grid = sample_face_grid(info, divisionsPerFace, divisionsPerFace);
        totalInteriorSamples += static_cast<int>(grid.size());
        allFaceGrids.push_back(std::move(grid));
    }
    report.interiorSampleCount = totalInteriorSamples;

    // Triangulate each face uniformly — no boundary band, no bridge triangles
    std::vector<StlTriangle> allTriangles;
    for (std::size_t fi = 0; fi < allFaceGrids.size(); ++fi) {
        int faceTriCount = 0;
        triangulate_face_grid(
            allFaceGrids[fi],
            divisionsPerFace,
            divisionsPerFace,
            allTriangles,
            faceTriCount);
    }

    // Build output mesh
    outMesh.clear();
    for (const auto& tri : allTriangles) {
        if (!is_degenerate(tri, 1.0e-18)) {
            outMesh.addTriangle(tri);
        }
    }

    report.outputTriangleCount = static_cast<int>(outMesh.triangleCount());
    report.bandRingCount = 0;
    report.boundaryBandSampleCount = 0;

    if (outMesh.empty()) {
        return fail_report(report, "STP-sampled fitting mesh has no valid triangles.");
    }

    report.output_bbox = outMesh.boundingBox();
    if (!finite_bbox(report.output_bbox)) {
        return fail_report(report, "Output mesh bounding box is invalid.");
    }

    if (totalInteriorSamples > options.maxTotalSamples) {
        report.warningMessage = "Interior sample count exceeds recommended maximum.";
    }

    report.success = true;
    report.message = "STP-sampled fitting mesh generated.";
    return report;
}

}
