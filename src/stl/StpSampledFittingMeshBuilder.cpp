#include "stl/StpSampledFittingMeshBuilder.h"

#include <BRepBndLib.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_UniformAbscissa.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>
#include <TopExp_Explorer.hxx>

#include "merge/RegionBoundaryAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
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
    double parameter = 0.0;
    int edgeId = -1;
    int faceIndex = -1;
};

struct GuardBandBuildResult {
    int edgeCount = 0;
    int sampleCount = 0;
    int triangleCount = 0;
    int adjacentFaceSampleCount = 0;
    int fallbackSampleCount = 0;
};

struct SurfaceParamScale {
    double u = 1.0;
    double v = 1.0;
};

double squared_distance(const gp_Pnt& lhs, const gp_Pnt& rhs) {
    return lhs.SquareDistance(rhs);
}

gp_Pnt bbox_center(const StlBoundingBox& bbox) {
    return gp_Pnt(
        (bbox.min.x + bbox.max.x) * 0.5,
        (bbox.min.y + bbox.max.y) * 0.5,
        (bbox.min.z + bbox.max.z) * 0.5);
}

double clamp_range(double value, double first, double last) {
    if (first <= last) {
        return std::clamp(value, first, last);
    }
    return std::clamp(value, last, first);
}

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
                sample.parameter = uniformSampler.Parameter(i);
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
                sample.parameter = param;
                sample.edgeId = edgeId;
                samples.push_back(sample);
            }
        }
    }
    return samples;
}

std::vector<gp_Pnt> collect_unique_edge_endpoints(
    const ShapeDocument& document,
    const std::vector<EdgeId>& orderedEdges,
    double tolerance) {
    std::vector<gp_Pnt> points;
    const auto toleranceSquared = tolerance * tolerance;
    const auto& topology = document.topology();

    auto add_unique = [&](const gp_Pnt& point) {
        for (const auto& existing : points) {
            if (squared_distance(existing, point) <= toleranceSquared) {
                return;
            }
        }
        points.push_back(point);
    };

    for (const auto edgeId : orderedEdges) {
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) {
            continue;
        }
        const auto& edge = topology.edge(edgeId);
        double firstParam = 0.0, lastParam = 0.0;
        const auto curve = BRep_Tool::Curve(edge, firstParam, lastParam);
        if (curve.IsNull()) {
            continue;
        }
        add_unique(curve->Value(firstParam));
        add_unique(curve->Value(lastParam));
    }

    return points;
}

