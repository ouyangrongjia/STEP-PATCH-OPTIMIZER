#include "merge/MergeRegionGrower.h"

#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopAbs_Orientation.hxx>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <vector>

namespace spo {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct FaceSample {
    gp_Pnt center;
    gp_Dir normal;
    double area = 0.0;
};

double clampCosine(double value) {
    return std::max(-1.0, std::min(1.0, value));
}

std::optional<FaceSample> sampleFace(const TopoDS_Face& face) {
    if (face.IsNull()) {
        return std::nullopt;
    }

    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    if (!std::isfinite(uMin) || !std::isfinite(uMax) || !std::isfinite(vMin) || !std::isfinite(vMax)) {
        return std::nullopt;
    }

    BRepAdaptor_Surface surface(face);
    BRepLProp_SLProps props(surface, (uMin + uMax) * 0.5, (vMin + vMax) * 0.5, 1, Precision::Confusion());
    if (!props.IsNormalDefined()) {
        return std::nullopt;
    }

    FaceSample sample;
    sample.center = props.Value();
    sample.normal = props.Normal();
    if (face.Orientation() == TopAbs_REVERSED) {
        sample.normal.Reverse();
    }

    GProp_GProps areaProps;
    BRepGProp::SurfaceProperties(face, areaProps);
    sample.area = areaProps.Mass();
    return sample;
}

double distancePointToPlane(const gp_Pnt& point, const gp_Pnt& planePoint, const gp_Dir& planeNormal) {
    return std::abs(gp_Vec(planePoint, point).Dot(gp_Vec(planeNormal)));
}

double angleBetweenNormalsDeg(const gp_Dir& first, const gp_Dir& second) {
    const auto dot = std::abs(clampCosine(first.Dot(second)));
    return std::acos(dot) * 180.0 / kPi;
}

std::optional<EdgeId> sharedEdgeBetweenFaces(const TopologyGraph& topology, FaceId first, FaceId second) {
    const auto firstEdges = topology.edgesForFace(first);
    for (const auto edge : firstEdges) {
        const auto* adjacency = topology.adjacencyForEdge(edge);
        if (adjacency == nullptr) {
            continue;
        }
        if (std::find(adjacency->faces.begin(), adjacency->faces.end(), second) != adjacency->faces.end()) {
            return edge;
        }
    }
    return std::nullopt;
}

bool containsFace(const std::set<FaceId>& faces, const EdgeAdjacency& adjacency) {
    if (adjacency.faces.empty()) {
        return false;
    }

    for (const auto face : adjacency.faces) {
        if (!faces.contains(face)) {
            return false;
        }
    }
    return true;
}

}

std::vector<MergeCandidate> MergeRegionGrower::growPlaneLikeRegions(
    const ShapeDocument& document,
    const std::set<EdgeId>& protectedEdges,
    const MergePlannerOptions& options,
    int* visitedFaces,
    int* rejectedRegions) const {
    std::vector<MergeCandidate> candidates;
    const auto& topology = document.topology();
    std::vector<bool> visited(topology.faceCount(), false);
    int visitedCount = 0;
    int rejectedCount = 0;

    for (FaceId seed = 0; seed < topology.faceCount(); ++seed) {
        if (visited[seed]) {
            continue;
        }

        const auto seedSample = sampleFace(topology.face(seed));
        if (!seedSample.has_value()) {
            visited[seed] = true;
            ++visitedCount;
            ++rejectedCount;
            continue;
        }

        std::queue<FaceId> queue;
        std::set<FaceId> regionFaces;
        std::set<EdgeId> blockedEdges;
        std::vector<double> distances;
        std::vector<double> normalAngles;

        visited[seed] = true;
        ++visitedCount;
        queue.push(seed);
        regionFaces.insert(seed);
        distances.push_back(0.0);
        normalAngles.push_back(0.0);

        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();

            for (const auto neighbor : topology.adjacentFaces(current)) {
                const auto sharedEdge = sharedEdgeBetweenFaces(topology, current, neighbor);
                if (sharedEdge.has_value() && protectedEdges.contains(*sharedEdge)) {
                    blockedEdges.insert(*sharedEdge);
                    continue;
                }

                if (visited[neighbor]) {
                    continue;
                }

                const auto neighborSample = sampleFace(topology.face(neighbor));
                if (!neighborSample.has_value()) {
                    continue;
                }

                const auto normalAngle = angleBetweenNormalsDeg(seedSample->normal, neighborSample->normal);
                const auto distance = distancePointToPlane(neighborSample->center, seedSample->center, seedSample->normal);
                if (normalAngle > options.max_normal_angle_degrees || distance > options.max_plane_distance) {
                    continue;
                }

                visited[neighbor] = true;
                ++visitedCount;
                queue.push(neighbor);
                regionFaces.insert(neighbor);
                distances.push_back(distance);
                normalAngles.push_back(normalAngle);
            }
        }

        if (static_cast<int>(regionFaces.size()) < options.min_region_faces) {
            ++rejectedCount;
            continue;
        }

        MergeCandidate candidate;
        candidate.candidate_id = static_cast<int>(candidates.size());
        candidate.candidate_type = MergeCandidateType::PlaneLike;
        candidate.faces.assign(regionFaces.begin(), regionFaces.end());
        candidate.face_count = static_cast<int>(candidate.faces.size());
        candidate.blocked_edges.assign(blockedEdges.begin(), blockedEdges.end());
        candidate.protected_edges = candidate.blocked_edges;

        std::set<EdgeId> internalEdges;
        std::set<EdgeId> boundaryEdges;
        for (const auto face : candidate.faces) {
            const auto faceSample = sampleFace(topology.face(face));
            if (faceSample.has_value()) {
                candidate.total_area += faceSample->area;
            }

            for (const auto edge : topology.edgesForFace(face)) {
                const auto* adjacency = topology.adjacencyForEdge(edge);
                if (adjacency != nullptr && adjacency->faces.size() == 2 && containsFace(regionFaces, *adjacency)) {
                    internalEdges.insert(edge);
                } else {
                    boundaryEdges.insert(edge);
                }
            }
        }

        candidate.internal_edges.assign(internalEdges.begin(), internalEdges.end());
        candidate.boundary_edges.assign(boundaryEdges.begin(), boundaryEdges.end());
        candidate.internal_edge_count = static_cast<int>(candidate.internal_edges.size());
        candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());
        candidate.max_distance = distances.empty() ? 0.0 : *std::max_element(distances.begin(), distances.end());
        candidate.mean_distance = distances.empty()
            ? 0.0
            : std::accumulate(distances.begin(), distances.end(), 0.0) / distances.size();
        candidate.max_normal_angle_deg = normalAngles.empty()
            ? 0.0
            : *std::max_element(normalAngles.begin(), normalAngles.end());
        candidate.mean_normal_angle_deg = normalAngles.empty()
            ? 0.0
            : std::accumulate(normalAngles.begin(), normalAngles.end(), 0.0) / normalAngles.size();
        candidate.fit_error = candidate.max_distance;
        candidate.risk_level = candidate.max_normal_angle_deg <= options.max_normal_angle_degrees * 0.5
            ? MergeRiskLevel::Low
            : MergeRiskLevel::Medium;

        candidates.push_back(std::move(candidate));
    }

    if (visitedFaces != nullptr) {
        *visitedFaces = visitedCount;
    }
    if (rejectedRegions != nullptr) {
        *rejectedRegions = rejectedCount;
    }
    return candidates;
}

}
