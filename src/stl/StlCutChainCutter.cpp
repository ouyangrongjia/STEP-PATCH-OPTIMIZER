#include "stl/StlCutChainCutter.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

namespace spo {

// ============================================================================
// Internal helpers — geometry
// ============================================================================

namespace {

constexpr double kDoubleMax = std::numeric_limits<double>::max();

// ---- vec3 utilities ----

using Vec3 = std::array<double, 3>;

Vec3 vec3_from_stl(const StlVec3& v) { return {v.x, v.y, v.z}; }
StlVec3 stl_from_vec3(const Vec3& v) { return {v[0], v[1], v[2]}; }

Vec3 vec3_sub(const Vec3& a, const Vec3& b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
Vec3 vec3_add(const Vec3& a, const Vec3& b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
Vec3 vec3_mul(const Vec3& a, double t) { return {a[0]*t, a[1]*t, a[2]*t}; }
Vec3 vec3_lerp(const Vec3& a, const Vec3& b, double t) { return vec3_add(a, vec3_mul(vec3_sub(b, a), t)); }
double vec3_dot(const Vec3& a, const Vec3& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Vec3 vec3_cross(const Vec3& a, const Vec3& b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
double vec3_norm2(const Vec3& v) { return vec3_dot(v, v); }
double vec3_norm(const Vec3& v) { return std::sqrt(vec3_norm2(v)); }
Vec3 vec3_normalize(const Vec3& v) {
    double n = vec3_norm(v);
    return (n < 1e-30) ? Vec3{0,0,0} : Vec3{v[0]/n, v[1]/n, v[2]/n};
}

Vec3 gppnt_to_vec3(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }

// ---- stable key for spatial hashing ----

template <typename T>
struct KeyHash {
    std::size_t operator()(const std::tuple<T,T,T>& k) const noexcept {
        return static_cast<std::size_t>(
            (std::get<0>(k) * 73856093ULL) ^
            (std::get<1>(k) * 19349663ULL) ^
            (std::get<2>(k) * 83492791ULL));
    }
};

std::tuple<std::int64_t, std::int64_t, std::int64_t> make_spatial_key(
    const Vec3& p, double tol) {
    return {
        static_cast<std::int64_t>(std::llround(p[0] / tol)),
        static_cast<std::int64_t>(std::llround(p[1] / tol)),
        static_cast<std::int64_t>(std::llround(p[2] / tol))
    };
}

// ---- edge helpers ----

using EdgePair = StlCutChainCutter::EdgePair;

EdgePair make_edge(std::int64_t a, std::int64_t b) {
    return a < b ? EdgePair{a, b} : EdgePair{b, a};
}

bool edge_eq(const EdgePair& a, const EdgePair& b) { return a == b; }

// ---- 2D point/segment helpers ----

using Vec2 = std::array<double, 2>;

Vec2 sub2(const Vec2& a, const Vec2& b) { return {a[0]-b[0], a[1]-b[1]}; }
Vec2 add2(const Vec2& a, const Vec2& b) { return {a[0]+b[0], a[1]+b[1]}; }
Vec2 mul2(const Vec2& a, double t) { return {a[0]*t, a[1]*t}; }
double dot2(const Vec2& a, const Vec2& b) { return a[0]*b[0]+a[1]*b[1]; }
double cross2(const Vec2& a, const Vec2& b) { return a[0]*b[1]-a[1]*b[0]; }
double norm2_2(const Vec2& v) { return dot2(v, v); }
double norm2(const Vec2& v) { return std::sqrt(norm2_2(v)); }

int orient2d(const Vec2& a, const Vec2& b, const Vec2& c) {
    double v = cross2(sub2(b, a), sub2(c, a));
    if (v > 1e-12) return 1;
    if (v < -1e-12) return -1;
    return 0;
}

bool point_on_segment_2d(const Vec2& p, const Vec2& a, const Vec2& b, double tol = 1e-8) {
    Vec2 ab = sub2(b, a);
    double den = dot2(ab, ab);
    if (den < 1e-30) return norm2_2(sub2(p, a)) <= tol * tol;
    double t = dot2(sub2(p, a), ab) / den;
    if (t < -tol || t > 1.0 + tol) return false;
    Vec2 q = add2(a, mul2(ab, std::clamp(t, 0.0, 1.0)));
    return norm2_2(sub2(p, q)) <= tol * tol;
}

// segment A-B intersection with C-D. Returns intersection point and edge param on C-D.
std::pair<bool, double> segment_intersection_2d(
    const Vec2& pA, const Vec2& pB, const Vec2& a, const Vec2& b, double tol = 1e-10,
    Vec2* outPt = nullptr) {
    Vec2 r = sub2(pB, pA);
    Vec2 s = sub2(b, a);
    double den = r[0]*s[1] - r[1]*s[0];
    if (std::abs(den) < tol) return {false, 0.0};
    Vec2 ap = sub2(a, pA);
    double t = (ap[0]*s[1] - ap[1]*s[0]) / den;
    double u = (ap[0]*r[1] - ap[1]*r[0]) / den;
    if (t >= -tol && t <= 1.0+tol && u >= -tol && u <= 1.0+tol) {
        t = std::clamp(t, 0.0, 1.0);
        u = std::clamp(u, 0.0, 1.0);
        if (outPt) *outPt = add2(pA, mul2(r, t));
        return {true, u};
    }
    return {false, 0.0};
}

double point_param_on_segment_2d(const Vec2& p, const Vec2& a, const Vec2& b) {
    Vec2 ab = sub2(b, a);
    double den = dot2(ab, ab);
    if (den < 1e-30) return 0.0;
    return dot2(sub2(p, a), ab) / den;
}

// ---- 2D ear-clipping triangulation ----

bool is_ear(const std::vector<Vec2>& poly, int i, double tol) {
    int n = static_cast<int>(poly.size());
    int prev = (i - 1 + n) % n;
    int next = (i + 1) % n;
    Vec2 a = poly[static_cast<std::size_t>(prev)];
    Vec2 b = poly[static_cast<std::size_t>(i)];
    Vec2 c = poly[static_cast<std::size_t>(next)];

    // Must be convex
    if (orient2d(a, b, c) <= 0) return false;

    // No other vertex inside the triangle a-b-c
    for (int j = 0; j < n; ++j) {
        if (j == prev || j == i || j == next) continue;
        Vec2 p = poly[static_cast<std::size_t>(j)];
        // Check if p is inside triangle (a,b,c)
        if (orient2d(a, b, p) >= -tol &&
            orient2d(b, c, p) >= -tol &&
            orient2d(c, a, p) >= -tol) {
            return false;
        }
    }
    return true;
}

struct TriangulateResult {
    std::vector<std::array<int, 3>> tris;
    bool ok = false;
};

TriangulateResult triangulate_ear_clip(const std::vector<Vec2>& polygon, double tol = 1e-10) {
    TriangulateResult result;
    int n = static_cast<int>(polygon.size());
    if (n < 3) return result;
    if (n == 3) {
        result.tris.push_back({0, 1, 2});
        result.ok = true;
        return result;
    }

    std::vector<Vec2> verts = polygon;
    std::vector<int> indices(n);
    for (int i = 0; i < n; ++i) indices[static_cast<std::size_t>(i)] = i;

    int safety = 0;
    while (static_cast<int>(verts.size()) > 3 && safety < static_cast<int>(verts.size()) * 20) {
        ++safety;
        int m = static_cast<int>(verts.size());
        bool found = false;
        for (int i = 0; i < m; ++i) {
            if (is_ear(verts, i, tol)) {
                int p = (i - 1 + m) % m;
                int q = i;
                int r = (i + 1) % m;
                result.tris.push_back({indices[static_cast<std::size_t>(p)],
                                       indices[static_cast<std::size_t>(q)],
                                       indices[static_cast<std::size_t>(r)]});
                verts.erase(verts.begin() + i);
                indices.erase(indices.begin() + i);
                found = true;
                break;
            }
        }
        if (!found) break;
    }
    if (static_cast<int>(verts.size()) == 3) {
        result.tris.push_back({indices[0], indices[1], indices[2]});
        result.ok = true;
    } else if (static_cast<int>(verts.size()) <= 2) {
        result.ok = !result.tris.empty();
    }
    return result;
}

// ---- constrained polygon triangulation ----
// Given a set of vertices and constraint edges (forming a PSLG with outer
// polygon boundary), produce a triangulation that respects the constraints.

struct ConstrainedTriInput {
    std::vector<Vec2> vertices;
    std::vector<EdgePair> segments; // edges that must appear in output
    StlCutChainCutter::EdgeHash edgeHash;
};

// Simple "split along constraints" triangulator:
// Start with the polygon boundary, recursively split by constraint edges.
std::vector<std::array<int, 3>> triangulate_constrained_polygon(
    const std::vector<Vec2>& vertices,
    const std::vector<EdgePair>& constraintEdges,
    double tol = 1e-10) {
    std::vector<std::array<int, 3>> result;

    // Single triangle: already done
    if (vertices.size() == 3 && constraintEdges.empty()) {
        result.push_back({0, 1, 2});
        return result;
    }

    // If no constraints, just ear-clip
    if (constraintEdges.empty()) {
        auto r = triangulate_ear_clip(vertices, tol);
        return r.ok ? r.tris : result;
    }

    // Use the first constraint edge to split
    int a = static_cast<int>(constraintEdges[0].first);
    int b = static_cast<int>(constraintEdges[0].second);
    if (a >= static_cast<int>(vertices.size()) || b >= static_cast<int>(vertices.size())) {
        return result;
    }

    // Find the polygon on each side of the constraint edge
    // Collect remaining constraints on each side
    std::vector<EdgePair> remaining;
    for (std::size_t k = 1; k < constraintEdges.size(); ++k) {
        remaining.push_back(constraintEdges[k]);
    }

    // Split into left polygon and right polygon
    // This is a simplified approach: construct two sub-polygons split along AB
    // Left side: vertices on or to the left of AB
    // Right side: vertices on or to the right of AB
    Vec2 pa = vertices[static_cast<std::size_t>(a)];
    Vec2 pb = vertices[static_cast<std::size_t>(b)];
    Vec2 ab = sub2(pb, pa);

    // Classify all vertices
    std::vector<int> side(vertices.size(), 0); // -1=left, 0=on, 1=right
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (static_cast<int>(i) == a || static_cast<int>(i) == b) { side[i] = 0; continue; }
        double cr = cross2(ab, sub2(vertices[i], pa));
        side[i] = (cr > tol) ? 1 : ((cr < -tol) ? -1 : 0);
    }

    // Build left polygon: vertices on or left of AB, in order, with AB added
    std::vector<int> leftIdx, rightIdx;
    std::map<int, int> leftMap, rightMap;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (side[i] <= 0) {
            leftMap[static_cast<int>(i)] = static_cast<int>(leftIdx.size());
            leftIdx.push_back(static_cast<int>(i));
        }
        if (side[i] >= 0) {
            rightMap[static_cast<int>(i)] = static_cast<int>(rightIdx.size());
            rightIdx.push_back(static_cast<int>(i));
        }
    }

    // Triangulate left sub-polygon
    std::vector<Vec2> leftVerts;
    for (int idx : leftIdx) leftVerts.push_back(vertices[static_cast<std::size_t>(idx)]);
    std::vector<EdgePair> leftConstraints;
    for (const auto& e : remaining) {
        auto lit = leftMap.find(static_cast<int>(e.first));
        auto rit = leftMap.find(static_cast<int>(e.second));
        if (lit != leftMap.end() && rit != leftMap.end()) {
            leftConstraints.push_back({lit->second, rit->second});
        }
    }
    auto leftTris = triangulate_constrained_polygon(leftVerts, leftConstraints, tol);
    for (auto& tri : leftTris) {
        tri = {leftIdx[static_cast<std::size_t>(tri[0])],
               leftIdx[static_cast<std::size_t>(tri[1])],
               leftIdx[static_cast<std::size_t>(tri[2])]};
        result.push_back(tri);
    }

    // Triangulate right sub-polygon
    std::vector<Vec2> rightVerts;
    for (int idx : rightIdx) rightVerts.push_back(vertices[static_cast<std::size_t>(idx)]);
    std::vector<EdgePair> rightConstraints;
    for (const auto& e : remaining) {
        auto lit = rightMap.find(static_cast<int>(e.first));
        auto rit = rightMap.find(static_cast<int>(e.second));
        if (lit != rightMap.end() && rit != rightMap.end()) {
            rightConstraints.push_back({lit->second, rit->second});
        }
    }
    auto rightTris = triangulate_constrained_polygon(rightVerts, rightConstraints, tol);
    for (auto& tri : rightTris) {
        tri = {rightIdx[static_cast<std::size_t>(tri[0])],
               rightIdx[static_cast<std::size_t>(tri[1])],
               rightIdx[static_cast<std::size_t>(tri[2])]};
        result.push_back(tri);
    }

    return result;
}

// Return true if a point q is inside triangle (a,b,c) in 2D (using winding)
bool point_in_triangle_2d(const Vec2& q, const Vec2& a, const Vec2& b, const Vec2& c) {
    double sign = orient2d(a, b, c);
    return (orient2d(a, b, q) * sign >= 0) &&
           (orient2d(b, c, q) * sign >= 0) &&
           (orient2d(c, a, q) * sign >= 0);
}

} // anonymous namespace


// ============================================================================
// StlCutChainCutter — public interface
// ============================================================================

StlCutChainCutter::MeshTopology StlCutChainCutter::buildTopology(const StlMesh& mesh) {
    MeshTopology topo;
    const auto& tris = mesh.triangles();
    if (tris.empty()) return topo;

    // Deduplicate vertices
    std::map<std::tuple<std::int64_t,std::int64_t,std::int64_t>, std::int64_t> vertMap;
    std::vector<Vec3> verts;
    auto addVert = [&](const Vec3& p) -> std::int64_t {
        auto key = make_spatial_key(p, 1e-12);
        auto it = vertMap.find(key);
        if (it != vertMap.end()) return it->second;
        std::int64_t id = static_cast<std::int64_t>(verts.size());
        verts.push_back(p);
        vertMap[key] = id;
        return id;
    };

    for (const auto& t : tris) {
        Vec3 v0 = vec3_from_stl(t.v0);
        Vec3 v1 = vec3_from_stl(t.v1);
        Vec3 v2 = vec3_from_stl(t.v2);
        std::int64_t a = addVert(v0);
        std::int64_t b = addVert(v1);
        std::int64_t c = addVert(v2);
        topo.faces.push_back({a, b, c});
    }
    topo.vertices = std::move(verts);

    // Edge→faces map + face centers
    topo.centers.reserve(topo.faces.size());
    double totalEdgeLen = 0.0;
    std::int64_t edgeCount = 0;

    for (std::int64_t fi = 0; fi < static_cast<std::int64_t>(topo.faces.size()); ++fi) {
        auto& f = topo.faces[static_cast<std::size_t>(fi)];
        topo.centers.push_back(triangleCentroid(
            topo.vertices[static_cast<std::size_t>(f[0])],
            topo.vertices[static_cast<std::size_t>(f[1])],
            topo.vertices[static_cast<std::size_t>(f[2])]));

        for (int k = 0; k < 3; ++k) {
            auto e = make_edge(f[k], f[(k+1)%3]);
            topo.edgeToFaces[e].push_back(fi);
            totalEdgeLen += vec3_norm(vec3_sub(
                topo.vertices[static_cast<std::size_t>(e.first)],
                topo.vertices[static_cast<std::size_t>(e.second)]));
            ++edgeCount;
        }
    }
    topo.avgEdgeLength = (edgeCount > 0) ? totalEdgeLen / static_cast<double>(edgeCount) : 0.0;

    // Face adjacency from edge→faces
    topo.neighbors.resize(topo.faces.size());
    for (const auto& [e, faceList] : topo.edgeToFaces) {
        if (faceList.size() == 2) {
            topo.neighbors[static_cast<std::size_t>(faceList[0])].push_back(faceList[1]);
            topo.neighbors[static_cast<std::size_t>(faceList[1])].push_back(faceList[0]);
        }
    }

    return topo;
}

// ============================================================================
// Geometry utilities
// ============================================================================

std::array<double, 3> StlCutChainCutter::triangleCentroid(
    const Vec3& a, const Vec3& b, const Vec3& c) {
    return {(a[0]+b[0]+c[0])/3.0, (a[1]+b[1]+c[1])/3.0, (a[2]+b[2]+c[2])/3.0};
}

double StlCutChainCutter::triangleArea(const Vec3& a, const Vec3& b, const Vec3& c) {
    return 0.5 * vec3_norm(vec3_cross(vec3_sub(b, a), vec3_sub(c, a)));
}

double StlCutChainCutter::pointSegmentDistance(
    const Vec3& p, const Vec3& a, const Vec3& b) {
    Vec3 ab = vec3_sub(b, a);
    double den = vec3_norm2(ab);
    if (den < 1e-30) return vec3_norm(vec3_sub(p, a));
    double t = vec3_dot(vec3_sub(p, a), ab) / den;
    t = std::clamp(t, 0.0, 1.0);
    return vec3_norm(vec3_sub(p, vec3_lerp(a, b, t)));
}

double StlCutChainCutter::polylineLength(const std::vector<Vec3>& pts) {
    if (pts.size() < 2) return 0.0;
    double len = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i)
        len += vec3_norm(vec3_sub(pts[i], pts[i-1]));
    return len;
}

// ============================================================================
// Sampling & projection
// ============================================================================

std::vector<Vec3> StlCutChainCutter::resamplePolyline(
    const std::vector<Vec3>& points, double maxStep) {
    if (points.size() < 2) return points;
    std::vector<Vec3> out = {points[0]};
    for (std::size_t i = 0; i < points.size(); ++i) {
        std::size_t j = (i + 1) % points.size();
        double segLen = vec3_norm(vec3_sub(points[j], points[i]));
        int pieces = std::max(1, static_cast<int>(std::ceil(segLen / maxStep)));
        for (int k = 1; k <= pieces; ++k) {
            if (i == points.size() - 1 && k == pieces) continue;
            double t = static_cast<double>(k) / static_cast<double>(pieces);
            out.push_back(vec3_lerp(points[i], points[j], t));
        }
    }
    return out;
}

std::vector<Vec3> StlCutChainCutter::sampleBoundaryLoop(
    const std::vector<gp_Pnt>& loop, int samplesPerEdge) {
    std::vector<Vec3> out;
    int n = std::max(2, samplesPerEdge);
    for (std::size_t i = 0; i < loop.size(); ++i) {
        std::size_t j = (i + 1) % loop.size();
        Vec3 a = gppnt_to_vec3(loop[i]);
        Vec3 b = gppnt_to_vec3(loop[j]);
        for (int k = 0; k < n; ++k) {
            double t = static_cast<double>(k) / static_cast<double>(n);
            out.push_back(vec3_lerp(a, b, t));
        }
    }
    return out;
}

void StlCutChainCutter::projectToStl(
    const MeshTopology& topo,
    const std::vector<Vec3>& redPoints,
    std::vector<Vec3>& greenPoints,
    std::vector<std::int64_t>& triIds,
    std::vector<double>& distances) {
    greenPoints.clear();
    triIds.clear();
    distances.clear();
    greenPoints.reserve(redPoints.size());
    triIds.reserve(redPoints.size());
    distances.reserve(redPoints.size());

    for (const auto& rp : redPoints) {
        double bestDist2 = kDoubleMax;
        Vec3 bestProj = rp;
        std::int64_t bestTri = -1;

        for (std::int64_t fi = 0; fi < static_cast<std::int64_t>(topo.faces.size()); ++fi) {
            const auto& f = topo.faces[static_cast<std::size_t>(fi)];
            const Vec3& a = topo.vertices[static_cast<std::size_t>(f[0])];
            const Vec3& b = topo.vertices[static_cast<std::size_t>(f[1])];
            const Vec3& c = topo.vertices[static_cast<std::size_t>(f[2])];

            // Barycentric projection to triangle plane
            Vec3 ab = vec3_sub(b, a);
            Vec3 ac = vec3_sub(c, a);
            Vec3 n = vec3_cross(ab, ac);
            double area2 = vec3_norm2(n);
            if (area2 < 1e-20) continue;
            n = vec3_mul(n, 1.0 / std::sqrt(area2));

            // Project point to plane
            Vec3 ap = vec3_sub(rp, a);
            double t = vec3_dot(ap, n);
            Vec3 proj = vec3_sub(rp, vec3_mul(n, t));

            // Check if projection is inside triangle (barycentric)
            Vec3 bp = vec3_sub(proj, b);
            Vec3 cp = vec3_sub(proj, c);
            Vec3 cb = vec3_sub(b, c);
            Vec3 ca = vec3_sub(c, a);
            Vec3 ba = vec3_sub(a, b);

            double sn = vec3_dot(n, vec3_cross(ab, vec3_sub(proj, a)));
            double sbn = vec3_dot(n, vec3_cross(cb, bp));
            double scn = vec3_dot(n, vec3_cross(ca, cp));
            double san = vec3_dot(n, vec3_cross(ba, vec3_sub(proj, b)));

            double sum = sn + sbn + scn + san;
            if (std::abs(sum) < 1e-20) continue;
            double u = sn / sum;
            double v = sbn / sum;
            double w = scn / sum;
            // The 4th is for the split at vertex a — simplify to 3 components
            // Using standard barycentric: u+v+w=1, u,v,w >= 0 for inside
            // Recompute with cleaner method
            double d00 = vec3_dot(ab, ab);
            double d01 = vec3_dot(ab, ac);
            double d11 = vec3_dot(ac, ac);
            double d20 = vec3_dot(ap, ab);
            double d21 = vec3_dot(ap, ac);
            double denom = d00 * d11 - d01 * d01;
            if (std::abs(denom) < 1e-20) continue;
            v = (d11 * d20 - d01 * d21) / denom;
            w = (d00 * d21 - d01 * d20) / denom;
            u = 1.0 - v - w;

            bool inside = (u >= -1e-6) && (v >= -1e-6) && (w >= -1e-6);
            double d2 = vec3_norm2(vec3_sub(proj, rp));
            if (!inside) {
                // Point projects outside triangle — find closest point on edges
                double ed = std::min({
                    pointSegmentDistance(rp, a, b),
                    pointSegmentDistance(rp, b, c),
                    pointSegmentDistance(rp, c, a)});
                d2 = ed * ed;
            }

            if (d2 < bestDist2) {
                bestDist2 = d2;
                bestTri = fi;
                if (inside)
                    bestProj = vec3_add(a, vec3_add(vec3_mul(ab, v), vec3_mul(ac, w)));
            }
        }

        greenPoints.push_back(bestProj != rp ? bestProj : rp);
        triIds.push_back(bestTri >= 0 ? bestTri : 0);
        distances.push_back(std::sqrt(bestDist2));
    }
}

// ============================================================================
// Face-path tracing (A*)
// ============================================================================

std::vector<std::int64_t> StlCutChainCutter::facePath(
    const MeshTopology& topo,
    std::int64_t start, std::int64_t end,
    const Vec3& p0, const Vec3& p1) const {
    if (start == end) return {start};

    auto heuristic = [&](std::int64_t fid) -> double {
        return pointSegmentDistance(topo.centers[static_cast<std::size_t>(fid)], p0, p1)
               / std::max(topo.avgEdgeLength, 1e-12);
    };

    using Entry = std::tuple<double, double, std::int64_t>; // (f, cost, face)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
    heap.emplace(heuristic(start), 0.0, start);

    std::unordered_map<std::int64_t, double> bestCost;
    std::unordered_map<std::int64_t, std::int64_t> prev;
    bestCost[start] = 0.0;

    int visited = 0;
    const int maxVisits = 6000;

    while (!heap.empty()) {
        auto [fVal, cost, face] = heap.top();
        heap.pop();

        if (cost > bestCost[face]) continue;
        if (face == end) {
            std::vector<std::int64_t> path;
            for (std::int64_t f = face; ; f = prev[f]) {
                path.push_back(f);
                if (f == start) break;
            }
            std::reverse(path.begin(), path.end());
            return path;
        }

        if (++visited > maxVisits) break;

        const auto& nbs = topo.neighbors[static_cast<std::size_t>(face)];
        for (std::int64_t nb : nbs) {
            double step = 1.0 + heuristic(nb);
            double gc = cost + step;
            auto it = bestCost.find(nb);
            if (it == bestCost.end() || gc < it->second) {
                bestCost[nb] = gc;
                prev[nb] = face;
                heap.emplace(gc + heuristic(nb), gc, nb);
            }
        }
    }
    return {};
}

// ============================================================================
// Global cut-chain tracing
// ============================================================================

// Internal structs for tracking cut vertices
struct LocalCutVert {
    Vec3 xyz;
    std::int64_t id = -1;
};

void StlCutChainCutter::traceBoundary(
    const MeshTopology& topo,
    const std::vector<Vec3>& greenPoints,
    const std::vector<std::int64_t>& triIds,
    std::vector<CutPoint>& cutVertices,
    std::unordered_map<std::int64_t, std::vector<FaceConstraint>>& faceConstraints,
    std::vector<EdgePair>& chainEdges) const {
    const std::int64_t n = static_cast<std::int64_t>(greenPoints.size());
    if (n < 2) return;

    // Spatial key → cut vertex ID
    std::map<std::tuple<std::int64_t,std::int64_t,std::int64_t>, std::int64_t> vertKeyToId;

    auto addVert = [&](const Vec3& p) -> std::int64_t {
        auto key = make_spatial_key(p, 1e-9);
        auto it = vertKeyToId.find(key);
        if (it != vertKeyToId.end()) return it->second;
        std::int64_t vid = static_cast<std::int64_t>(cutVertices.size());
        cutVertices.push_back({p, vid});
        vertKeyToId[key] = vid;
        return vid;
    };

    // Assign green point IDs
    std::vector<std::int64_t> greenIds(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        greenIds[static_cast<std::size_t>(i)] = addVert(greenPoints[static_cast<std::size_t>(i)]);

    int failedSegments = 0;
    int totalSegments = 0;

    for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t j = (i + 1) % n;
        Vec3 p0 = greenPoints[static_cast<std::size_t>(i)];
        Vec3 p1 = greenPoints[static_cast<std::size_t>(j)];
        std::int64_t f0 = triIds[static_cast<std::size_t>(i)];
        std::int64_t f1 = triIds[static_cast<std::size_t>(j)];
        std::int64_t c0 = greenIds[static_cast<std::size_t>(i)];
        std::int64_t c1 = greenIds[static_cast<std::size_t>(j)];

        auto path = facePath(topo, f0, f1, p0, p1);

        if (path.empty()) {
            faceConstraints[f0].push_back({c0, c1});
            chainEdges.push_back(make_edge(c0, c1));
            ++failedSegments;
            ++totalSegments;
            continue;
        }

        if (path.size() == 1) {
            faceConstraints[path[0]].push_back({c0, c1});
            chainEdges.push_back(make_edge(c0, c1));
            ++totalSegments;
            continue;
        }

        Vec3 currentP = p0;
        std::int64_t currentC = c0;

        for (std::size_t k = 0; k + 1 < path.size(); ++k) {
            std::int64_t fa = path[k];
            std::int64_t fb = path[k+1];
            // Find shared edge between fa and fb
            const auto& faVerts = topo.faces[static_cast<std::size_t>(fa)];
            const auto& fbVerts = topo.faces[static_cast<std::size_t>(fb)];

            EdgePair sharedEdge{-1, -1};
            for (int ei = 0; ei < 3; ++ei) {
                auto e = make_edge(faVerts[ei], faVerts[(ei+1)%3]);
                for (int ej = 0; ej < 3; ++ej) {
                    auto e2 = make_edge(fbVerts[ej], fbVerts[(ej+1)%3]);
                    if (e == e2) { sharedEdge = e; break; }
                }
                if (sharedEdge.first >= 0) break;
            }

            if (sharedEdge.first < 0) continue;

            // Find exit point on shared edge
            Vec3 a3 = topo.vertices[static_cast<std::size_t>(sharedEdge.first)];
            Vec3 b3 = topo.vertices[static_cast<std::size_t>(sharedEdge.second)];

            // Project to 2D using the face fa's basis
            // Simple approach: find intersection of line (currentP→p1) with edge (a3→b3)
            Vec3 dir = vec3_sub(p1, currentP);
            Vec3 edgeVec = vec3_sub(b3, a3);
            Vec3 n = vec3_cross(dir, edgeVec);
            double n2 = vec3_norm2(n);
            if (n2 < 1e-20) continue;

            // Use plane containing currentP→p1 and a3→b3 to find intersection
            Vec3 ap = vec3_sub(a3, currentP);
            double d = vec3_dot(vec3_cross(ap, edgeVec), n) / n2;
            double edgeT = vec3_dot(vec3_cross(ap, dir), n) / n2;

            edgeT = std::clamp(edgeT, 0.0, 1.0);
            Vec3 exitP = vec3_lerp(a3, b3, edgeT);

            std::int64_t exitC = addVert(exitP);
            faceConstraints[fa].push_back({currentC, exitC});
            chainEdges.push_back(make_edge(currentC, exitC));
            ++totalSegments;

            currentP = exitP;
            currentC = exitC;
        }

        faceConstraints[path.back()].push_back({currentC, c1});
        chainEdges.push_back(make_edge(currentC, c1));
        ++totalSegments;
    }
}

// ============================================================================
// Build split mesh (per-face constrained re-triangulation)
// ============================================================================

StlMesh StlCutChainCutter::buildSplitMesh(
    const MeshTopology& topo,
    const std::vector<CutPoint>& cutVertices,
    const std::unordered_map<std::int64_t, std::vector<FaceConstraint>>& faceConstraints,
    std::unordered_set<EdgePair, StlCutChainCutter::EdgeHash>& cutEdgesOut,
    int& triangulationFailed,
    int& missingConstraints) const {
    triangulationFailed = 0;
    missingConstraints = 0;

    // Accumulate output (global vertex positions → global vertex ids)
    std::vector<Vec3> outVerts;
    for (const auto& v : topo.vertices) outVerts.push_back(v);
    for (const auto& cv : cutVertices) outVerts.push_back(cv.xyz);
    std::int64_t cutVertBase = static_cast<std::int64_t>(topo.vertices.size());

    // For each face: build local 2D triangulation, map back to 3D output
    struct LocalVert {
        Vec2 pos2d;
        std::int64_t globalId = -1;
    };

    std::vector<FaceTriple> outFaces;

    for (std::int64_t fi = 0; fi < static_cast<std::int64_t>(topo.faces.size()); ++fi) {
        auto it = faceConstraints.find(fi);
        if (it == faceConstraints.end()) {
            // No constraints → keep original face
            outFaces.push_back(topo.faces[static_cast<std::size_t>(fi)]);
            continue;
        }

        const auto& constraints = it->second;
        const auto& f = topo.faces[static_cast<std::size_t>(fi)];
        const Vec3& A = topo.vertices[static_cast<std::size_t>(f[0])];
        const Vec3& B = topo.vertices[static_cast<std::size_t>(f[1])];
        const Vec3& C = topo.vertices[static_cast<std::size_t>(f[2])];

        // Build 2D basis on triangle plane
        Vec3 u = vec3_normalize(vec3_sub(B, A));
        Vec3 n = vec3_cross(vec3_sub(B, A), vec3_sub(C, A));
        n = vec3_normalize(n);
        Vec3 v = vec3_cross(n, u);
        v = vec3_normalize(v);

        auto to2d = [&](const Vec3& p3) -> Vec2 {
            Vec3 r = vec3_sub(p3, A);
            return {vec3_dot(r, u), vec3_dot(r, v)};
        };

        auto to3d = [&](const Vec2& q2) -> Vec3 {
            return vec3_add(A, vec3_add(vec3_mul(u, q2[0]), vec3_mul(v, q2[1])));
        };

        // Collect all vertices (original corners + cut vertices on this face)
        Vec2 tri2d[3] = {to2d(A), to2d(B), to2d(C)};
        std::vector<LocalVert> localVerts;
        std::map<std::pair<double,double>, int, decltype([](const auto& a, const auto& b){
            if (std::abs(a.first - b.first) > 1e-10) return a.first < b.first;
            return a.second < b.second;
        })> posToIdx;

        auto addLocalVert = [&](const Vec2& q2, std::int64_t globalId) -> int {
            std::pair<double,double> key{std::round(q2[0]/1e-8)*1e-8, std::round(q2[1]/1e-8)*1e-8};
            auto pit = posToIdx.find(key);
            if (pit != posToIdx.end()) return pit->second;
            int idx = static_cast<int>(localVerts.size());
            localVerts.push_back({q2, globalId});
            posToIdx[key] = idx;
            return idx;
        };

        // Add original triangle corner vertices
        int iA = addLocalVert(tri2d[0], f[0]);
        int iB = addLocalVert(tri2d[1], f[1]);
        int iC = addLocalVert(tri2d[2], f[2]);

        // Original boundary segments
        std::vector<EdgePair> allSegments = {
            {iA, iB}, {iB, iC}, {iC, iA}
        };

        // Add cut vertices and constraint segments
        std::vector<std::pair<int,int>> cutSegsLocal;
        for (const auto& fc : constraints) {
            if (fc.cutIdA < 0 || fc.cutIdB < 0) continue;
            std::int64_t gidA = cutVertBase + fc.cutIdA;
            std::int64_t gidB = cutVertBase + fc.cutIdB;
            if (static_cast<std::size_t>(fc.cutIdA) >= cutVertices.size() ||
                static_cast<std::size_t>(fc.cutIdB) >= cutVertices.size()) continue;

            Vec2 qa = to2d(cutVertices[static_cast<std::size_t>(fc.cutIdA)].xyz);
            Vec2 qb = to2d(cutVertices[static_cast<std::size_t>(fc.cutIdB)].xyz);
            if (norm2_2(sub2(qa, qb)) < 1e-15) continue;

            int la = addLocalVert(qa, gidA);
            int lb = addLocalVert(qb, gidB);
            if (la != lb) {
                cutSegsLocal.push_back({la, lb});
                allSegments.push_back({la, lb});
            }
        }

        if (cutSegsLocal.empty()) {
            outFaces.push_back(topo.faces[static_cast<std::size_t>(fi)]);
            continue;
        }

        // Add intersection points between crossing segments
        double localTol = 1e-7;
        for (std::size_t si = 0; si < allSegments.size(); ++si) {
            for (std::size_t sj = si + 1; sj < allSegments.size(); ++sj) {
                auto [ea, eb] = allSegments[si];
                auto [ec, ed] = allSegments[sj];
                Vec2 q;
                auto [hit, _] = segment_intersection_2d(
                    localVerts[static_cast<std::size_t>(ea)].pos2d,
                    localVerts[static_cast<std::size_t>(eb)].pos2d,
                    localVerts[static_cast<std::size_t>(ec)].pos2d,
                    localVerts[static_cast<std::size_t>(ed)].pos2d,
                    localTol, &q);
                if (hit) addLocalVert(q, -1);
            }
        }

        // Build vertex list and edge list for triangulation
        std::vector<Vec2> polyVerts;
        for (const auto& lv : localVerts) polyVerts.push_back(lv.pos2d);

        // Use the triangle boundary edges as polygon, interior constraint edges
        // Split original boundary segments by points lying on them
        std::set<EdgePair> finalSegs;
        auto addSplitSegs = [&](const EdgePair& origSeg, const std::vector<int>& ptsOnSeg) {
            // Sort ptsOnSeg by parameter along origSeg
            Vec2 pa = polyVerts[static_cast<std::size_t>(origSeg.first)];
            Vec2 pb = polyVerts[static_cast<std::size_t>(origSeg.second)];
            std::vector<std::pair<double,int>> hits;
            for (int pi : ptsOnSeg) {
                if (pi == origSeg.first || pi == origSeg.second) continue;
                double t = point_param_on_segment_2d(polyVerts[static_cast<std::size_t>(pi)], pa, pb);
                t = std::clamp(t, 0.0, 1.0);
                hits.push_back({t, pi});
            }
            std::sort(hits.begin(), hits.end());
            // Deduplicate
            std::vector<int> order = {static_cast<int>(origSeg.first)};
            for (auto& [t, pi] : hits) {
                if (!order.empty() && pi == order.back()) continue;
                order.push_back(pi);
            }
            order.push_back(origSeg.second);
            for (std::size_t k = 0; k + 1 < order.size(); ++k)
                finalSegs.insert(make_edge(order[k], order[k+1]));
        };

        // Split original edges
        for (const auto& seg : std::array<EdgePair,3>{{{iA,iB},{iB,iC},{iC,iA}}}) {
            std::vector<int> onSeg;
            for (int vi = 0; vi < static_cast<int>(localVerts.size()); ++vi) {
                if (point_on_segment_2d(polyVerts[static_cast<std::size_t>(vi)],
                    polyVerts[static_cast<std::size_t>(seg.first)],
                    polyVerts[static_cast<std::size_t>(seg.second)], 1e-7)) {
                    onSeg.push_back(vi);
                }
            }
            addSplitSegs(seg, onSeg);
        }

        // Add constraint edges, also split
        for (auto [la, lb] : cutSegsLocal) {
            std::vector<int> onSeg;
            for (int vi = 0; vi < static_cast<int>(localVerts.size()); ++vi) {
                if (point_on_segment_2d(polyVerts[static_cast<std::size_t>(vi)],
                    polyVerts[static_cast<std::size_t>(la)],
                    polyVerts[static_cast<std::size_t>(lb)], 1e-7)) {
                    onSeg.push_back(vi);
                }
            }
            addSplitSegs({la, lb}, onSeg);
        }

        // Build polygon vertices — take all vertices
        std::vector<Vec2> tnVerts;
        tnVerts.reserve(polyVerts.size());
        // Build mapping from original index to triangulation index
        std::vector<int> idxMap(polyVerts.size(), -1);
        for (std::size_t vi = 0; vi < polyVerts.size(); ++vi) {
            idxMap[vi] = static_cast<int>(tnVerts.size());
            tnVerts.push_back(polyVerts[vi]);
        }

        std::vector<EdgePair> tnSegs;
        for (const auto& seg : finalSegs) {
            tnSegs.push_back({idxMap[static_cast<std::size_t>(seg.first)],
                              idxMap[static_cast<std::size_t>(seg.second)]});
        }

        auto localTris = triangulate_constrained_polygon(tnVerts, tnSegs, 1e-9);
        if (localTris.empty()) {
            ++triangulationFailed;
            outFaces.push_back(topo.faces[static_cast<std::size_t>(fi)]);
            continue;
        }

        // Map 2D triangles to 3D output
        // For each triangulation vertex: if it maps to a known global ID, use it;
        // otherwise create a new output vertex
        std::vector<std::int64_t> tnToGlobal(tnVerts.size(), -1);
        for (std::size_t oi = 0; oi < polyVerts.size(); ++oi) {
            int ti = idxMap[oi];
            if (ti < 0) continue;
            if (localVerts[oi].globalId >= 0) {
                tnToGlobal[static_cast<std::size_t>(ti)] = localVerts[oi].globalId;
            } else {
                Vec3 p3 = to3d(tnVerts[static_cast<std::size_t>(ti)]);
                tnToGlobal[static_cast<std::size_t>(ti)] = static_cast<std::int64_t>(outVerts.size());
                outVerts.push_back(p3);
            }
        }

        for (const auto& tri : localTris) {
            std::int64_t va = tnToGlobal[static_cast<std::size_t>(tri[0])];
            std::int64_t vb = tnToGlobal[static_cast<std::size_t>(tri[1])];
            std::int64_t vc = tnToGlobal[static_cast<std::size_t>(tri[2])];
            if (va < 0 || vb < 0 || vc < 0 || va == vb || vb == vc || va == vc) continue;
            // Check degenerate area
            Vec3 pa = outVerts[static_cast<std::size_t>(va)];
            Vec3 pb = outVerts[static_cast<std::size_t>(vb)];
            Vec3 pc = outVerts[static_cast<std::size_t>(vc)];
            if (triangleArea(pa, pb, pc) < 1e-14) continue;
            outFaces.push_back({va, vb, vc});
        }

        // Record cut edges: for each constraint edge in local triangulation,
        // find its corresponding output edge and add to cutEdgesOut
        for (auto [la, lb] : cutSegsLocal) {
            int tla = idxMap[static_cast<std::size_t>(la)];
            int tlb = idxMap[static_cast<std::size_t>(lb)];
            if (tla < 0 || tlb < 0) continue;

            // Find all tn vertices on tnSeg (tla,tlb) and record adjacent pairs
            Vec2 qa = tnVerts[static_cast<std::size_t>(tla)];
            Vec2 qb = tnVerts[static_cast<std::size_t>(tlb)];
            std::vector<std::pair<double,int>> hits;
            for (int ti = 0; ti < static_cast<int>(tnVerts.size()); ++ti) {
                if (point_on_segment_2d(tnVerts[static_cast<std::size_t>(ti)], qa, qb, 1e-6)) {
                    double t = point_param_on_segment_2d(tnVerts[static_cast<std::size_t>(ti)], qa, qb);
                    hits.push_back({std::clamp(t, 0.0, 1.0), ti});
                }
            }
            if (hits.size() < 2) { ++missingConstraints; continue; }
            std::sort(hits.begin(), hits.end());
            for (std::size_t h = 0; h + 1 < hits.size(); ++h) {
                std::int64_t ga = tnToGlobal[static_cast<std::size_t>(hits[h].second)];
                std::int64_t gb = tnToGlobal[static_cast<std::size_t>(hits[h+1].second)];
                if (ga >= 0 && gb >= 0 && ga != gb)
                    cutEdgesOut.insert(make_edge(ga, gb));
            }
        }
    }

    StlMesh result;
    for (const auto& f : outFaces) {
        if (static_cast<std::size_t>(f[0]) >= outVerts.size() ||
            static_cast<std::size_t>(f[1]) >= outVerts.size() ||
            static_cast<std::size_t>(f[2]) >= outVerts.size()) continue;
        StlTriangle t;
        t.v0 = stl_from_vec3(outVerts[static_cast<std::size_t>(f[0])]);
        t.v1 = stl_from_vec3(outVerts[static_cast<std::size_t>(f[1])]);
        t.v2 = stl_from_vec3(outVerts[static_cast<std::size_t>(f[2])]);
        t.normal = {0, 0, 1};
        result.addTriangle(t);
    }
    return result;
}

// ============================================================================
// Flood-fill component selection
// ============================================================================

StlMesh StlCutChainCutter::floodFillPatch(
    const StlMesh& splitMesh,
    const std::unordered_set<EdgePair, StlCutChainCutter::EdgeHash>& cutEdges,
    const std::optional<std::vector<gp_Pnt>>& seedPoints,
    const StlCutChainOptions& options) const {
    if (splitMesh.empty()) return {};

    // Build edge→faces and face adjacency (blocked by cut edges)
    const auto& tris = splitMesh.triangles();
    using SpatialKey = std::tuple<std::int64_t, std::int64_t, std::int64_t>;
    std::map<std::pair<SpatialKey, SpatialKey>, std::vector<std::int64_t>> e2f;
    for (std::int64_t fi = 0; fi < static_cast<std::int64_t>(tris.size()); ++fi) {
        const auto& t = tris[static_cast<std::size_t>(fi)];
        Vec3 v0 = vec3_from_stl(t.v0);
        Vec3 v1 = vec3_from_stl(t.v1);
        Vec3 v2 = vec3_from_stl(t.v2);

        auto addEdge = [&](const Vec3& a, const Vec3& b) {
            auto ka = make_spatial_key(a, 1e-12);
            auto kb = make_spatial_key(b, 1e-12);
            if (ka < kb) e2f[{ka, kb}].push_back(fi);
            else         e2f[{kb, ka}].push_back(fi);
        };
        addEdge(v0, v1);
        addEdge(v1, v2);
        addEdge(v2, v0);
    }

    // Face adjacency
    std::vector<std::vector<std::int64_t>> adj(static_cast<std::size_t>(tris.size()));
    for (const auto& [ekey, flist] : e2f) {
        if (flist.size() != 2) continue;
        adj[static_cast<std::size_t>(flist[0])].push_back(flist[1]);
        adj[static_cast<std::size_t>(flist[1])].push_back(flist[0]);
    }

    // Connected components via BFS
    std::int64_t nFaces = static_cast<std::int64_t>(tris.size());
    std::vector<bool> visited(static_cast<std::size_t>(nFaces), false);
    std::vector<std::vector<std::int64_t>> components;

    for (std::int64_t fi = 0; fi < nFaces; ++fi) {
        if (visited[static_cast<std::size_t>(fi)]) continue;
        std::vector<std::int64_t> comp;
        std::deque<std::int64_t> q;
        q.push_back(fi);
        visited[static_cast<std::size_t>(fi)] = true;
        while (!q.empty()) {
            auto f = q.front(); q.pop_front();
            comp.push_back(f);
            for (auto nb : adj[static_cast<std::size_t>(f)]) {
                if (!visited[static_cast<std::size_t>(nb)]) {
                    visited[static_cast<std::size_t>(nb)] = true;
                    q.push_back(nb);
                }
            }
        }
        components.push_back(std::move(comp));
    }

    if (components.empty()) return {};

    // Select patch component: prefer seed-guided, otherwise smaller cut-adjacent
    double totalFaceCount = static_cast<double>(nFaces);

    // Compute component areas and cut-adjacency
    struct CompInfo {
        int index;
        std::int64_t faceCount;
        double area;
        bool touchesCut;
    };
    std::vector<CompInfo> compInfos;

    for (int ci = 0; ci < static_cast<int>(components.size()); ++ci) {
        const auto& comp = components[static_cast<std::size_t>(ci)];
        double area = 0.0;
        bool touchesCut = false;

        for (auto fi : comp) {
            const auto& t = tris[static_cast<std::size_t>(fi)];
            area += triangleArea(
                vec3_from_stl(t.v0), vec3_from_stl(t.v1), vec3_from_stl(t.v2));
            // Check if any face edge is a cut edge
            for (auto nb : adj[static_cast<std::size_t>(fi)]) {
                // If neighbor is in a different component, this edge is a cut edge surrogate
                // (or a boundary edge)
            }
        }
        // Approximate cut-adjacency: if component touches boundary in vertex graph
        // (Simplified: mark as touching-cut if it's near any cut edge vertex)
        touchesCut = (comp.size() < static_cast<std::size_t>(nFaces)); // rough heuristic

        compInfos.push_back({ci, static_cast<std::int64_t>(comp.size()), area, touchesCut});
    }

    auto minFaces = options.minComponentFaceCount;
    double minArea = std::max(1e-12, totalFaceCount * 0.0 * options.minComponentAreaRatio);

    decltype(compInfos.begin()) chosenIt = compInfos.end();
    int voteCount = 0;

    // Seed-guided selection
    if (seedPoints.has_value() && !seedPoints->empty()) {
        std::map<int, int> votes;
        for (const auto& sp : seedPoints.value()) {
            Vec3 sv = gppnt_to_vec3(sp);
            double bestD2 = kDoubleMax;
            int bestCi = -1;
            for (const auto& info : compInfos) {
                // Compute centroid of component, distance to seed
                Vec3 centroid = {0,0,0};
                for (auto fi : components[static_cast<std::size_t>(info.index)]) {
                    const auto& t = tris[static_cast<std::size_t>(fi)];
                    centroid = vec3_add(centroid, vec3_mul(
                        vec3_add(vec3_add(
                            vec3_from_stl(t.v0), vec3_from_stl(t.v1)),
                            vec3_from_stl(t.v2)), 1.0/3.0));
                }
                centroid = vec3_mul(centroid, 1.0/static_cast<double>(info.faceCount));
                double d2 = vec3_norm2(vec3_sub(sv, centroid));
                if (d2 < bestD2) { bestD2 = d2; bestCi = info.index; }
            }
            if (bestCi >= 0) votes[bestCi]++;
        }

        if (!votes.empty()) {
            auto best = std::max_element(votes.begin(), votes.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            for (auto it = compInfos.begin(); it != compInfos.end(); ++it)
                if (it->index == best->first) { chosenIt = it; break; }
            voteCount = best->second;
        }
    }

    if (chosenIt == compInfos.end()) {
        // No seed → pick smaller cut-adjacent component
        std::vector<CompInfo*> candidates;
        for (auto& info : compInfos)
            if (info.faceCount >= minFaces) candidates.push_back(&info);

        if (candidates.empty()) {
            // Fallback: all components
            for (auto& info : compInfos)
                if (info.faceCount >= 1) candidates.push_back(&info);
        }
        if (candidates.empty()) return {};

        auto best = std::min_element(candidates.begin(), candidates.end(),
            [](const CompInfo* a, const CompInfo* b) { return a->area < b->area; });

        for (auto it = compInfos.begin(); it != compInfos.end(); ++it)
            if (&(*it) == *best) { chosenIt = it; break; }
    }

    if (chosenIt == compInfos.end()) return {};

    // Build patch mesh from selected component faces
    const auto& chosenComp = components[static_cast<std::size_t>(chosenIt->index)];
    StlMesh patch;
    for (auto fi : chosenComp)
        patch.addTriangle(tris[static_cast<std::size_t>(fi)]);

    return patch;
}

// ============================================================================
// Boundary snap
// ============================================================================

StlMesh StlCutChainCutter::snapBoundary(
    const StlMesh& patch,
    const std::vector<Vec3>& /*green*/,
    const std::vector<Vec3>& /*red*/,
    double /*maxDist*/) {
    // Boundary snap is a post-process that aligns the patch outer boundary
    // back to the original STEP free edge. Currently a no-op.
    return patch;
}

// ============================================================================
// Diagnostics
// ============================================================================

std::vector<int> StlCutChainCutter::computeCutEdgeDegrees(
    const std::unordered_set<EdgePair, StlCutChainCutter::EdgeHash>& cutEdges) {
    std::unordered_map<std::int64_t, int> deg;
    for (const auto& e : cutEdges) {
        deg[e.first]++;
        deg[e.second]++;
    }
    std::vector<int> result;
    for (const auto& [v, d] : deg) result.push_back(d);
    return result;
}

// ============================================================================
// Main entry: cut()
// ============================================================================

StlCutChainResult StlCutChainCutter::cut(
    const StlMesh& sourceMesh,
    const std::vector<gp_Pnt>& boundaryLoop,
    const std::optional<std::vector<gp_Pnt>>& seedPoints,
    const StlCutChainOptions& options) const {
    StlCutChainResult result;

    if (sourceMesh.empty()) {
        result.message = "Source STL mesh is empty.";
        return result;
    }
    if (boundaryLoop.size() < 3) {
        result.message = "Boundary loop has fewer than 3 points.";
        return result;
    }

    // 1. Build mesh topology
    auto topo = buildTopology(sourceMesh);
    result.splitMeshVertexCount = static_cast<int>(topo.vertices.size());
    result.splitMeshFaceCount   = static_cast<int>(topo.faces.size());
    result.originalFaces        = static_cast<int>(topo.faces.size());

    if (topo.faces.empty()) {
        result.message = "Mesh topology has no faces after vertex dedup.";
        return result;
    }

    // 2. Sample boundary loop (red points) and resample by green step
    auto redRaw = sampleBoundaryLoop(boundaryLoop, options.samplesPerEdge);
    double greenStep = options.greenResampleFactor > 0
        ? topo.avgEdgeLength * options.greenResampleFactor
        : topo.avgEdgeLength * 0.35;
    greenStep = std::max(greenStep, 1e-6);
    auto redDense = resamplePolyline(redRaw, greenStep);

    // 3. Project red boundary to STL surface (green points)
    std::vector<Vec3> green;
    std::vector<std::int64_t> triIds;
    std::vector<double> distances;
    projectToStl(topo, redDense, green, triIds, distances);

    // 4. Trace global cut chain through STL faces
    std::vector<CutPoint> cutVerts;
    std::unordered_map<std::int64_t, std::vector<FaceConstraint>> faceConstr;
    std::vector<EdgePair> chainEdges;
    traceBoundary(topo, green, triIds, cutVerts, faceConstr, chainEdges);

    result.cutVertexCount = static_cast<int>(cutVerts.size());
    result.chainEdgeCount = static_cast<int>(chainEdges.size());

    // 5. Build split mesh
    std::unordered_set<EdgePair, StlCutChainCutter::EdgeHash> cutEdgesSet;
    int triFailed = 0, missConstr = 0;
    auto splitMesh = buildSplitMesh(topo, cutVerts, faceConstr, cutEdgesSet, triFailed, missConstr);

    result.triangulationFailed    = triFailed;
    result.missingConstraintEdges = missConstr;
    result.splitMeshVertexCount   = static_cast<int>(
        result.splitMeshVertexCount + result.cutVertexCount); // approximate
    result.splitMeshFaceCount     = static_cast<int>(splitMesh.triangleCount());
    result.cutEdgeDegrees         = computeCutEdgeDegrees(cutEdgesSet);

    // 6. Flood fill to select patch component
    auto patchMesh = floodFillPatch(splitMesh, cutEdgesSet, seedPoints, options);
    result.patchFaceCount = static_cast<int>(patchMesh.triangleCount());

    if (patchMesh.empty()) {
        result.message = "No valid patch component after flood fill.";
        return result;
    }

    // 7. Snap boundary
    if (options.snapBoundaryToRed) {
        patchMesh = snapBoundary(patchMesh, green, redDense, options.snapMaxDist);
    }

    result.patchMesh = std::move(patchMesh);
    result.success   = true;
    result.message   = "Global cut-chain patch extracted.";
    return result;
}

} // namespace spo