std::string append_warning(std::string warning, const std::string& addition) {
    if (addition.empty()) {
        return warning;
    }
    if (!warning.empty()) {
        warning += " ";
    }
    warning += addition;
    return warning;
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

bool face_uv_inside(const TopoDS_Face& face, const gp_Pnt2d& uv) {
    BRepClass_FaceClassifier classifier(face, uv, 1.0e-6, Standard_True);
    const auto state = classifier.State();
    return state == TopAbs_IN || state == TopAbs_ON;
}

SurfaceParamScale estimate_surface_param_scale(
    const Handle(Geom_Surface)& surface,
    double uMin,
    double uMax,
    double vMin,
    double vMax) {
    SurfaceParamScale scale;
    if (surface.IsNull()) {
        return scale;
    }

    const auto uSpan = std::abs(uMax - uMin);
    const auto vSpan = std::abs(vMax - vMin);
    const auto uMid = (uMin + uMax) * 0.5;
    const auto vMid = (vMin + vMax) * 0.5;

    if (uSpan > 1.0e-12) {
        scale.u = std::max(surface->Value(uMin, vMid).Distance(surface->Value(uMax, vMid)) / uSpan, 1.0e-9);
    }
    if (vSpan > 1.0e-12) {
        scale.v = std::max(surface->Value(uMid, vMin).Distance(surface->Value(uMid, vMax)) / vSpan, 1.0e-9);
    }
    return scale;
}

gp_Pnt radial_guard_point(
    const gp_Pnt& basePoint,
    const gp_Pnt& candidateCenter,
    double distance) {
    gp_Vec direction(candidateCenter, basePoint);
    if (direction.Magnitude() <= 1.0e-12) {
        direction = gp_Vec(1.0, 0.0, 0.0);
    }
    direction.Normalize();
    return basePoint.Translated(direction.Multiplied(distance));
}

std::optional<gp_Pnt> adjacent_face_guard_point(
    const TopoDS_Edge& edge,
    const TopoDS_Face& face,
    const BoundarySample& sample,
    const gp_Pnt& candidateCenter,
    int ring,
    double spacing) {
    const auto surface = BRep_Tool::Surface(face);
    if (surface.IsNull() || ring <= 0 || spacing <= 0.0) {
        return std::nullopt;
    }

    double uMin = 0.0, uMax = 0.0, vMin = 0.0, vMax = 0.0;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    if (!std::isfinite(uMin) || !std::isfinite(uMax) ||
        !std::isfinite(vMin) || !std::isfinite(vMax)) {
        return std::nullopt;
    }

    double firstParam = 0.0, lastParam = 0.0;
    const auto pcurve = BRep_Tool::CurveOnSurface(edge, face, firstParam, lastParam);
    if (pcurve.IsNull()) {
        return std::nullopt;
    }

    const auto uv = pcurve->Value(clamp_range(sample.parameter, firstParam, lastParam));
    const auto scale = estimate_surface_param_scale(surface, uMin, uMax, vMin, vMax);
    const auto uStep = spacing / std::max(scale.u, 1.0e-9);
    const auto vStep = spacing / std::max(scale.v, 1.0e-9);
    if (!std::isfinite(uStep) || !std::isfinite(vStep) || uStep <= 0.0 || vStep <= 0.0) {
        return std::nullopt;
    }

    constexpr std::array<std::array<double, 2>, 8> directions {{
        {{ 1.0,  0.0}},
        {{-1.0,  0.0}},
        {{ 0.0,  1.0}},
        {{ 0.0, -1.0}},
        {{ 1.0,  1.0}},
        {{ 1.0, -1.0}},
        {{-1.0,  1.0}},
        {{-1.0, -1.0}}
    }};

    bool found = false;
    gp_Pnt2d bestUv = uv;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (const auto& direction : directions) {
        const gp_Pnt2d probe(
            uv.X() + uStep * direction[0],
            uv.Y() + vStep * direction[1]);
        if (!face_uv_inside(face, probe)) {
            continue;
        }

        const auto point = surface->Value(probe.X(), probe.Y());
        const auto score = point.Distance(candidateCenter);
        if (!found || score > bestScore) {
            found = true;
            bestScore = score;
            bestUv = probe;
        }
    }

    if (!found) {
        return std::nullopt;
    }

    const auto du = (bestUv.X() - uv.X()) * static_cast<double>(ring);
    const auto dv = (bestUv.Y() - uv.Y()) * static_cast<double>(ring);
    const gp_Pnt2d targetUv(uv.X() + du, uv.Y() + dv);
    return surface->Value(targetUv.X(), targetUv.Y());
}

const gp_Pnt& nearest_support_point(
    const gp_Pnt& point,
    const std::vector<gp_Pnt>& supportPoints) {
    auto best = supportPoints.begin();
    auto bestDistance = std::numeric_limits<double>::infinity();
    for (auto it = supportPoints.begin(); it != supportPoints.end(); ++it) {
        const auto distance = squared_distance(point, *it);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = it;
        }
    }
    return *best;
}

int append_quad_strip(
    const std::vector<gp_Pnt>& inner,
    const std::vector<gp_Pnt>& outer,
    std::vector<StlTriangle>& outTriangles) {
    const auto count = std::min(inner.size(), outer.size());
    if (count < 2) {
        return 0;
    }

    int added = 0;
    for (std::size_t index = 0; index + 1 < count; ++index) {
        auto first = make_stl_triangle(inner[index], inner[index + 1], outer[index]);
        if (!is_degenerate(first, 1.0e-15)) {
            outTriangles.push_back(first);
            ++added;
        }

        auto second = make_stl_triangle(inner[index + 1], outer[index + 1], outer[index]);
        if (!is_degenerate(second, 1.0e-15)) {
            outTriangles.push_back(second);
            ++added;
        }
    }
    return added;
}

