#include "reduction/region_refit/RegionCurveNetwork.h"

#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopoDS_Vertex.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace spo {

namespace {

using MeshEdge = std::array<std::size_t, 2>;

MeshEdge orderedEdge(const std::size_t first, const std::size_t second) {
    return {std::min(first, second), std::max(first, second)};
}

bool finitePoint(const gp_Pnt& point) {
    return std::isfinite(point.X()) && std::isfinite(point.Y()) &&
        std::isfinite(point.Z());
}

std::vector<std::size_t> orderedChain(
    const std::vector<MeshEdge>& edges,
    bool& valid) {
    valid = false;
    std::map<std::size_t, std::vector<std::size_t>> adjacency;
    std::set<MeshEdge> remaining;
    for (const auto& edge : edges) {
        if (edge[0] == edge[1]) return {};
        adjacency[edge[0]].push_back(edge[1]);
        adjacency[edge[1]].push_back(edge[0]);
        remaining.insert(orderedEdge(edge[0], edge[1]));
    }
    std::vector<std::size_t> endpoints;
    for (auto& [vertex, neighbors] : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
        if (neighbors.size() > 2 || neighbors.empty()) return {};
        if (neighbors.size() == 1) endpoints.push_back(vertex);
    }
    if (endpoints.size() != 2) return {};
    std::sort(endpoints.begin(), endpoints.end());
    std::vector<std::size_t> result {endpoints.front()};
    std::optional<std::size_t> previous;
    auto current = endpoints.front();
    while (current != endpoints.back()) {
        std::optional<std::size_t> next;
        for (const auto neighbor : adjacency[current]) {
            const auto edge = orderedEdge(current, neighbor);
            if (remaining.contains(edge) &&
                (!previous.has_value() || neighbor != *previous)) {
                next = neighbor;
                break;
            }
        }
        if (!next.has_value()) return {};
        remaining.erase(orderedEdge(current, *next));
        previous = current;
        current = *next;
        result.push_back(current);
    }
    valid = remaining.empty();
    return valid ? result : std::vector<std::size_t> {};
}

const RegionBoundaryEdgeConstraint* constraintFor(
    const RegionTopologyConstraints& constraints,
    const EdgeId edgeId) {
    const auto found = std::find_if(
        constraints.boundaryEdges.begin(),
        constraints.boundaryEdges.end(),
        [&](const auto& constraint) { return constraint.edgeId == edgeId; });
    return found == constraints.boundaryEdges.end() ? nullptr : &*found;
}

std::optional<EdgeId> commonBoundaryEdgeId(
    const RegionReferenceVertex& first,
    const RegionReferenceVertex& second) {
    std::optional<EdgeId> result;
    for (const auto edgeId : first.boundaryEdgeIds) {
        if (std::find(
                second.boundaryEdgeIds.begin(),
                second.boundaryEdgeIds.end(),
                edgeId) != second.boundaryEdgeIds.end() &&
            (!result.has_value() || edgeId < *result)) {
            result = edgeId;
        }
    }
    return result;
}

bool projectToOriginalRegion(
    const gp_Pnt& point,
    const RegionTopologyConstraints& constraints,
    gp_Pnt& projected,
    double& distance,
    const std::optional<FaceId> preferredFaceId = std::nullopt) {
    distance = std::numeric_limits<double>::infinity();
    const auto evaluate = [&](const TopoDS_Face& face) {
        if (face.IsNull()) return;
        try {
            BRepExtrema_DistShapeShape extrema(
                BRepBuilderAPI_MakeVertex(point).Vertex(), face);
            extrema.Perform();
            if (!extrema.IsDone() || extrema.NbSolution() <= 0) return;
            const auto candidateDistance = extrema.Value();
            if (candidateDistance < distance) {
                distance = candidateDistance;
                projected = extrema.PointOnShape2(1);
            }
        } catch (const Standard_Failure&) {
        }
    };
    if (preferredFaceId.has_value()) {
        const auto preferred = std::find_if(
            constraints.selectedFaceGeometry.begin(),
            constraints.selectedFaceGeometry.end(),
            [&](const auto& item) {
                return item.first == *preferredFaceId;
            });
        if (preferred != constraints.selectedFaceGeometry.end()) {
            evaluate(preferred->second);
        }
    }
    for (const auto& [faceId, face] : constraints.selectedFaceGeometry) {
        if (preferredFaceId.has_value() && faceId == *preferredFaceId) continue;
        evaluate(face);
    }
    return finitePoint(projected) && std::isfinite(distance);
}

std::optional<FaceId> preferredFace(
    const RegionReferenceVertex& vertex) {
    return vertex.sources.empty()
        ? std::nullopt
        : std::optional<FaceId>(vertex.sources.front().faceId);
}

RegionCurveContinuity internalContinuity(
    const RegionReferenceMesh& mesh,
    const std::vector<std::size_t>& vertices,
    const RegionRefitOptions& options) {
    for (const auto vertexIndex : vertices) {
        const auto& vertex = mesh.vertices[vertexIndex];
        if (vertex.normalDiscontinuity) {
            return RegionCurveContinuity::G0;
        }
        for (std::size_t first = 0; first < vertex.sources.size(); ++first) {
            if (!vertex.sources[first].normalDefined) continue;
            for (std::size_t second = first + 1;
                 second < vertex.sources.size(); ++second) {
                if (!vertex.sources[second].normalDefined) continue;
                const auto dot = std::clamp(
                    vertex.sources[first].normal.Dot(
                        vertex.sources[second].normal),
                    -1.0,
                    1.0);
                const auto degrees =
                    std::acos(dot) * 180.0 / 3.14159265358979323846;
                if (degrees > options.g1ToleranceDegrees) {
                    return RegionCurveContinuity::G0;
                }
            }
        }
    }
    if (!options.enableInternalSeamG2) {
        return RegionCurveContinuity::G1;
    }
    for (const auto vertexIndex : vertices) {
        const auto& sources = mesh.vertices[vertexIndex].sources;
        if (sources.empty()) {
            return RegionCurveContinuity::G1;
        }
        for (const auto& source : sources) {
            if (!source.curvatureDefined ||
                !std::isfinite(source.minimumCurvature) ||
                !std::isfinite(source.maximumCurvature)) {
                return RegionCurveContinuity::G1;
            }
        }
        for (std::size_t first = 0; first < sources.size(); ++first) {
            for (std::size_t second = first + 1;
                 second < sources.size(); ++second) {
                if (std::abs(
                        sources[first].minimumCurvature -
                        sources[second].minimumCurvature) >
                        options.g2CurvatureTolerance ||
                    std::abs(
                        sources[first].maximumCurvature -
                        sources[second].maximumCurvature) >
                        options.g2CurvatureTolerance) {
                    return RegionCurveContinuity::G1;
                }
            }
        }
    }
    return RegionCurveContinuity::G2;
}

bool curveParameter(
    const Handle(Geom_Curve)& curve,
    const gp_Pnt& point,
    const double first,
    const double last,
    double& parameter) {
    if (curve.IsNull()) return false;
    try {
        GeomAPI_ProjectPointOnCurve projection(
            point, curve, std::min(first, last), std::max(first, last));
        if (projection.NbPoints() <= 0) return false;
        parameter = projection.LowerDistanceParameter();
        return std::isfinite(parameter);
    } catch (const Standard_Failure&) {
        return false;
    }
}

std::size_t nodeFor(
    RegionCurveNetwork& network,
    std::map<std::size_t, std::size_t>& nodeByMeshVertex,
    const std::size_t meshVertex,
    const gp_Pnt& point,
    const bool originalBoundary) {
    const auto found = nodeByMeshVertex.find(meshVertex);
    if (found != nodeByMeshVertex.end()) {
        auto& node = network.nodes[found->second];
        if (originalBoundary && !node.originalBoundary) {
            node.point = point;
            node.originalBoundary = true;
        }
        return found->second;
    }
    const auto id = network.nodes.size();
    network.nodes.push_back({id, meshVertex, point, originalBoundary, {}});
    nodeByMeshVertex.emplace(meshVertex, id);
    return id;
}

std::vector<double> chordParameters(
    const std::vector<gp_Pnt>& points,
    const double first,
    const double last) {
    std::vector<double> cumulative(points.size(), 0.0);
    for (std::size_t index = 1; index < points.size(); ++index) {
        cumulative[index] = cumulative[index - 1] +
            std::max(points[index - 1].Distance(points[index]), 1.0e-12);
    }
    const auto length = cumulative.back();
    if (length <= 1.0e-14) return {};
    for (auto& parameter : cumulative) {
        parameter = first + (last - first) * parameter / length;
    }
    return cumulative;
}

bool makeInternalCurve(
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const RegionRefitOptions& options,
    const std::vector<std::size_t>& vertices,
    const gp_Pnt& startPoint,
    const gp_Pnt& endPoint,
    Handle(Geom_Curve)& curve,
    std::vector<double>& meshParameters,
    double& maximumProjectionError,
    std::string& message) {
    if (vertices.size() < 2) {
        message = "An internal curve needs at least two carrier vertices.";
        return false;
    }
    std::vector<gp_Pnt> points;
    points.reserve(vertices.size());
    for (const auto vertex : vertices) {
        gp_Pnt projected;
        double distance = 0.0;
        if (!projectToOriginalRegion(
                mesh.vertices[vertex].point,
                constraints,
                projected,
                distance,
                preferredFace(mesh.vertices[vertex]))) {
            message = "An internal curve sample could not be projected to the original STEP region.";
            return false;
        }
        points.push_back(projected);
    }
    points.front() = startPoint;
    points.back() = endPoint;
    for (int iteration = 0; iteration < options.curveFairingIterations; ++iteration) {
        auto faired = points;
        for (std::size_t index = 1; index + 1 < points.size(); ++index) {
            const gp_Pnt average(
                (points[index - 1].X() + points[index + 1].X()) * 0.5,
                (points[index - 1].Y() + points[index + 1].Y()) * 0.5,
                (points[index - 1].Z() + points[index + 1].Z()) * 0.5);
            const gp_Pnt target(
                points[index].X() * (1.0 - options.curveFairingWeight) +
                    average.X() * options.curveFairingWeight,
                points[index].Y() * (1.0 - options.curveFairingWeight) +
                    average.Y() * options.curveFairingWeight,
                points[index].Z() * (1.0 - options.curveFairingWeight) +
                    average.Z() * options.curveFairingWeight);
            gp_Pnt projected;
            double distance = 0.0;
            if (!projectToOriginalRegion(
                    target,
                    constraints,
                    projected,
                    distance,
                    preferredFace(mesh.vertices[vertices[index]]))) {
                message = "A fairing iterate could not be projected to the original STEP region.";
                return false;
            }
            faired[index] = projected;
        }
        points = std::move(faired);
    }
    std::vector<gp_Pnt> uniquePoints;
    uniquePoints.reserve(points.size());
    const auto duplicateTolerance = std::max(
        1.0e-10, options.curveProjectionTolerance * 1.0e-6);
    for (const auto& point : points) {
        if (uniquePoints.empty() ||
            uniquePoints.back().Distance(point) > duplicateTolerance) {
            uniquePoints.push_back(point);
        }
    }
    if (uniquePoints.size() < 2 ||
        uniquePoints.front().Distance(uniquePoints.back()) <=
            duplicateTolerance) {
        message =
            "The projected internal interface collapsed to a degenerate curve.";
        return false;
    }
    constexpr std::size_t maximumInterpolationPointCount = 129;
    if (uniquePoints.size() > maximumInterpolationPointCount) {
        std::vector<gp_Pnt> reduced;
        reduced.reserve(maximumInterpolationPointCount);
        for (std::size_t index = 0;
             index < maximumInterpolationPointCount; ++index) {
            const auto source = index * (uniquePoints.size() - 1) /
                (maximumInterpolationPointCount - 1);
            if (reduced.empty() ||
                reduced.back().Distance(uniquePoints[source]) >
                    duplicateTolerance) {
                reduced.push_back(uniquePoints[source]);
            }
        }
        if (reduced.back().Distance(uniquePoints.back()) >
            duplicateTolerance) {
            reduced.push_back(uniquePoints.back());
        }
        uniquePoints = std::move(reduced);
    }
    TColgp_Array1OfPnt approximationPoints(
        1, static_cast<int>(uniquePoints.size()));
    for (std::size_t index = 0; index < uniquePoints.size(); ++index) {
        approximationPoints.SetValue(
            static_cast<int>(index + 1), uniquePoints[index]);
    }
    try {
        const auto degree = std::max(
            1, std::min(3, static_cast<int>(uniquePoints.size()) - 1));
        GeomAPI_PointsToBSpline approximation(
            approximationPoints,
            Approx_ChordLength,
            degree,
            degree,
            degree >= 3 ? GeomAbs_C2 : GeomAbs_C0,
            options.curveProjectionTolerance * 0.02);
        if (approximation.IsDone() && !approximation.Curve().IsNull()) {
            curve = approximation.Curve();
        }
    } catch (const Standard_Failure&) {
    }
    if (curve.IsNull()) {
        Handle(TColgp_HArray1OfPnt) interpolationPoints =
            new TColgp_HArray1OfPnt(
                1, static_cast<int>(uniquePoints.size()));
        for (std::size_t index = 0;
             index < uniquePoints.size(); ++index) {
            interpolationPoints->SetValue(
                static_cast<int>(index + 1), uniquePoints[index]);
        }
        try {
            GeomAPI_Interpolate interpolation(
                interpolationPoints,
                Standard_False,
                options.curveProjectionTolerance * 0.02);
            interpolation.Perform();
            if (interpolation.IsDone() &&
                !interpolation.Curve().IsNull()) {
                curve = interpolation.Curve();
            }
        } catch (const Standard_Failure&) {
        }
    }
    if (curve.IsNull()) {
        message =
            "Open CASCADE rejected both interpolation and approximation of the frozen internal curve.";
        return false;
    }
    meshParameters = chordParameters(
        points, curve->FirstParameter(), curve->LastParameter());
    if (meshParameters.size() != vertices.size()) {
        message = "The frozen internal curve has a degenerate parameter range.";
        return false;
    }
    maximumProjectionError = 0.0;
    const auto sampleCount = std::max(2, options.curveValidationSampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
        const auto alpha = static_cast<double>(sample) /
            static_cast<double>(sampleCount - 1);
        const auto parameter = curve->FirstParameter() +
            alpha * (curve->LastParameter() - curve->FirstParameter());
        const auto point = curve->Value(parameter);
        gp_Pnt projected;
        double distance = 0.0;
        const auto lower = std::lower_bound(
            meshParameters.begin(), meshParameters.end(), parameter);
        auto carrierSample = lower == meshParameters.end()
            ? meshParameters.size() - 1
            : static_cast<std::size_t>(
                std::distance(meshParameters.begin(), lower));
        if (carrierSample > 0 &&
            std::abs(meshParameters[carrierSample - 1] - parameter) <
                std::abs(meshParameters[carrierSample] - parameter)) {
            --carrierSample;
        }
        if (!projectToOriginalRegion(
                point,
                constraints,
                projected,
                distance,
                preferredFace(mesh.vertices[vertices[carrierSample]]))) {
            message = "A frozen internal curve validation sample could not be projected.";
            return false;
        }
        if (distance > options.curveProjectionTolerance) {
            gp_Pnt nearest;
            double nearestDistance = 0.0;
            if (!projectToOriginalRegion(
                    point, constraints, nearest, nearestDistance)) {
                message =
                    "A frozen internal curve validation sample could not be projected.";
                return false;
            }
            distance = nearestDistance;
        }
        maximumProjectionError = std::max(maximumProjectionError, distance);
    }
    return true;
}

RegionCurveContinuity boundaryContinuity(
    const RegionBoundaryEdgeConstraint& constraint) {
    return constraint.continuity == RegionBoundaryContinuity::Smooth
        ? RegionCurveContinuity::G1
        : RegionCurveContinuity::G0;
}

std::string stableHash(const RegionCurveNetwork& network) {
    std::uint64_t hash = 1469598103934665603ull;
    auto append = [&](const std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    append(static_cast<std::uint64_t>(network.patchCount));
    for (const auto patch : network.trianglePatches) {
        append(static_cast<std::uint64_t>(patch));
    }
    for (const auto& node : network.nodes) {
        append(node.id);
        append(node.meshVertex);
        append(static_cast<std::uint64_t>(std::llround(node.point.X() * 1.0e12)));
        append(static_cast<std::uint64_t>(std::llround(node.point.Y() * 1.0e12)));
        append(static_cast<std::uint64_t>(std::llround(node.point.Z() * 1.0e12)));
    }
    for (const auto& edge : network.edges) {
        append(edge.id);
        append(edge.startNode);
        append(edge.endNode);
        append(static_cast<std::uint64_t>(edge.firstPatch + 1));
        append(static_cast<std::uint64_t>(edge.secondPatch + 1));
        append(static_cast<std::uint64_t>(edge.continuity));
        append(edge.originalBoundary);
        append(edge.hasOriginalEdge ? edge.originalEdgeId : 0);
        for (const auto vertex : edge.meshVertices) append(vertex);
        constexpr int hashSamples = 17;
        for (int sample = 0; sample < hashSamples; ++sample) {
            const auto alpha = static_cast<double>(sample) /
                static_cast<double>(hashSamples - 1);
            const auto parameter = edge.firstParameter +
                alpha * (edge.lastParameter - edge.firstParameter);
            const auto point = edge.curve->Value(parameter);
            append(static_cast<std::uint64_t>(std::llround(point.X() * 1.0e12)));
            append(static_cast<std::uint64_t>(std::llround(point.Y() * 1.0e12)));
            append(static_cast<std::uint64_t>(std::llround(point.Z() * 1.0e12)));
        }
    }
    for (const auto& loop : network.patchLoops) {
        append(static_cast<std::uint64_t>(loop.patch + 1));
        for (const auto& use : loop.edges) {
            append(use.edgeIndex);
            append(use.forward);
        }
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

bool buildNetwork(
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const RegionRefitOptions& options,
    const RegionPatchLayout& layout,
    RegionCurveNetwork& network,
    std::string& message) {
    network.patchCount = layout.patchCount;
    network.trianglePatches = layout.trianglePatches;
    network.partitionLayoutHash = layout.hash;
    std::map<std::size_t, std::size_t> nodeByMeshVertex;
    std::map<MeshEdge, std::pair<std::size_t, bool>> edgeUseByMeshEdge;

    std::map<std::pair<EdgeId, int>, std::vector<MeshEdge>> outerByOwner;
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        const auto& loop = layout.patches[patch].boundaryLoop;
        for (std::size_t position = 0; position < loop.size(); ++position) {
            const auto first = loop[position];
            const auto second = loop[(position + 1) % loop.size()];
            const auto edgeId = commonBoundaryEdgeId(
                mesh.vertices[first], mesh.vertices[second]);
            if (edgeId.has_value()) {
                outerByOwner[{*edgeId, static_cast<int>(patch)}].push_back(
                    orderedEdge(first, second));
            }
        }
    }
    for (const auto& [owner, meshEdges] : outerByOwner) {
        bool valid = false;
        auto vertices = orderedChain(meshEdges, valid);
        const auto* constraint = constraintFor(constraints, owner.first);
        if (!valid || constraint == nullptr || constraint->originalEdge.IsNull()) {
            message = "An original boundary ownership chain is invalid.";
            return false;
        }
        double originalFirst = 0.0;
        double originalLast = 0.0;
        const auto curve = BRep_Tool::Curve(
            constraint->originalEdge, originalFirst, originalLast);
        double firstParameter = 0.0;
        double lastParameter = 0.0;
        if (curve.IsNull() ||
            !curveParameter(
                curve, mesh.vertices[vertices.front()].point,
                originalFirst, originalLast, firstParameter) ||
            !curveParameter(
                curve, mesh.vertices[vertices.back()].point,
                originalFirst, originalLast, lastParameter)) {
            message = "An original boundary chain has no recoverable curve range.";
            return false;
        }
        if (lastParameter < firstParameter) {
            std::reverse(vertices.begin(), vertices.end());
            std::swap(firstParameter, lastParameter);
        }
        std::vector<gp_Pnt> points;
        std::vector<double> parameters;
        points.reserve(vertices.size());
        parameters.reserve(vertices.size());
        for (const auto vertex : vertices) {
            double parameter = 0.0;
            if (!curveParameter(
                    curve, mesh.vertices[vertex].point,
                    originalFirst, originalLast, parameter)) {
                message = "An original boundary sample could not be parameterized.";
                return false;
            }
            points.push_back(curve->Value(parameter));
            parameters.push_back(parameter);
        }
        if (parameters.size() != vertices.size()) {
            message = "An original boundary segment is degenerate.";
            return false;
        }
        const auto start = nodeFor(
            network, nodeByMeshVertex, vertices.front(),
            curve->Value(firstParameter), true);
        const auto end = nodeFor(
            network, nodeByMeshVertex, vertices.back(),
            curve->Value(lastParameter), true);
        const auto parameterTolerance = std::max(
            1.0e-10, std::abs(originalLast - originalFirst) * 1.0e-9);
        const auto fullRange =
            std::abs(firstParameter - std::min(originalFirst, originalLast)) <=
                parameterTolerance &&
            std::abs(lastParameter - std::max(originalFirst, originalLast)) <=
                parameterTolerance;
        RegionCurveNetworkEdge edge;
        edge.id = network.edges.size();
        edge.startNode = start;
        edge.endNode = end;
        edge.firstPatch = owner.second;
        edge.continuity = boundaryContinuity(*constraint);
        edge.originalBoundary = true;
        edge.hasOriginalEdge = true;
        edge.originalEdgeId = owner.first;
        edge.originalEdge = constraint->originalEdge;
        edge.curve = curve;
        edge.firstParameter = firstParameter;
        edge.lastParameter = lastParameter;
        edge.meshVertices = vertices;
        edge.meshParameters = std::move(parameters);
        edge.reusesOriginalEdge = fullRange;
        edge.splitsOriginalEdge = !fullRange;
        edge.frozen = true;
        network.nodes[start].incidentEdges.push_back(edge.id);
        network.nodes[end].incidentEdges.push_back(edge.id);
        network.edges.push_back(std::move(edge));
        const auto edgeIndex = network.edges.size() - 1;
        for (std::size_t index = 0; index + 1 < vertices.size(); ++index) {
            edgeUseByMeshEdge[orderedEdge(vertices[index], vertices[index + 1])] = {
                edgeIndex, vertices[index] == network.edges[edgeIndex].meshVertices[index]};
        }
    }

    for (const auto& seam : layout.seams) {
        bool valid = false;
        auto vertices = orderedChain(seam.meshEdges, valid);
        if (!valid) {
            message = "An internal partition interface is open, branched, or cyclic.";
            return false;
        }
        gp_Pnt firstProjected;
        gp_Pnt lastProjected;
        double firstDistance = 0.0;
        double lastDistance = 0.0;
        if (!projectToOriginalRegion(
                mesh.vertices[vertices.front()].point,
                constraints,
                firstProjected,
                firstDistance,
                preferredFace(mesh.vertices[vertices.front()])) ||
            !projectToOriginalRegion(
                mesh.vertices[vertices.back()].point,
                constraints,
                lastProjected,
                lastDistance,
                preferredFace(mesh.vertices[vertices.back()]))) {
            message = "An internal curve endpoint could not be projected.";
            return false;
        }
        const auto start = nodeFor(
            network, nodeByMeshVertex, vertices.front(),
            firstProjected, mesh.vertices[vertices.front()].boundary);
        const auto end = nodeFor(
            network, nodeByMeshVertex, vertices.back(),
            lastProjected, mesh.vertices[vertices.back()].boundary);
        Handle(Geom_Curve) curve;
        std::vector<double> parameters;
        double maximumProjectionError = 0.0;
        if (!makeInternalCurve(
                mesh, constraints, options, vertices,
                network.nodes[start].point, network.nodes[end].point,
                curve, parameters, maximumProjectionError, message)) {
            return false;
        }
        RegionCurveNetworkEdge edge;
        edge.id = network.edges.size();
        edge.startNode = start;
        edge.endNode = end;
        edge.firstPatch = seam.firstPatch;
        edge.secondPatch = seam.secondPatch;
        edge.continuity = internalContinuity(mesh, vertices, options);
        edge.curve = curve;
        edge.firstParameter = curve->FirstParameter();
        edge.lastParameter = curve->LastParameter();
        edge.meshVertices = vertices;
        edge.meshParameters = std::move(parameters);
        edge.frozen = true;
        edge.maximumProjectionError = maximumProjectionError;
        network.nodes[start].incidentEdges.push_back(edge.id);
        network.nodes[end].incidentEdges.push_back(edge.id);
        network.edges.push_back(std::move(edge));
        const auto edgeIndex = network.edges.size() - 1;
        for (std::size_t index = 0; index + 1 < vertices.size(); ++index) {
            edgeUseByMeshEdge[orderedEdge(vertices[index], vertices[index + 1])] = {
                edgeIndex, true};
        }
    }

    network.patchLoops.reserve(layout.patches.size());
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        RegionCurveNetworkPatchLoop curveLoop;
        curveLoop.patch = static_cast<int>(patch);
        const auto& meshLoop = layout.patches[patch].boundaryLoop;
        for (std::size_t position = 0; position < meshLoop.size(); ++position) {
            const auto first = meshLoop[position];
            const auto second = meshLoop[(position + 1) % meshLoop.size()];
            const auto found = edgeUseByMeshEdge.find(orderedEdge(first, second));
            if (found == edgeUseByMeshEdge.end()) {
                message = "A patch loop segment has no frozen curve-network edge.";
                return false;
            }
            const auto& edge = network.edges[found->second.first];
            const auto vertexFound = std::find(
                edge.meshVertices.begin(), edge.meshVertices.end(), first);
            if (vertexFound == edge.meshVertices.end()) {
                message = "A patch loop segment is not owned by its curve-network edge.";
                return false;
            }
            const auto vertexIndex = static_cast<std::size_t>(
                std::distance(edge.meshVertices.begin(), vertexFound));
            const auto forward = vertexIndex + 1 < edge.meshVertices.size() &&
                edge.meshVertices[vertexIndex + 1] == second;
            const auto backward = vertexIndex > 0 &&
                edge.meshVertices[vertexIndex - 1] == second;
            if (!forward && !backward) {
                message = "A patch loop curve direction is inconsistent.";
                return false;
            }
            if (curveLoop.edges.empty() ||
                curveLoop.edges.back().edgeIndex != found->second.first) {
                curveLoop.edges.push_back({found->second.first, forward});
            } else if (curveLoop.edges.back().forward != forward) {
                message = "A patch loop reverses direction inside one frozen curve.";
                return false;
            }
        }
        if (curveLoop.edges.size() > 1 &&
            curveLoop.edges.front().edgeIndex == curveLoop.edges.back().edgeIndex) {
            if (curveLoop.edges.front().forward != curveLoop.edges.back().forward) {
                message = "A patch loop has inconsistent wrap-around curve direction.";
                return false;
            }
            curveLoop.edges.pop_back();
        }
        network.patchLoops.push_back(std::move(curveLoop));
    }
    return validateRegionCurveNetwork(
        network,
        options.curveProjectionTolerance,
        options.curveValidationSampleCount,
        message);
}

}

const char* toString(const RegionCurveContinuity continuity) {
    switch (continuity) {
    case RegionCurveContinuity::G0: return "G0";
    case RegionCurveContinuity::G1: return "G1";
    case RegionCurveContinuity::G2: return "G2";
    }
    return "Unknown";
}

bool RegionCurveNetwork::valid() const {
    return report.finitePassed && report.closurePassed &&
        report.ownershipPassed && report.projectionPassed &&
        report.nonManifoldEdgePassed && report.frozen && !hash.empty();
}

bool RegionCurveNetworkGenerationResult::success() const {
    return status == RegionRefitStatus::Success && !selectedCandidates.empty();
}

bool validateRegionCurveNetwork(
    RegionCurveNetwork& network,
    const double projectionTolerance,
    const int validationSampleCount,
    std::string& message) {
    message.clear();
    network.report = {};
    network.report.nodeCount = network.nodes.size();
    network.report.edgeCount = network.edges.size();
    if (projectionTolerance <= 0.0 || validationSampleCount < 2 ||
        network.patchCount <= 0 ||
        network.patchLoops.size() != static_cast<std::size_t>(network.patchCount)) {
        message = "Curve-network validation received inconsistent input.";
        network.report.rejectionReason = message;
        return false;
    }
    network.report.finitePassed = std::all_of(
        network.nodes.begin(), network.nodes.end(),
        [](const auto& node) { return finitePoint(node.point); });
    std::vector<int> edgeUseCount(network.edges.size(), 0);
    network.report.closurePassed = true;
    for (const auto& loop : network.patchLoops) {
        if (loop.patch < 0 || loop.patch >= network.patchCount ||
            loop.edges.size() < 2) {
            network.report.closurePassed = false;
            break;
        }
        for (std::size_t index = 0; index < loop.edges.size(); ++index) {
            const auto& use = loop.edges[index];
            const auto& nextUse = loop.edges[(index + 1) % loop.edges.size()];
            if (use.edgeIndex >= network.edges.size() ||
                nextUse.edgeIndex >= network.edges.size()) {
                network.report.closurePassed = false;
                break;
            }
            const auto& edge = network.edges[use.edgeIndex];
            const auto& next = network.edges[nextUse.edgeIndex];
            const auto endNode = use.forward ? edge.endNode : edge.startNode;
            const auto nextStart = nextUse.forward ? next.startNode : next.endNode;
            if (endNode != nextStart) {
                network.report.closurePassed = false;
                break;
            }
            ++edgeUseCount[use.edgeIndex];
        }
    }
    network.report.ownershipPassed = network.report.closurePassed;
    network.report.nonManifoldEdgePassed = true;
    network.report.projectionPassed = true;
    network.report.frozen = true;
    for (std::size_t index = 0; index < network.edges.size(); ++index) {
        auto& edge = network.edges[index];
        if (edge.id != index || edge.curve.IsNull() ||
            edge.startNode >= network.nodes.size() ||
            edge.endNode >= network.nodes.size() ||
            !std::isfinite(edge.firstParameter) ||
            !std::isfinite(edge.lastParameter) ||
            edge.lastParameter <= edge.firstParameter ||
            edge.meshVertices.size() < 2 ||
            edge.meshParameters.size() != edge.meshVertices.size()) {
            network.report.finitePassed = false;
        } else {
            const auto parametersIncreasing = std::adjacent_find(
                edge.meshParameters.begin(),
                edge.meshParameters.end(),
                [](const auto first, const auto second) {
                    return !std::isfinite(first) || !std::isfinite(second) ||
                        second <= first;
                }) == edge.meshParameters.end();
            const auto endpointsMatch =
                edge.curve->Value(edge.firstParameter).Distance(
                    network.nodes[edge.startNode].point) <=
                    projectionTolerance &&
                edge.curve->Value(edge.lastParameter).Distance(
                    network.nodes[edge.endNode].point) <=
                    projectionTolerance;
            network.report.finitePassed = network.report.finitePassed &&
                parametersIncreasing && endpointsMatch;
        }
        const auto expectedUseCount = edge.originalBoundary ? 1 : 2;
        if (edgeUseCount[index] != expectedUseCount) {
            network.report.ownershipPassed = false;
        }
        if (!edge.originalBoundary &&
            (edge.firstPatch < 0 || edge.secondPatch < 0 ||
                edge.firstPatch == edge.secondPatch)) {
            network.report.nonManifoldEdgePassed = false;
        }
        if (edge.originalBoundary) ++network.report.boundaryEdgeCount;
        else ++network.report.internalEdgeCount;
        switch (edge.continuity) {
        case RegionCurveContinuity::G0: ++network.report.g0EdgeCount; break;
        case RegionCurveContinuity::G1: ++network.report.g1EdgeCount; break;
        case RegionCurveContinuity::G2: ++network.report.g2EdgeCount; break;
        }
        network.report.maximumProjectionError = std::max(
            network.report.maximumProjectionError,
            edge.maximumProjectionError);
        network.report.projectionPassed =
            network.report.projectionPassed &&
            std::isfinite(edge.maximumProjectionError) &&
            edge.maximumProjectionError <= projectionTolerance;
        network.report.frozen = network.report.frozen && edge.frozen;
    }
    if (!network.report.finitePassed) message = "Curve network contains non-finite or incomplete geometry.";
    else if (!network.report.closurePassed) message = "Curve network contains an open or misoriented patch loop.";
    else if (!network.report.ownershipPassed) message = "Curve network edge ownership counts are inconsistent.";
    else if (!network.report.nonManifoldEdgePassed) message = "Curve network contains an invalid shared-edge adjacency.";
    else if (!network.report.projectionPassed) {
        std::ostringstream stream;
        stream << "Curve network exceeds the original STEP projection tolerance: maximum "
               << network.report.maximumProjectionError
               << ", tolerance " << projectionTolerance << '.';
        message = stream.str();
    }
    else if (!network.report.frozen) message = "Curve network contains an unfrozen edge.";
    if (!message.empty()) {
        network.report.rejectionReason = message;
        return false;
    }
    network.hash = stableHash(network);
    return true;
}

RegionCurveNetworkGenerationResult RegionCurveNetworkGenerator::generate(
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const RegionRefitLayoutMode mode,
    const RegionRefitOptions& options,
    const bool allowDiagnosticCandidates) const {
    RegionCurveNetworkGenerationResult result;
    const auto partitions = RegionPatchLayoutGenerator().generate(
        mesh, mode, options, allowDiagnosticCandidates);
    result.layoutReports = partitions.candidates;
    if (!partitions.success()) {
        result.status = partitions.status;
        result.message = partitions.message;
        return result;
    }
    std::set<std::string> selectedHashes;
    for (const auto& layout : partitions.selectedCandidates) {
        selectedHashes.insert(layout.hash);
    }
    for (const auto& layout : partitions.constructibleCandidates) {
        RegionCurveNetworkCandidate candidate;
        candidate.partitionLayout = layout;
        std::string failure;
        if (!buildNetwork(
                mesh, constraints, options, layout,
                candidate.network, failure)) {
            const auto report = std::find_if(
                result.layoutReports.begin(), result.layoutReports.end(),
                [&](const auto& item) { return item.layoutHash == layout.hash; });
            if (report != result.layoutReports.end()) {
                report->rejectionReason = failure;
            }
            continue;
        }
        if (selectedHashes.contains(layout.hash)) {
            result.selectedCandidates.push_back(candidate);
        }
        result.constructibleCandidates.push_back(std::move(candidate));
    }
    if (result.selectedCandidates.empty()) {
        result.status = RegionRefitStatus::NoValidatedLayoutWithinBudget;
        result.message = "No partition candidate produced a frozen, closed curve network.";
        return result;
    }
    result.status = RegionRefitStatus::Success;
    result.message = "Generated and froze deterministic STEP-constrained curve networks.";
    return result;
}

}
