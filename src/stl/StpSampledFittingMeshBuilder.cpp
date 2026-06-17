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
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>
#include <gp_Vec2d.hxx>
#include <TopExp_Explorer.hxx>

#include "merge/RegionBoundaryAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
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

struct OverCoverBuildResult {
    int boundaryEdgeCount = 0;
    int coveredBoundaryEdgeCount = 0;
    int sampleCount = 0;
    int triangleCount = 0;
    int fallbackCount = 0;
    int rejectedCount = 0;
    double boundaryCoverage = 0.0;
};

struct AdjacentFaceSupportCollarBuildResult {
    int boundaryEdgeCount = 0;
    int coveredBoundaryEdgeCount = 0;
    int sampleCount = 0;
    int triangleCount = 0;
    int adjacentFaceSampleCount = 0;
    int fallbackCount = 0;
    int rejectedCount = 0;
    double boundaryCoverage = 0.0;
    int cornerClampCount = 0;
    double maxOffset = 0.0;
};

struct QuantizedPoint {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    bool operator==(const QuantizedPoint& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct QuantizedPointHash {
    std::size_t operator()(const QuantizedPoint& point) const {
        const auto hx = std::hash<std::int64_t>{}(point.x);
        const auto hy = std::hash<std::int64_t>{}(point.y);
        const auto hz = std::hash<std::int64_t>{}(point.z);
        return hx ^ (hy << 1) ^ (hz << 2);
    }
};

struct BoundaryEdgeKey {
    int first = -1;
    int second = -1;

    bool operator==(const BoundaryEdgeKey& other) const {
        return first == other.first && second == other.second;
    }
};

struct BoundaryEdgeKeyHash {
    std::size_t operator()(const BoundaryEdgeKey& edge) const {
        const auto h0 = std::hash<int>{}(edge.first);
        const auto h1 = std::hash<int>{}(edge.second);
        return h0 ^ (h1 << 1);
    }
};

struct BoundaryEdgeAccum {
    int first = -1;
    int second = -1;
    int count = 0;
};

QuantizedPoint quantize_point(const StlVec3& point) {
    constexpr double scale = 1.0e8;
    return {
        static_cast<std::int64_t>(std::llround(point.x * scale)),
        static_cast<std::int64_t>(std::llround(point.y * scale)),
        static_cast<std::int64_t>(std::llround(point.z * scale))
    };
}

bool finite_vec(const StlVec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

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
    const gp_Vec a(p0, p1);
    const gp_Vec b(p0, p2);
    auto normal = a.Crossed(b);
    if (normal.Magnitude() > 1.0e-15) {
        normal.Normalize();
        t.normal = {normal.X(), normal.Y(), normal.Z()};
    } else {
        t.normal = {0.0, 0.0, 0.0};
    }
    return t;
}

double dot_vec(const gp_Vec& lhs, const gp_Vec& rhs) {
    return lhs.X() * rhs.X() + lhs.Y() * rhs.Y() + lhs.Z() * rhs.Z();
}

double vector_delta_magnitude(const gp_Vec& lhs, const gp_Vec& rhs) {
    const auto dx = lhs.X() - rhs.X();
    const auto dy = lhs.Y() - rhs.Y();
    const auto dz = lhs.Z() - rhs.Z();
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::optional<gp_Vec> normalized_vec(gp_Vec value) {
    if (value.Magnitude() <= 1.0e-12) {
        return std::nullopt;
    }
    value.Normalize();
    return value;
}

bool normalized_dot_below(const gp_Vec& lhs, const gp_Vec& rhs, double threshold) {
    const auto nl = normalized_vec(lhs);
    const auto nr = normalized_vec(rhs);
    if (!nl.has_value() || !nr.has_value()) {
        return false;
    }
    return dot_vec(*nl, *nr) < threshold;
}

void apply_corner_safe_support_clamp(
    const std::vector<gp_Pnt>& innerRing,
    std::vector<gp_Pnt>& supportRing,
    double targetDistance,
    int iterations,
    double maxOffsetScale,
    int& adjustedCount,
    double& maxOffset) {
    const auto count = innerRing.size();
    if (count < 3 || supportRing.size() != count || targetDistance <= 0.0) {
        return;
    }

    std::vector<gp_Vec> offsets;
    offsets.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        offsets.emplace_back(innerRing[index], supportRing[index]);
    }

    const auto safeIterations = std::clamp(iterations, 0, 8);
    const auto safeScale = std::max(maxOffsetScale, 1.0);
    const auto maxAllowedOffset = targetDistance * safeScale;
    constexpr double kTurnDotThreshold = 0.94; // about 20 degrees.
    constexpr double kChangeTolerance = 1.0e-9;
    std::vector<unsigned char> touched(count, 0);

    for (int iteration = 0; iteration < safeIterations; ++iteration) {
        auto nextOffsets = offsets;
        for (std::size_t index = 0; index < count; ++index) {
            const auto prev = index == 0 ? count - 1 : index - 1;
            const auto next = (index + 1) % count;

            const gp_Vec prevSegment(innerRing[prev], innerRing[index]);
            const gp_Vec nextSegment(innerRing[index], innerRing[next]);
            const auto boundaryCorner = normalized_dot_below(prevSegment, nextSegment, kTurnDotThreshold);
            const auto offsetJump =
                normalized_dot_below(offsets[index], offsets[prev], kTurnDotThreshold) ||
                normalized_dot_below(offsets[index], offsets[next], kTurnDotThreshold);
            const auto oversized = offsets[index].Magnitude() > maxAllowedOffset;
            if (!boundaryCorner && !offsetJump && !oversized) {
                continue;
            }
            touched[index] = 1;

            auto blended = offsets[index].Multiplied(0.5);
            blended += offsets[prev].Multiplied(0.25);
            blended += offsets[next].Multiplied(0.25);
            if (blended.Magnitude() <= 1.0e-12) {
                blended = offsets[index];
            }
            if (blended.Magnitude() > maxAllowedOffset) {
                blended.Normalize();
                blended.Multiply(maxAllowedOffset);
            }
            if (vector_delta_magnitude(blended, offsets[index]) > kChangeTolerance) {
                touched[index] = 1;
            }
            nextOffsets[index] = blended;
        }
        offsets = std::move(nextOffsets);
    }

    for (std::size_t index = 0; index < count; ++index) {
        if (offsets[index].Magnitude() > maxAllowedOffset) {
            offsets[index].Normalize();
            offsets[index].Multiply(maxAllowedOffset);
            touched[index] = 1;
        }
        maxOffset = std::max(maxOffset, offsets[index].Magnitude());
        supportRing[index] = innerRing[index].Translated(offsets[index]);
    }

    adjustedCount += static_cast<int>(std::count(touched.begin(), touched.end(), 1));
}

std::optional<gp_Vec> triangle_normal(
    const gp_Pnt& p0,
    const gp_Pnt& p1,
    const gp_Pnt& p2) {
    const gp_Vec a(p0, p1);
    const gp_Vec b(p0, p2);
    auto normal = a.Crossed(b);
    if (normal.Magnitude() <= 1.0e-15) {
        return std::nullopt;
    }
    normal.Normalize();
    return normal;
}

StlTriangle make_oriented_stl_triangle(
    const gp_Pnt& p0,
    const gp_Pnt& p1,
    const gp_Pnt& p2,
    const gp_Vec& referenceNormal) {
    const auto normal = triangle_normal(p0, p1, p2);
    if (normal.has_value() && referenceNormal.Magnitude() > 1.0e-15 &&
        dot_vec(*normal, referenceNormal) < 0.0) {
        return make_stl_triangle(p0, p2, p1);
    }
    return make_stl_triangle(p0, p1, p2);
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

std::optional<gp_Vec> surface_normal_at(
    const TopoDS_Face& face,
    const Handle(Geom_Surface)& surface,
    const gp_Pnt2d& uv) {
    if (surface.IsNull()) {
        return std::nullopt;
    }

    try {
        GeomLProp_SLProps props(surface, uv.X(), uv.Y(), 1, 1.0e-7);
        if (!props.IsNormalDefined()) {
            return std::nullopt;
        }

        gp_Vec normal(props.Normal());
        if (face.Orientation() == TopAbs_REVERSED) {
            normal.Reverse();
        }
        if (normal.Magnitude() <= 1.0e-15) {
            return std::nullopt;
        }
        normal.Normalize();
        return normal;
    } catch (const Standard_Failure&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<gp_Pnt> surface_point_at_uv_offset(
    const Handle(Geom_Surface)& surface,
    const gp_Pnt2d& uv,
    const gp_Vec2d& uvDirection,
    double distance,
    double uMin,
    double uMax,
    double vMin,
    double vMax) {
    if (surface.IsNull() || distance <= 0.0 || uvDirection.SquareMagnitude() <= 1.0e-24) {
        return std::nullopt;
    }

    try {
        auto direction = uvDirection;
        direction.Normalize();
        const auto span = std::max({
            std::abs(uMax - uMin),
            std::abs(vMax - vMin),
            1.0
        });
        const auto probeStep = span * 1.0e-5;
        const auto base = surface->Value(uv.X(), uv.Y());
        const auto probe = surface->Value(
            uv.X() + direction.X() * probeStep,
            uv.Y() + direction.Y() * probeStep);
        const auto probeDistance = base.Distance(probe);
        if (!std::isfinite(probeDistance) || probeDistance <= 1.0e-12) {
            return std::nullopt;
        }

        const auto paramStep = probeStep * distance / probeDistance;
        if (!std::isfinite(paramStep) || paramStep <= 0.0) {
            return std::nullopt;
        }

        const auto target = surface->Value(
            uv.X() + direction.X() * paramStep,
            uv.Y() + direction.Y() * paramStep);
        if (!std::isfinite(target.X()) || !std::isfinite(target.Y()) || !std::isfinite(target.Z())) {
            return std::nullopt;
        }
        return target;
    } catch (const Standard_Failure&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<gp_Pnt> candidate_surface_over_cover_point(
    const TopoDS_Edge& edge,
    const TopoDS_Face& face,
    const BoundarySample& sample,
    const gp_Pnt& candidateCenter,
    double distance,
    gp_Vec& outNormal) {
    const auto surface = BRep_Tool::Surface(face);
    if (surface.IsNull() || distance <= 0.0) {
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

    try {
        gp_Pnt2d uv;
        gp_Vec2d tangent;
        pcurve->D1(clamp_range(sample.parameter, firstParam, lastParam), uv, tangent);
        if (tangent.SquareMagnitude() <= 1.0e-24) {
            return std::nullopt;
        }

        const auto normal = surface_normal_at(face, surface, uv);
        if (!normal.has_value()) {
            return std::nullopt;
        }
        outNormal = *normal;

        gp_Vec2d left(-tangent.Y(), tangent.X());
        gp_Vec2d right(tangent.Y(), -tangent.X());
        left.Normalize();
        right.Normalize();

        const auto span = std::max({
            std::abs(uMax - uMin),
            std::abs(vMax - vMin),
            1.0
        });
        const auto probeStep = span * 1.0e-5;
        auto outside = [&](const gp_Vec2d& direction) {
            const gp_Pnt2d probe(
                uv.X() + direction.X() * probeStep,
                uv.Y() + direction.Y() * probeStep);
            return !face_uv_inside(face, probe);
        };

        const auto leftPoint = surface_point_at_uv_offset(
            surface, uv, left, distance, uMin, uMax, vMin, vMax);
        const auto rightPoint = surface_point_at_uv_offset(
            surface, uv, right, distance, uMin, uMax, vMin, vMax);
        const auto leftOutside = outside(left);
        const auto rightOutside = outside(right);

        if (leftOutside && !rightOutside && leftPoint.has_value()) {
            return leftPoint;
        }
        if (rightOutside && !leftOutside && rightPoint.has_value()) {
            return rightPoint;
        }
        if (leftPoint.has_value() && rightPoint.has_value()) {
            return leftPoint->Distance(candidateCenter) >= rightPoint->Distance(candidateCenter)
                ? leftPoint
                : rightPoint;
        }
        if (leftPoint.has_value()) {
            return leftPoint;
        }
        return rightPoint;
    } catch (const Standard_Failure&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<gp_Pnt> adjacent_face_support_point(
    const TopoDS_Edge& edge,
    const TopoDS_Face& face,
    const BoundarySample& sample,
    const gp_Pnt& candidateCenter,
    double distance,
    gp_Vec& outNormal) {
    const auto surface = BRep_Tool::Surface(face);
    if (surface.IsNull() || distance <= 0.0) {
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

    try {
        gp_Pnt2d uv;
        gp_Vec2d tangent;
        pcurve->D1(clamp_range(sample.parameter, firstParam, lastParam), uv, tangent);
        if (tangent.SquareMagnitude() <= 1.0e-24) {
            return std::nullopt;
        }

        const auto normal = surface_normal_at(face, surface, uv);
        if (!normal.has_value()) {
            return std::nullopt;
        }
        outNormal = *normal;

        gp_Vec2d left(-tangent.Y(), tangent.X());
        gp_Vec2d right(tangent.Y(), -tangent.X());
        left.Normalize();
        right.Normalize();

        const auto span = std::max({
            std::abs(uMax - uMin),
            std::abs(vMax - vMin),
            1.0
        });
        const auto probeStep = span * 1.0e-5;
        auto inside = [&](const gp_Vec2d& direction) {
            const gp_Pnt2d probe(
                uv.X() + direction.X() * probeStep,
                uv.Y() + direction.Y() * probeStep);
            return face_uv_inside(face, probe);
        };

        const auto leftPoint = surface_point_at_uv_offset(
            surface, uv, left, distance, uMin, uMax, vMin, vMax);
        const auto rightPoint = surface_point_at_uv_offset(
            surface, uv, right, distance, uMin, uMax, vMin, vMax);
        const auto leftInside = inside(left);
        const auto rightInside = inside(right);

        if (leftInside && !rightInside && leftPoint.has_value()) {
            return leftPoint;
        }
        if (rightInside && !leftInside && rightPoint.has_value()) {
            return rightPoint;
        }
        if (leftPoint.has_value() && rightPoint.has_value()) {
            return leftPoint->Distance(candidateCenter) >= rightPoint->Distance(candidateCenter)
                ? leftPoint
                : rightPoint;
        }
        if (leftPoint.has_value()) {
            return leftPoint;
        }
        return rightPoint;
    } catch (const Standard_Failure&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
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
    auto bestDistinct = supportPoints.end();
    auto bestDistance = std::numeric_limits<double>::infinity();
    auto bestDistinctDistance = std::numeric_limits<double>::infinity();
    for (auto it = supportPoints.begin(); it != supportPoints.end(); ++it) {
        const auto distance = squared_distance(point, *it);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = it;
        }
        if (distance > 1.0e-14 && distance < bestDistinctDistance) {
            bestDistinctDistance = distance;
            bestDistinct = it;
        }
    }
    if (bestDistinct != supportPoints.end()) {
        return *bestDistinct;
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

int append_oriented_quad_strip(
    const std::vector<gp_Pnt>& inner,
    const std::vector<gp_Pnt>& outer,
    const std::vector<gp_Vec>& normals,
    std::vector<StlTriangle>& outTriangles,
    int& rejected,
    bool countRejected = true) {
    const auto count = std::min({inner.size(), outer.size(), normals.size()});
    if (count < 2) {
        return 0;
    }

    int added = 0;
    for (std::size_t index = 0; index + 1 < count; ++index) {
        auto referenceNormal = normals[index];
        if (normals[index + 1].Magnitude() > 1.0e-15) {
            referenceNormal += normals[index + 1];
        }
        if (referenceNormal.Magnitude() <= 1.0e-15) {
            referenceNormal = gp_Vec(0.0, 0.0, 1.0);
        }
        referenceNormal.Normalize();

        auto first = make_oriented_stl_triangle(inner[index], inner[index + 1], outer[index], referenceNormal);
        if (!is_degenerate(first, 1.0e-15)) {
            outTriangles.push_back(first);
            ++added;
        } else if (countRejected) {
            ++rejected;
        }

        auto second = make_oriented_stl_triangle(inner[index + 1], outer[index + 1], outer[index], referenceNormal);
        if (!is_degenerate(second, 1.0e-15)) {
            outTriangles.push_back(second);
            ++added;
        } else if (countRejected) {
            ++rejected;
        }
    }
    return added;
}

int append_oriented_closed_quad_strip(
    const std::vector<gp_Pnt>& inner,
    const std::vector<gp_Pnt>& outer,
    const std::vector<gp_Vec>& normals,
    std::vector<StlTriangle>& outTriangles,
    int& rejected,
    bool countRejected = true) {
    const auto count = std::min({inner.size(), outer.size(), normals.size()});
    if (count < 3) {
        return 0;
    }

    int added = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto next = (index + 1) % count;
        auto referenceNormal = normals[index];
        if (normals[next].Magnitude() > 1.0e-15) {
            referenceNormal += normals[next];
        }
        if (referenceNormal.Magnitude() <= 1.0e-15) {
            referenceNormal = gp_Vec(0.0, 0.0, 1.0);
        }
        referenceNormal.Normalize();

        auto first = make_oriented_stl_triangle(inner[index], inner[next], outer[index], referenceNormal);
        if (!is_degenerate(first, 1.0e-15)) {
            outTriangles.push_back(first);
            ++added;
        } else if (countRejected) {
            ++rejected;
        }

        auto second = make_oriented_stl_triangle(inner[next], outer[next], outer[index], referenceNormal);
        if (!is_degenerate(second, 1.0e-15)) {
            outTriangles.push_back(second);
            ++added;
        } else if (countRejected) {
            ++rejected;
        }
    }
    return added;
}

struct MeshBoundaryLoop {
    std::vector<gp_Pnt> points;
    int edgeCount = 0;
    int componentCount = 0;
    int rejectedCount = 0;
};

MeshBoundaryLoop extract_ordered_mesh_boundary_loop(const std::vector<StlTriangle>& triangles) {
    MeshBoundaryLoop result;
    std::vector<StlVec3> vertices;
    std::unordered_map<QuantizedPoint, int, QuantizedPointHash> vertexIds;
    std::unordered_map<BoundaryEdgeKey, BoundaryEdgeAccum, BoundaryEdgeKeyHash> edges;

    auto vertex_id = [&](const StlVec3& vertex) -> int {
        const auto key = quantize_point(vertex);
        const auto found = vertexIds.find(key);
        if (found != vertexIds.end()) {
            return found->second;
        }

        const auto id = static_cast<int>(vertices.size());
        vertexIds.emplace(key, id);
        vertices.push_back(vertex);
        return id;
    };

    auto add_edge = [&](int first, int second) {
        if (first == second) {
            ++result.rejectedCount;
            return;
        }
        const BoundaryEdgeKey key {std::min(first, second), std::max(first, second)};
        auto& edge = edges[key];
        edge.first = key.first;
        edge.second = key.second;
        ++edge.count;
    };

    for (const auto& triangle : triangles) {
        if (!finite_vec(triangle.v0) || !finite_vec(triangle.v1) || !finite_vec(triangle.v2)) {
            result.rejectedCount += 3;
            continue;
        }
        const auto v0 = vertex_id(triangle.v0);
        const auto v1 = vertex_id(triangle.v1);
        const auto v2 = vertex_id(triangle.v2);
        add_edge(v0, v1);
        add_edge(v1, v2);
        add_edge(v2, v0);
    }

    std::vector<BoundaryEdgeAccum> boundaryEdges;
    boundaryEdges.reserve(edges.size());
    for (const auto& [_, edge] : edges) {
        if (edge.count == 1) {
            boundaryEdges.push_back(edge);
        }
    }
    result.edgeCount = static_cast<int>(boundaryEdges.size());
    if (boundaryEdges.empty()) {
        return result;
    }

    std::vector<std::vector<int>> adjacency(vertices.size());
    for (const auto& edge : boundaryEdges) {
        adjacency[static_cast<std::size_t>(edge.first)].push_back(edge.second);
        adjacency[static_cast<std::size_t>(edge.second)].push_back(edge.first);
    }

    std::vector<bool> boundaryVisited(vertices.size(), false);
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        if (adjacency[index].empty() || boundaryVisited[index]) {
            continue;
        }
        ++result.componentCount;
        std::queue<int> queue;
        queue.push(static_cast<int>(index));
        boundaryVisited[index] = true;
        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();
            for (const auto next : adjacency[static_cast<std::size_t>(current)]) {
                if (!boundaryVisited[static_cast<std::size_t>(next)]) {
                    boundaryVisited[static_cast<std::size_t>(next)] = true;
                    queue.push(next);
                }
            }
        }
    }

    const auto start = boundaryEdges.front().first;
    int previous = -1;
    int current = start;
    std::unordered_set<int> visitedLoopVertices;
    for (int guard = 0; guard <= result.edgeCount + 1; ++guard) {
        if (current < 0 || static_cast<std::size_t>(current) >= vertices.size()) {
            break;
        }
        if (visitedLoopVertices.find(current) != visitedLoopVertices.end()) {
            break;
        }
        visitedLoopVertices.insert(current);
        result.points.push_back(vec3_to_point(vertices[static_cast<std::size_t>(current)]));

        int next = -1;
        for (const auto candidate : adjacency[static_cast<std::size_t>(current)]) {
            if (candidate != previous) {
                next = candidate;
                break;
            }
        }
        if (next < 0) {
            break;
        }
        if (next == start) {
            break;
        }
        previous = current;
        current = next;
    }

    return result;
}

std::optional<FaceId> guard_face_for_edge(
    const TopologyGraph& topology,
    EdgeId edgeId,
    const std::unordered_set<FaceId>& candidateFaces);

AdjacentFaceSupportCollarBuildResult append_adjacent_face_support_collar(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionBoundaryAnalysis& boundary,
    const StpSampledFittingOptions& options,
    const StlBoundingBox& candidateBBox,
    const std::vector<StlTriangle>& baseTriangles,
    std::vector<StlTriangle>& outTriangles) {
    AdjacentFaceSupportCollarBuildResult result;
    if (!options.enableAdjacentFaceSupportCollar ||
        options.adjacentFaceSupportCollarWidth <= 0.0 ||
        options.adjacentFaceSupportCollarRingCount <= 0 ||
        baseTriangles.empty()) {
        return result;
    }

    const auto& topology = document.topology();
    std::unordered_set<FaceId> candidateFaces(candidate.faces.begin(), candidate.faces.end());
    result.boundaryEdgeCount = static_cast<int>(boundary.ordered_boundary_edges.size());
    if (boundary.ordered_boundary_edges.empty()) {
        return result;
    }

    const auto center = bbox_center(candidateBBox);
    const auto ringCount = std::max(options.adjacentFaceSupportCollarRingCount, 1);
    const auto width = options.adjacentFaceSupportCollarWidth;
    const auto samplesPerEdge = std::max(
        options.adjacentFaceSupportCollarSamplesPerEdge,
        options.minBoundarySamplesPerEdge);
    constexpr double joinToleranceSquared = 1.0e-10;

    std::vector<gp_Pnt> boundaryRing;
    std::vector<gp_Vec> normalRing;
    std::vector<std::vector<gp_Pnt>> supportRings(static_cast<std::size_t>(ringCount));
    std::vector<std::vector<unsigned char>> supportRingFromAdjacent(static_cast<std::size_t>(ringCount));
    boundaryRing.reserve(static_cast<std::size_t>(result.boundaryEdgeCount * samplesPerEdge));
    normalRing.reserve(static_cast<std::size_t>(result.boundaryEdgeCount * samplesPerEdge));
    for (auto& ring : supportRings) {
        ring.reserve(static_cast<std::size_t>(result.boundaryEdgeCount * samplesPerEdge));
    }
    for (auto& ring : supportRingFromAdjacent) {
        ring.reserve(static_cast<std::size_t>(result.boundaryEdgeCount * samplesPerEdge));
    }

    bool hasPreviousPoint = false;
    gp_Pnt previousPoint;
    gp_Pnt firstLoopPoint;

    for (std::size_t edgeOrdinal = 0; edgeOrdinal < boundary.ordered_boundary_edges.size(); ++edgeOrdinal) {
        const auto edgeId = boundary.ordered_boundary_edges[edgeOrdinal];
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) {
            ++result.rejectedCount;
            continue;
        }

        const auto adjacentFaceId = guard_face_for_edge(topology, edgeId, candidateFaces);
        const auto& edge = topology.edge(edgeId);
        const TopoDS_Face* adjacentFace = nullptr;
        if (adjacentFaceId.has_value() && *adjacentFaceId < topology.faceCount()) {
            adjacentFace = &topology.face(*adjacentFaceId);
        }

        BRepAdaptor_Curve adaptor(edge);
        const auto firstParam = adaptor.FirstParameter();
        const auto lastParam = adaptor.LastParameter();
        const auto firstPoint = adaptor.Value(firstParam);
        const auto lastPoint = adaptor.Value(lastParam);
        const auto reverseEdge = hasPreviousPoint &&
            lastPoint.SquareDistance(previousPoint) < firstPoint.SquareDistance(previousPoint);

        int appendedForEdge = 0;
        int adjacentSamplesForEdge = 0;
        for (int sampleIndex = 0; sampleIndex <= samplesPerEdge; ++sampleIndex) {
            const auto ratio = static_cast<double>(sampleIndex) / static_cast<double>(samplesPerEdge);
            const auto parameter = reverseEdge
                ? lastParam + (firstParam - lastParam) * ratio
                : firstParam + (lastParam - firstParam) * ratio;
            const auto point = adaptor.Value(parameter);

            if (hasPreviousPoint && sampleIndex == 0 &&
                point.SquareDistance(previousPoint) <= joinToleranceSquared) {
                continue;
            }
            if (edgeOrdinal + 1 == boundary.ordered_boundary_edges.size() &&
                sampleIndex == samplesPerEdge &&
                !boundaryRing.empty() &&
                point.SquareDistance(firstLoopPoint) <= joinToleranceSquared) {
                continue;
            }

            BoundarySample sample;
            sample.point = point;
            sample.parameter = parameter;
            sample.edgeId = edgeId;

            if (boundaryRing.empty()) {
                firstLoopPoint = point;
            }
            previousPoint = point;
            hasPreviousPoint = true;
            boundaryRing.push_back(point);

            gp_Vec normal(0.0, 0.0, 1.0);
            for (int ring = 1; ring <= ringCount; ++ring) {
                const auto distance = width * static_cast<double>(ring) / static_cast<double>(ringCount);
                std::optional<gp_Pnt> supportPoint;
                gp_Vec supportNormal = normal;
                if (adjacentFace != nullptr) {
                    supportPoint = adjacent_face_support_point(
                        edge,
                        *adjacentFace,
                        sample,
                        center,
                        distance,
                        supportNormal);
                }

                const auto ringIndex = static_cast<std::size_t>(ring - 1);
                if (supportPoint.has_value()) {
                    normal = supportNormal;
                    supportRings[ringIndex].push_back(*supportPoint);
                    supportRingFromAdjacent[ringIndex].push_back(1);
                    ++adjacentSamplesForEdge;
                } else {
                    ++result.fallbackCount;
                    supportRings[ringIndex].push_back(
                        radial_guard_point(point, center, distance));
                    supportRingFromAdjacent[ringIndex].push_back(0);
                }
            }
            normalRing.push_back(normal);
            ++appendedForEdge;
        }

        if (appendedForEdge >= 2 && adjacentSamplesForEdge > 0) {
            ++result.coveredBoundaryEdgeCount;
        } else {
            ++result.rejectedCount;
        }
    }

    const auto meshBoundary = extract_ordered_mesh_boundary_loop(baseTriangles);
    result.rejectedCount += meshBoundary.rejectedCount;
    if (meshBoundary.points.size() < 3 || boundaryRing.empty()) {
        result.boundaryCoverage = 0.0;
        return result;
    }

    std::vector<gp_Vec> meshNormalRing;
    std::vector<std::vector<gp_Pnt>> meshSupportRings(static_cast<std::size_t>(ringCount));
    meshNormalRing.reserve(meshBoundary.points.size());
    for (auto& ring : meshSupportRings) {
        ring.reserve(meshBoundary.points.size());
    }

    for (const auto& meshPoint : meshBoundary.points) {
        std::size_t nearestIndex = 0;
        auto nearestDistance = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < boundaryRing.size(); ++index) {
            const auto distance = squared_distance(meshPoint, boundaryRing[index]);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearestIndex = index;
            }
        }

        meshNormalRing.push_back(normalRing[nearestIndex]);
        for (int ring = 0; ring < ringCount; ++ring) {
            const auto ringIndex = static_cast<std::size_t>(ring);
            if (nearestIndex >= supportRings[ringIndex].size()) {
                ++result.rejectedCount;
                meshSupportRings[ringIndex].push_back(meshPoint);
                continue;
            }

            gp_Vec offset(boundaryRing[nearestIndex], supportRings[ringIndex][nearestIndex]);
            if (offset.Magnitude() <= 1.0e-12) {
                ++result.rejectedCount;
                meshSupportRings[ringIndex].push_back(meshPoint);
                continue;
            }

            meshSupportRings[ringIndex].push_back(meshPoint.Translated(offset));
            ++result.sampleCount;
            if (nearestIndex < supportRingFromAdjacent[ringIndex].size() &&
                supportRingFromAdjacent[ringIndex][nearestIndex] != 0) {
                ++result.adjacentFaceSampleCount;
            }
        }
    }

    for (int ring = 0; ring < ringCount; ++ring) {
        const auto ringIndex = static_cast<std::size_t>(ring);
        const auto targetDistance = width * static_cast<double>(ring + 1) / static_cast<double>(ringCount);
        if (options.enableAdjacentFaceSupportCollarCornerClamp) {
            apply_corner_safe_support_clamp(
                meshBoundary.points,
                meshSupportRings[ringIndex],
                targetDistance,
                options.adjacentFaceSupportCollarCornerSmoothingIterations,
                options.adjacentFaceSupportCollarMaxOffsetScale,
                result.cornerClampCount,
                result.maxOffset);
        } else {
            for (std::size_t index = 0; index < meshBoundary.points.size() &&
                index < meshSupportRings[ringIndex].size(); ++index) {
                const gp_Vec offset(meshBoundary.points[index], meshSupportRings[ringIndex][index]);
                result.maxOffset = std::max(result.maxOffset, offset.Magnitude());
            }
        }
    }

    auto added = 0;
    if (!meshSupportRings.empty()) {
        added += append_oriented_closed_quad_strip(
            meshBoundary.points,
            meshSupportRings.front(),
            meshNormalRing,
            outTriangles,
            result.rejectedCount);
        for (std::size_t ring = 1; ring < meshSupportRings.size(); ++ring) {
            added += append_oriented_closed_quad_strip(
                meshSupportRings[ring - 1],
                meshSupportRings[ring],
                meshNormalRing,
                outTriangles,
                result.rejectedCount);
        }
    }
    result.triangleCount += added;

    result.boundaryCoverage = result.boundaryEdgeCount > 0
        ? static_cast<double>(result.coveredBoundaryEdgeCount) / static_cast<double>(result.boundaryEdgeCount)
        : 0.0;
    return result;
}

OverCoverBuildResult append_boundary_over_cover_strip(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionBoundaryAnalysis& boundary,
    const StpSampledFittingOptions& options,
    const StlBoundingBox& candidateBBox,
    const std::vector<StlTriangle>& baseTriangles,
    std::vector<StlTriangle>& outTriangles) {
    OverCoverBuildResult result;
    if (!options.enableBoundaryOverCoverStrip ||
        options.boundaryOverCoverWidth <= 0.0 ||
        options.boundaryOverCoverRingCount <= 0 ||
        baseTriangles.empty()) {
        return result;
    }

    const auto& topology = document.topology();
    std::unordered_set<FaceId> candidateFaces(candidate.faces.begin(), candidate.faces.end());
    result.boundaryEdgeCount = static_cast<int>(boundary.ordered_boundary_edges.size());
    if (boundary.ordered_boundary_edges.empty()) {
        return result;
    }

    const auto center = bbox_center(candidateBBox);
    const auto ringCount = std::max(options.boundaryOverCoverRingCount, 1);
    const auto width = options.boundaryOverCoverWidth;
    const auto samplesPerEdge = std::max(options.boundarySamplesPerEdge, options.minBoundarySamplesPerEdge);
    constexpr double joinToleranceSquared = 1.0e-10;

    std::vector<gp_Pnt> boundaryRing;
    std::vector<gp_Vec> normalRing;
    std::vector<std::size_t> cornerIndices;
    std::vector<std::vector<gp_Pnt>> overCoverRings(static_cast<std::size_t>(ringCount));
    boundaryRing.reserve(static_cast<std::size_t>(result.boundaryEdgeCount * samplesPerEdge));
    normalRing.reserve(static_cast<std::size_t>(result.boundaryEdgeCount * samplesPerEdge));
    cornerIndices.reserve(static_cast<std::size_t>(result.boundaryEdgeCount));
    for (auto& ring : overCoverRings) {
        ring.reserve(static_cast<std::size_t>(result.boundaryEdgeCount * samplesPerEdge));
    }

    bool hasPreviousPoint = false;
    gp_Pnt previousPoint;
    gp_Pnt firstLoopPoint;

    for (std::size_t edgeOrdinal = 0; edgeOrdinal < boundary.ordered_boundary_edges.size(); ++edgeOrdinal) {
        const auto edgeId = boundary.ordered_boundary_edges[edgeOrdinal];
        if (edgeId < 0 || static_cast<std::size_t>(edgeId) >= topology.edgeCount()) {
            ++result.rejectedCount;
            continue;
        }

        const auto candidateFaceId = [&]() -> std::optional<FaceId> {
            const auto* adjacency = topology.adjacencyForEdge(edgeId);
            if (adjacency == nullptr) {
                return std::nullopt;
            }
            for (const auto faceId : adjacency->faces) {
                if (candidateFaces.find(faceId) != candidateFaces.end()) {
                    return faceId;
                }
            }
            return std::nullopt;
        }();

        const auto& edge = topology.edge(edgeId);
        const TopoDS_Face* candidateFace = nullptr;
        if (candidateFaceId.has_value() && *candidateFaceId < topology.faceCount()) {
            candidateFace = &topology.face(*candidateFaceId);
        }

        BRepAdaptor_Curve adaptor(edge);
        const auto firstParam = adaptor.FirstParameter();
        const auto lastParam = adaptor.LastParameter();
        const auto firstPoint = adaptor.Value(firstParam);
        const auto lastPoint = adaptor.Value(lastParam);
        const auto reverseEdge = hasPreviousPoint &&
            lastPoint.SquareDistance(previousPoint) < firstPoint.SquareDistance(previousPoint);

        int appendedForEdge = 0;
        for (int sampleIndex = 0; sampleIndex <= samplesPerEdge; ++sampleIndex) {
            const auto ratio = static_cast<double>(sampleIndex) / static_cast<double>(samplesPerEdge);
            const auto parameter = reverseEdge
                ? lastParam + (firstParam - lastParam) * ratio
                : firstParam + (lastParam - firstParam) * ratio;
            const auto point = adaptor.Value(parameter);

            if (hasPreviousPoint && sampleIndex == 0 &&
                point.SquareDistance(previousPoint) <= joinToleranceSquared) {
                continue;
            }
            if (edgeOrdinal + 1 == boundary.ordered_boundary_edges.size() &&
                sampleIndex == samplesPerEdge &&
                !boundaryRing.empty() &&
                point.SquareDistance(firstLoopPoint) <= joinToleranceSquared) {
                continue;
            }

            BoundarySample sample;
            sample.point = point;
            sample.parameter = parameter;
            sample.edgeId = edgeId;

            const auto sampleOrdinal = boundaryRing.size();
            const auto isCornerSample = sampleIndex == 0 || sampleIndex == samplesPerEdge;
            if (boundaryRing.empty()) {
                firstLoopPoint = point;
            }
            previousPoint = point;
            hasPreviousPoint = true;

            boundaryRing.push_back(sample.point);

            gp_Vec normal(0.0, 0.0, 1.0);
            if (candidateFace != nullptr) {
                double pcurveFirst = 0.0, pcurveLast = 0.0;
                const auto pcurve = BRep_Tool::CurveOnSurface(edge, *candidateFace, pcurveFirst, pcurveLast);
                if (!pcurve.IsNull()) {
                    const auto uv = pcurve->Value(clamp_range(sample.parameter, pcurveFirst, pcurveLast));
                    const auto surface = BRep_Tool::Surface(*candidateFace);
                    const auto surfaceNormal = surface_normal_at(*candidateFace, surface, uv);
                    if (surfaceNormal.has_value()) {
                        normal = *surfaceNormal;
                    } else {
                        ++result.fallbackCount;
                    }
                } else {
                    ++result.fallbackCount;
                }
            } else {
                ++result.fallbackCount;
            }
            normalRing.push_back(normal);

            for (int ring = 1; ring <= ringCount; ++ring) {
                const auto distance = width * static_cast<double>(ring) / static_cast<double>(ringCount);
                std::optional<gp_Pnt> overCoverPoint;
                if (candidateFace != nullptr) {
                    gp_Vec surfaceNormal = normal;
                    overCoverPoint = candidate_surface_over_cover_point(
                        edge,
                        *candidateFace,
                        sample,
                        center,
                        distance,
                        surfaceNormal);
                    if (overCoverPoint.has_value()) {
                        normalRing.back() = surfaceNormal;
                    }
                }

                if (!overCoverPoint.has_value()) {
                    ++result.fallbackCount;
                    overCoverPoint = radial_guard_point(sample.point, center, distance);
                }

                overCoverRings[static_cast<std::size_t>(ring - 1)].push_back(*overCoverPoint);
            }
            if (isCornerSample) {
                cornerIndices.push_back(sampleOrdinal);
            }
            ++appendedForEdge;
        }

        if (appendedForEdge >= 2) {
            ++result.coveredBoundaryEdgeCount;
        } else {
            ++result.rejectedCount;
        }
    }

    if (boundaryRing.size() < 3) {
        return result;
    }

    for (int ring = 1; ring <= ringCount; ++ring) {
        const auto ringIndex = static_cast<std::size_t>(ring - 1);
        const auto distance = width * static_cast<double>(ring) / static_cast<double>(ringCount);
        auto& overCoverRing = overCoverRings[ringIndex];
        for (const auto cornerIndex : cornerIndices) {
            if (cornerIndex >= boundaryRing.size()) {
                continue;
            }
            const auto prev = cornerIndex == 0 ? boundaryRing.size() - 1 : cornerIndex - 1;
            const auto next = (cornerIndex + 1) % boundaryRing.size();
            gp_Vec prevOffset(boundaryRing[prev], overCoverRing[prev]);
            gp_Vec nextOffset(boundaryRing[next], overCoverRing[next]);
            if (prevOffset.Magnitude() <= 1.0e-12 || nextOffset.Magnitude() <= 1.0e-12) {
                continue;
            }
            prevOffset.Normalize();
            nextOffset.Normalize();
            auto miter = prevOffset + nextOffset;
            if (miter.Magnitude() <= 1.0e-12) {
                continue;
            }
            miter.Normalize();
            const auto denominator = std::max(std::abs(dot_vec(miter, prevOffset)), 0.25);
            const auto miterDistance = std::min(distance / denominator, distance * 2.5);
            overCoverRing[cornerIndex] = boundaryRing[cornerIndex].Translated(miter.Multiplied(miterDistance));
        }
    }

    const auto meshBoundary = extract_ordered_mesh_boundary_loop(baseTriangles);
    result.boundaryEdgeCount = meshBoundary.edgeCount;
    result.rejectedCount += meshBoundary.rejectedCount;
    if (meshBoundary.points.size() < 3 || boundaryRing.empty()) {
        result.boundaryCoverage = 0.0;
        return result;
    }

    std::vector<gp_Vec> meshNormalRing;
    std::vector<std::vector<gp_Pnt>> meshOverCoverRings(static_cast<std::size_t>(ringCount));
    meshNormalRing.reserve(meshBoundary.points.size());
    for (auto& ring : meshOverCoverRings) {
        ring.reserve(meshBoundary.points.size());
    }

    for (const auto& meshPoint : meshBoundary.points) {
        std::size_t nearestIndex = 0;
        auto nearestDistance = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < boundaryRing.size(); ++index) {
            const auto distance = squared_distance(meshPoint, boundaryRing[index]);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearestIndex = index;
            }
        }

        meshNormalRing.push_back(normalRing[nearestIndex]);
        for (int ring = 0; ring < ringCount; ++ring) {
            const auto ringIndex = static_cast<std::size_t>(ring);
            if (nearestIndex >= overCoverRings[ringIndex].size()) {
                ++result.rejectedCount;
                meshOverCoverRings[ringIndex].push_back(meshPoint);
                continue;
            }
            gp_Vec offset(boundaryRing[nearestIndex], overCoverRings[ringIndex][nearestIndex]);
            if (offset.Magnitude() <= 1.0e-12) {
                ++result.rejectedCount;
                meshOverCoverRings[ringIndex].push_back(meshPoint);
            } else {
                meshOverCoverRings[ringIndex].push_back(meshPoint.Translated(offset));
            }
        }
    }

    result.sampleCount = static_cast<int>(meshBoundary.points.size()) * ringCount;
    auto added = 0;
    if (!meshOverCoverRings.empty()) {
        added += append_oriented_closed_quad_strip(
            meshBoundary.points,
            meshOverCoverRings.front(),
            meshNormalRing,
            outTriangles,
            result.rejectedCount);
        for (std::size_t ring = 1; ring < meshOverCoverRings.size(); ++ring) {
            added += append_oriented_closed_quad_strip(
                meshOverCoverRings[ring - 1],
                meshOverCoverRings[ring],
                meshNormalRing,
                outTriangles,
                result.rejectedCount);
        }
    }
    result.triangleCount += added;
    if (added > 0 && meshBoundary.componentCount == 1) {
        result.coveredBoundaryEdgeCount = result.boundaryEdgeCount;
    }

    result.boundaryCoverage = result.boundaryEdgeCount > 0
        ? static_cast<double>(result.coveredBoundaryEdgeCount) / static_cast<double>(result.boundaryEdgeCount)
        : 0.0;
    return result;
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
    report.boundaryOverCoverStripEnabled = options.enableBoundaryOverCoverStrip;
    report.adjacentFaceSupportCollarEnabled = options.enableAdjacentFaceSupportCollar;
    if (options.enableBoundaryGuardBandSampling) {
        report.boundaryGuardBandRingCount = std::max(options.boundaryGuardBandRingCount, 0);
        report.boundaryGuardBandSpacing = options.boundaryGuardBandSpacing;
    }
    if (options.enableBoundaryOverCoverStrip) {
        report.boundaryOverCoverWidth = options.boundaryOverCoverWidth;
        report.boundaryOverCoverRingCount = std::max(options.boundaryOverCoverRingCount, 0);
    }
    if (options.enableAdjacentFaceSupportCollar) {
        report.adjacentFaceSupportCollarWidth = options.adjacentFaceSupportCollarWidth;
        report.adjacentFaceSupportCollarRingCount = std::max(options.adjacentFaceSupportCollarRingCount, 0);
        report.adjacentFaceSupportCollarCornerClampEnabled =
            options.enableAdjacentFaceSupportCollarCornerClamp;
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

    if (options.enableAdjacentFaceSupportCollar) {
        const auto supportCollar = append_adjacent_face_support_collar(
            document,
            candidate,
            boundary,
            options,
            candidateBBox,
            allTriangles,
            allTriangles);
        report.adjacentFaceSupportCollarEdgeCount = supportCollar.boundaryEdgeCount;
        report.adjacentFaceSupportCollarSampleCount = supportCollar.sampleCount;
        report.adjacentFaceSupportCollarTriangleCount = supportCollar.triangleCount;
        report.adjacentFaceSupportCollarAdjacentFaceSampleCount = supportCollar.adjacentFaceSampleCount;
        report.adjacentFaceSupportCollarFallbackCount = supportCollar.fallbackCount;
        report.adjacentFaceSupportCollarRejectedCount = supportCollar.rejectedCount;
        report.adjacentFaceSupportCollarBoundaryCoverage = supportCollar.boundaryCoverage;
        report.adjacentFaceSupportCollarCornerClampCount = supportCollar.cornerClampCount;
        report.adjacentFaceSupportCollarMaxOffset = supportCollar.maxOffset;
        if (supportCollar.triangleCount == 0) {
            report.warningMessage = append_warning(
                report.warningMessage,
                "B2.2 adjacent-face support collar was enabled but no collar triangles were generated.");
        }
    }

    if (options.enableBoundaryOverCoverStrip) {
        const auto overCover = append_boundary_over_cover_strip(
            document,
            candidate,
            boundary,
            options,
            candidateBBox,
            allTriangles,
            allTriangles);
        report.boundaryOverCoverSampleCount = overCover.sampleCount;
        report.boundaryOverCoverTriangleCount = overCover.triangleCount;
        report.boundaryOverCoverFallbackCount = overCover.fallbackCount;
        report.boundaryOverCoverRejectedCount = overCover.rejectedCount;
        report.boundaryOverCoverBoundaryCoverage = overCover.boundaryCoverage;
        if (overCover.triangleCount == 0) {
            report.warningMessage = append_warning(
                report.warningMessage,
                "B2.1 boundary over-cover strip was enabled but no strip triangles were generated.");
        }
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