std::optional<FaceId> guard_face_for_edge(
    const TopologyGraph& topology,
    EdgeId edgeId,
    const std::unordered_set<FaceId>& candidateFaces) {
    const auto* adjacency = topology.adjacencyForEdge(edgeId);
    if (adjacency == nullptr) {
        return std::nullopt;
    }
    for (const auto faceId : adjacency->faces) {
        if (candidateFaces.find(faceId) == candidateFaces.end()) {
            return faceId;
        }
    }
    return std::nullopt;
}

GuardBandBuildResult append_boundary_guard_band(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionBoundaryAnalysis& boundary,
    const StpSampledFittingOptions& options,
    const StlBoundingBox& candidateBBox,
    const std::vector<gp_Pnt>& supportPoints,
    std::vector<StlTriangle>& outTriangles) {
    GuardBandBuildResult result;
    if (!options.enableBoundaryGuardBandSampling ||
        options.boundaryGuardBandRingCount <= 0 ||
        options.boundaryGuardBandSpacing <= 0.0 ||
        supportPoints.empty()) {
        return result;
    }

    const auto& topology = document.topology();
    std::unordered_set<FaceId> candidateFaces(candidate.faces.begin(), candidate.faces.end());
    const auto center = bbox_center(candidateBBox);
    const auto samplesPerEdge = std::max(options.boundaryGuardBandSamplesPerEdge, options.minBoundarySamplesPerEdge);
    const auto ringCount = std::max(options.boundaryGuardBandRingCount, 1);

    for (const auto edgeId : boundary.ordered_boundary_edges) {
        if (edgeId >= topology.edgeCount()) {
            continue;
        }

        std::vector<int> edgeSampleCounts;
        const auto edgeSamples = sample_boundary_edges(
            document,
            {edgeId},
            samplesPerEdge,
            samplesPerEdge,
            options.chordError,
            edgeSampleCounts);
        if (edgeSamples.size() < 2) {
            continue;
        }

        const auto guardFaceId = guard_face_for_edge(topology, edgeId, candidateFaces);
        const auto& edge = topology.edge(edgeId);
        const TopoDS_Face* guardFace = nullptr;
        if (guardFaceId.has_value() && *guardFaceId < topology.faceCount()) {
            guardFace = &topology.face(*guardFaceId);
        }

        std::vector<gp_Pnt> supportRing;
        std::vector<gp_Pnt> boundaryRing;
        std::vector<std::vector<gp_Pnt>> guardRings(static_cast<std::size_t>(ringCount));
        supportRing.reserve(edgeSamples.size());
        boundaryRing.reserve(edgeSamples.size());
        for (auto& ring : guardRings) {
            ring.reserve(edgeSamples.size());
        }

        for (const auto& sample : edgeSamples) {
            supportRing.push_back(nearest_support_point(sample.point, supportPoints));
            boundaryRing.push_back(sample.point);

            for (int ring = 1; ring <= ringCount; ++ring) {
                std::optional<gp_Pnt> guardPoint;
                if (guardFace != nullptr) {
                    guardPoint = adjacent_face_guard_point(
                        edge,
                        *guardFace,
                        sample,
                        center,
                        ring,
                        options.boundaryGuardBandSpacing);
                }

                if (guardPoint.has_value()) {
                    ++result.adjacentFaceSampleCount;
                    guardRings[static_cast<std::size_t>(ring - 1)].push_back(*guardPoint);
                } else {
                    ++result.fallbackSampleCount;
                    guardRings[static_cast<std::size_t>(ring - 1)].push_back(
                        radial_guard_point(
                            sample.point,
                            center,
                            options.boundaryGuardBandSpacing * static_cast<double>(ring)));
                }
                ++result.sampleCount;
            }
        }

        auto added = append_quad_strip(supportRing, boundaryRing, outTriangles);
        if (!guardRings.empty()) {
            added += append_quad_strip(boundaryRing, guardRings.front(), outTriangles);
            for (std::size_t ring = 1; ring < guardRings.size(); ++ring) {
                added += append_quad_strip(guardRings[ring - 1], guardRings[ring], outTriangles);
            }
        }

        if (added > 0) {
            ++result.edgeCount;
            result.triangleCount += added;
        }
    }

    return result;
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
    report.cornerFeatureDenseSamplingEnabled = options.enableCornerFeatureDenseSampling;
    report.boundaryGuardBandSamplingEnabled = options.enableBoundaryGuardBandSampling;
    if (options.enableBoundaryGuardBandSampling) {
        report.boundaryGuardBandRingCount = std::max(options.boundaryGuardBandRingCount, 0);
        report.boundaryGuardBandSpacing = options.boundaryGuardBandSpacing;
    }

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
    if (options.enableCornerFeatureDenseSampling) {
        const auto requestedFeatureDivisions = std::max(
            divisionsPerFace + 4,
            std::max(8, options.cornerFeatureSamplesPerEdge / 2));
        const auto featureDivisionCap = std::max(options.maxInteriorDivisions, 28);
        divisionsPerFace = std::clamp(
            requestedFeatureDivisions,
            divisionsPerFace,
            featureDivisionCap);
        report.cornerFeatureSurfaceDivisionCount = divisionsPerFace;
    }
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
    std::vector<gp_Pnt> supportPoints;

    for (const auto& info : faceInfos) {
        auto grid = sample_face_grid(info, divisionsPerFace, divisionsPerFace);
        totalInteriorSamples += static_cast<int>(grid.size());
        for (const auto& sample : grid) {
            if (sample.inside) {
                supportPoints.push_back(sample.point);
            }
        }
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

    if (options.enableBoundaryGuardBandSampling) {
        const auto guardBand = append_boundary_guard_band(
            document,
            candidate,
            boundary,
            options,
            candidateBBox,
            supportPoints,
            allTriangles);
        report.boundaryGuardBandEdgeCount = guardBand.edgeCount;
        report.boundaryGuardBandSampleCount = guardBand.sampleCount;
        report.boundaryGuardBandTriangleCount = guardBand.triangleCount;
        report.boundaryGuardBandAdjacentFaceSampleCount = guardBand.adjacentFaceSampleCount;
        report.boundaryGuardBandFallbackSampleCount = guardBand.fallbackSampleCount;
        report.bandRingCount = report.boundaryGuardBandRingCount;
        report.boundaryBandSampleCount = report.boundaryGuardBandSampleCount;
        if (guardBand.triangleCount == 0) {
            report.warningMessage = append_warning(
                report.warningMessage,
                "B2 boundary guard-band sampling was enabled but no guard-band triangles were generated.");
        }
    }

    if (options.enableCornerFeatureDenseSampling) {
        std::vector<int> denseEdgeSampleCounts;
        const auto denseSamplesPerEdge = std::max(
            options.cornerFeatureSamplesPerEdge,
            options.minBoundarySamplesPerEdge);
        const auto denseSamples = sample_boundary_edges(
            document,
            boundary.ordered_boundary_edges,
            denseSamplesPerEdge,
            denseSamplesPerEdge,
            options.chordError,
            denseEdgeSampleCounts);
        std::vector<gp_Pnt> densePoints;
        densePoints.reserve(denseSamples.size());
        for (const auto& sample : denseSamples) {
            densePoints.push_back(sample.point);
        }

        const auto cornerAnchors = collect_unique_edge_endpoints(
            document,
            boundary.ordered_boundary_edges,
            1.0e-7);

        report.featureEdgeDenseSampleCount = static_cast<int>(densePoints.size());
        report.cornerAnchorSampleCount = static_cast<int>(cornerAnchors.size());

        if (densePoints.empty() && cornerAnchors.empty()) {
            report.warningMessage = append_warning(
                report.warningMessage,
                "B1 corner/feature dense sampling was enabled but no anchors were generated.");
        }
    }

    // Build output mesh
    outMesh.clear();
    for (const auto& tri : allTriangles) {
        if (!is_degenerate(tri, 1.0e-18)) {
            outMesh.addTriangle(tri);
        }
    }

    report.outputTriangleCount = static_cast<int>(outMesh.triangleCount());
    if (!options.enableBoundaryGuardBandSampling) {
        report.bandRingCount = 0;
        report.boundaryBandSampleCount = 0;
    }

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
