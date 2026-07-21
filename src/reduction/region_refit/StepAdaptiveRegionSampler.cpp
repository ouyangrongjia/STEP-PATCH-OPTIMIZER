#include "reduction/region_refit/StepAdaptiveRegionSampler.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spo {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinimumTriangleShapeQuality = 1.0e-8;
constexpr int kMaximumNormalRefinementIterations = 14;

struct LocalVertex {
    RegionReferenceVertex value;
    std::vector<EdgeId> boundaryEdges;
    std::vector<EdgeId> internalSeamEdges;
};

struct LocalTriangle {
    std::array<std::size_t, 3> vertices {};
};

struct LocalBoundarySample {
    EdgeId edgeId = 0;
    double parameter = 0.0;
    std::size_t vertexIndex = 0;
    bool endpoint = false;
};

struct LocalFaceMesh {
    FaceId faceId = 0;
    TopoDS_Face face;
    Handle(Poly_Triangulation) triangulation;
    TopLoc_Location location;
    std::vector<LocalVertex> vertices;
    std::vector<LocalTriangle> triangles;
    std::vector<LocalBoundarySample> boundarySamples;
};

struct CurveSample {
    double parameter = 0.0;
    gp_Pnt point;
    gp_Dir normal {0.0, 0.0, 1.0};
    bool normalDefined = false;
};

struct WeldKey {
    EdgeId edgeId = 0;
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    bool operator==(const WeldKey& other) const {
        return edgeId == other.edgeId && x == other.x && y == other.y && z == other.z;
    }
};

struct WeldKeyHash {
    std::size_t operator()(const WeldKey& key) const {
        std::size_t seed = std::hash<EdgeId> {}(key.edgeId);
        const auto mix = [&seed](const std::int64_t value) {
            seed ^= std::hash<std::int64_t> {}(value) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
        };
        mix(key.x);
        mix(key.y);
        mix(key.z);
        return seed;
    }
};

StepAdaptiveRegionSamplingResult fail(
    const RegionRefitStatus status,
    std::string message) {
    StepAdaptiveRegionSamplingResult result;
    result.report.status = status;
    result.report.message = std::move(message);
    return result;
}

double clampedAngleDegrees(const gp_Dir& first, const gp_Dir& second) {
    const auto dot = std::clamp(first.Dot(second), -1.0, 1.0);
    return std::acos(dot) * 180.0 / kPi;
}

double pointSegmentDistance(
    const gp_Pnt& point,
    const gp_Pnt& first,
    const gp_Pnt& second) {
    const gp_Vec segment(first, second);
    const auto lengthSquared = segment.SquareMagnitude();
    if (lengthSquared <= 1.0e-24) {
        return point.Distance(first);
    }
    const gp_Vec offset(first, point);
    const auto parameter = std::clamp(offset.Dot(segment) / lengthSquared, 0.0, 1.0);
    return point.Distance(first.Translated(segment * parameter));
}

RegionReferenceSourceSample evaluateSource(
    const TopoDS_Face& face,
    const FaceId faceId,
    const gp_Pnt2d& uv) {
    RegionReferenceSourceSample source;
    source.faceId = faceId;
    source.uv = uv;
    BRepAdaptor_Surface surface(face, true);
    BRepLProp_SLProps properties(surface, uv.X(), uv.Y(), 2, Precision::Confusion());
    if (properties.IsNormalDefined()) {
        source.normalDefined = true;
        source.normal = properties.Normal();
        if (face.Orientation() == TopAbs_REVERSED) {
            source.normal.Reverse();
        }
    }
    if (properties.IsCurvatureDefined()) {
        source.curvatureDefined = true;
        source.minimumCurvature = properties.MinCurvature();
        source.maximumCurvature = properties.MaxCurvature();
    }
    return source;
}

RegionReferenceVertex makeVertex(
    const TopoDS_Face& face,
    const FaceId faceId,
    const gp_Pnt& point,
    const gp_Pnt2d& uv) {
    RegionReferenceVertex vertex;
    vertex.point = point;
    vertex.sources.push_back(evaluateSource(face, faceId, uv));
    vertex.normal = vertex.sources.front().normal;
    return vertex;
}

std::optional<CurveSample> evaluateBoundary(
    const ShapeDocument& document,
    const RegionBoundaryEdgeConstraint& constraint,
    const double parameter) {
    if (constraint.selectedPcurve.IsNull()) {
        return std::nullopt;
    }
    BRepAdaptor_Curve curve(constraint.originalEdge);
    CurveSample sample;
    sample.parameter = parameter;
    sample.point = curve.Value(parameter);
    const auto uv = constraint.selectedPcurve->Value(parameter);
    const auto source = evaluateSource(
        document.topology().face(constraint.selectedFaceId),
        constraint.selectedFaceId,
        uv);
    sample.normal = source.normal;
    sample.normalDefined = true;
    return sample;
}

bool appendAdaptiveBoundaryParameters(
    const ShapeDocument& document,
    const RegionBoundaryEdgeConstraint& constraint,
    const CurveSample& first,
    const CurveSample& last,
    const double chordTolerance,
    const double normalToleranceDegrees,
    const int depth,
    std::vector<double>& parameters,
    double& maximumChordError) {
    const auto middleParameter = (first.parameter + last.parameter) * 0.5;
    const auto middle = evaluateBoundary(document, constraint, middleParameter);
    if (!middle.has_value()) {
        return false;
    }
    const auto chordError = pointSegmentDistance(middle->point, first.point, last.point);
    const auto normalError = first.normalDefined && last.normalDefined
        ? clampedAngleDegrees(first.normal, last.normal)
        : 0.0;
    if (depth < 20 &&
        (chordError > chordTolerance || normalError > normalToleranceDegrees) &&
        parameters.size() < 16384) {
        if (!appendAdaptiveBoundaryParameters(
                document,
                constraint,
                first,
                *middle,
                chordTolerance,
                normalToleranceDegrees,
                depth + 1,
                parameters,
                maximumChordError)) {
            return false;
        }
        return appendAdaptiveBoundaryParameters(
            document,
            constraint,
            *middle,
            last,
            chordTolerance,
            normalToleranceDegrees,
            depth + 1,
            parameters,
            maximumChordError);
    }
    maximumChordError = std::max(maximumChordError, chordError);
    parameters.push_back(last.parameter);
    return true;
}

std::vector<double> adaptiveBoundaryParameters(
    const ShapeDocument& document,
    const RegionBoundaryEdgeConstraint& constraint,
    const double chordTolerance,
    const double normalToleranceDegrees,
    double& maximumChordError) {
    std::vector<double> parameters;
    const auto first = evaluateBoundary(document, constraint, constraint.selectedFirstParameter);
    const auto last = evaluateBoundary(document, constraint, constraint.selectedLastParameter);
    if (!first.has_value() || !last.has_value()) {
        return parameters;
    }
    parameters.push_back(first->parameter);
    if (!appendAdaptiveBoundaryParameters(
            document,
            constraint,
            *first,
            *last,
            chordTolerance,
            normalToleranceDegrees,
            0,
            parameters,
            maximumChordError)) {
        parameters.clear();
    }
    return parameters;
}

bool containsBoth(const LocalTriangle& triangle, const std::size_t first, const std::size_t second) {
    return std::find(triangle.vertices.begin(), triangle.vertices.end(), first) != triangle.vertices.end() &&
        std::find(triangle.vertices.begin(), triangle.vertices.end(), second) != triangle.vertices.end();
}

bool splitBoundarySegment(
    std::vector<LocalTriangle>& triangles,
    const std::size_t first,
    const std::size_t second,
    const std::vector<std::size_t>& inserted) {
    if (inserted.empty()) {
        return true;
    }
    const auto found = std::find_if(triangles.begin(), triangles.end(), [&](const auto& triangle) {
        return containsBoth(triangle, first, second);
    });
    if (found == triangles.end()) {
        return false;
    }

    std::size_t third = 0;
    for (const auto vertex : found->vertices) {
        if (vertex != first && vertex != second) {
            third = vertex;
            break;
        }
    }
    bool firstToSecond = false;
    for (int index = 0; index < 3; ++index) {
        if (found->vertices[static_cast<std::size_t>(index)] == first &&
            found->vertices[static_cast<std::size_t>((index + 1) % 3)] == second) {
            firstToSecond = true;
            break;
        }
    }

    std::vector<std::size_t> chain;
    chain.reserve(inserted.size() + 2);
    if (firstToSecond) {
        chain.push_back(first);
        chain.insert(chain.end(), inserted.begin(), inserted.end());
        chain.push_back(second);
    } else {
        chain.push_back(second);
        chain.insert(chain.end(), inserted.rbegin(), inserted.rend());
        chain.push_back(first);
    }

    const auto replaceIndex = static_cast<std::size_t>(std::distance(triangles.begin(), found));
    triangles[replaceIndex] = {{chain[0], chain[1], third}};
    for (std::size_t index = 1; index + 1 < chain.size(); ++index) {
        triangles.push_back({{chain[index], chain[index + 1], third}});
    }
    return true;
}

const RegionBoundaryEdgeConstraint* findBoundaryConstraint(
    const RegionTopologyConstraints& constraints,
    const EdgeId edgeId) {
    const auto found = std::find_if(
        constraints.boundaryEdges.begin(),
        constraints.boundaryEdges.end(),
        [edgeId](const auto& edge) { return edge.edgeId == edgeId; });
    return found == constraints.boundaryEdges.end() ? nullptr : &*found;
}

bool parameterBetween(const double value, const double first, const double last, const double tolerance) {
    return value >= std::min(first, last) - tolerance && value <= std::max(first, last) + tolerance;
}

std::optional<std::size_t> findExistingParameter(
    const std::vector<double>& parameters,
    const double value,
    const double tolerance) {
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (std::abs(parameters[index] - value) <= tolerance) {
            return index;
        }
    }
    return std::nullopt;
}

bool addBoundarySamples(
    const ShapeDocument& document,
    const RegionTopologyConstraints& constraints,
    const RegionBoundaryEdgeConstraint& constraint,
    const TopoDS_Edge& copiedEdge,
    const std::vector<double>& requiredParameters,
    LocalFaceMesh& faceMesh) {
    const auto& polygon = BRep_Tool::PolygonOnTriangulation(
        copiedEdge,
        faceMesh.triangulation,
        faceMesh.location);
    if (polygon.IsNull() || !polygon->HasParameters()) {
        return false;
    }

    const auto& nodes = polygon->Nodes();
    const auto& polygonParameters = polygon->Parameters()->Array1();
    if (nodes.Length() != polygonParameters.Length() || nodes.Length() < 2) {
        return false;
    }

    std::vector<std::size_t> localNodes;
    std::vector<double> parameters;
    localNodes.reserve(static_cast<std::size_t>(nodes.Length()));
    parameters.reserve(static_cast<std::size_t>(nodes.Length()));
    for (int offset = 0; offset < nodes.Length(); ++offset) {
        const auto arrayIndex = nodes.Lower() + offset;
        const auto localIndex = static_cast<std::size_t>(nodes(arrayIndex) - 1);
        localNodes.push_back(localIndex);
        parameters.push_back(polygonParameters(polygonParameters.Lower() + offset));
        auto& vertex = faceMesh.vertices[localIndex].value;
        vertex.boundary = true;
        vertex.hardBoundary = true;
        auto& boundaryEdges = faceMesh.vertices[localIndex].boundaryEdges;
        if (std::find(boundaryEdges.begin(), boundaryEdges.end(), constraint.edgeId) == boundaryEdges.end()) {
            boundaryEdges.push_back(constraint.edgeId);
        }
        auto& boundaryEdgeIds = faceMesh.vertices[localIndex].value.boundaryEdgeIds;
        if (std::find(boundaryEdgeIds.begin(), boundaryEdgeIds.end(), constraint.edgeId) == boundaryEdgeIds.end()) {
            boundaryEdgeIds.push_back(constraint.edgeId);
        }
    }

    const auto parameterTolerance = std::max(
        1.0e-12,
        std::abs(constraint.selectedLastParameter - constraint.selectedFirstParameter) * 1.0e-12);
    std::map<double, std::size_t> requiredVertices;
    for (const auto parameter : requiredParameters) {
        const auto existing = findExistingParameter(parameters, parameter, parameterTolerance);
        if (existing.has_value()) {
            requiredVertices.emplace(parameter, localNodes[*existing]);
            continue;
        }
        const auto evaluated = evaluateBoundary(document, constraint, parameter);
        if (!evaluated.has_value()) {
            return false;
        }
        const auto uv = constraint.selectedPcurve->Value(parameter);
        LocalVertex vertex;
        vertex.value = makeVertex(
            document.topology().face(constraint.selectedFaceId),
            constraint.selectedFaceId,
            evaluated->point,
            uv);
        vertex.value.boundary = true;
        vertex.value.hardBoundary = true;
        vertex.boundaryEdges.push_back(constraint.edgeId);
        vertex.value.boundaryEdgeIds.push_back(constraint.edgeId);
        const auto localIndex = faceMesh.vertices.size();
        faceMesh.vertices.push_back(std::move(vertex));
        requiredVertices.emplace(parameter, localIndex);
    }

    for (std::size_t segment = 0; segment + 1 < localNodes.size(); ++segment) {
        std::vector<std::pair<double, std::size_t>> inserted;
        for (const auto& [parameter, localIndex] : requiredVertices) {
            if (localIndex == localNodes[segment] || localIndex == localNodes[segment + 1]) {
                continue;
            }
            if (parameterBetween(parameter, parameters[segment], parameters[segment + 1], parameterTolerance)) {
                inserted.emplace_back(parameter, localIndex);
            }
        }
        std::sort(inserted.begin(), inserted.end(), [&](const auto& lhs, const auto& rhs) {
            return parameters[segment] <= parameters[segment + 1]
                ? lhs.first < rhs.first
                : lhs.first > rhs.first;
        });
        std::vector<std::size_t> insertedIndices;
        insertedIndices.reserve(inserted.size());
        for (const auto& value : inserted) {
            insertedIndices.push_back(value.second);
        }
        if (!splitBoundarySegment(
                faceMesh.triangles,
                localNodes[segment],
                localNodes[segment + 1],
                insertedIndices)) {
            return false;
        }
    }

    for (const auto& [parameter, localIndex] : requiredVertices) {
        const bool endpoint =
            std::abs(parameter - constraint.selectedFirstParameter) <= parameterTolerance ||
            std::abs(parameter - constraint.selectedLastParameter) <= parameterTolerance;
        auto& vertex = faceMesh.vertices[localIndex].value;
        vertex.boundaryEndpoint = vertex.boundaryEndpoint || endpoint;
        faceMesh.boundarySamples.push_back({constraint.edgeId, parameter, localIndex, endpoint});
    }
    return true;
}

WeldKey weldKey(const EdgeId edgeId, const gp_Pnt& point, const double tolerance) {
    return {
        edgeId,
        static_cast<std::int64_t>(std::llround(point.X() / tolerance)),
        static_cast<std::int64_t>(std::llround(point.Y() / tolerance)),
        static_cast<std::int64_t>(std::llround(point.Z() / tolerance))};
}

const RegionReferenceSourceSample* sourceForFace(
    const RegionReferenceVertex& vertex,
    const FaceId faceId) {
    const auto found = std::find_if(vertex.sources.begin(), vertex.sources.end(), [faceId](const auto& source) {
        return source.faceId == faceId;
    });
    return found == vertex.sources.end() ? nullptr : &*found;
}

double unwrapPeriodicParameter(
    double value,
    const double reference,
    const double period) {
    if (!(period > 0.0)) {
        return value;
    }
    while (value - reference > period * 0.5) {
        value -= period;
    }
    while (reference - value > period * 0.5) {
        value += period;
    }
    return value;
}

gp_Pnt2d triangleUvCentroid(
    const BRepAdaptor_Surface& surface,
    const RegionReferenceSourceSample& first,
    const RegionReferenceSourceSample& second,
    const RegionReferenceSourceSample& third) {
    auto secondU = second.uv.X();
    auto thirdU = third.uv.X();
    auto secondV = second.uv.Y();
    auto thirdV = third.uv.Y();
    if (surface.IsUPeriodic()) {
        secondU = unwrapPeriodicParameter(secondU, first.uv.X(), surface.UPeriod());
        thirdU = unwrapPeriodicParameter(thirdU, first.uv.X(), surface.UPeriod());
    }
    if (surface.IsVPeriodic()) {
        secondV = unwrapPeriodicParameter(secondV, first.uv.Y(), surface.VPeriod());
        thirdV = unwrapPeriodicParameter(thirdV, first.uv.Y(), surface.VPeriod());
    }
    return {
        (first.uv.X() + secondU + thirdU) / 3.0,
        (first.uv.Y() + secondV + thirdV) / 3.0};
}

void orientLocalTriangle(
    const TopoDS_Face& originalFace,
    const LocalFaceMesh& faceMesh,
    LocalTriangle& triangle) {
    const auto& first = faceMesh.vertices[triangle.vertices[0]];
    const auto& second = faceMesh.vertices[triangle.vertices[1]];
    const auto& third = faceMesh.vertices[triangle.vertices[2]];
    gp_Vec triangleNormal(first.value.point, second.value.point);
    triangleNormal.Cross(gp_Vec(first.value.point, third.value.point));
    if (triangleNormal.SquareMagnitude() <= 1.0e-24) {
        return;
    }
    BRepAdaptor_Surface surface(originalFace, true);
    const auto uv = triangleUvCentroid(
        surface,
        first.value.sources.front(),
        second.value.sources.front(),
        third.value.sources.front());
    const auto exactSource = evaluateSource(originalFace, faceMesh.faceId, uv);
    if (exactSource.normalDefined && gp_Dir(triangleNormal).Dot(exactSource.normal) < 0.0) {
        std::swap(triangle.vertices[1], triangle.vertices[2]);
    }
}

double localTriangleNormalError(
    const TopoDS_Face& originalFace,
    const LocalFaceMesh& faceMesh,
    const LocalTriangle& triangle,
    gp_Pnt2d* centroidUv) {
    const auto& first = faceMesh.vertices[triangle.vertices[0]];
    const auto& second = faceMesh.vertices[triangle.vertices[1]];
    const auto& third = faceMesh.vertices[triangle.vertices[2]];
    BRepAdaptor_Surface surface(originalFace, true);
    const auto uv = triangleUvCentroid(
        surface,
        first.value.sources.front(),
        second.value.sources.front(),
        third.value.sources.front());
    if (centroidUv != nullptr) {
        *centroidUv = uv;
    }

    const auto exactSource = evaluateSource(originalFace, faceMesh.faceId, uv);
    double maximumError = 0.0;
    std::vector<gp_Dir> definedNormals;
    for (const auto* vertex : {&first, &second, &third}) {
        if (vertex->value.normalDiscontinuity) {
            continue;
        }
        const auto& source = vertex->value.sources.front();
        if (source.normalDefined) {
            definedNormals.push_back(source.normal);
            if (exactSource.normalDefined) {
                maximumError = std::max(
                    maximumError,
                    clampedAngleDegrees(source.normal, exactSource.normal));
            }
        }
    }
    for (std::size_t firstIndex = 0; firstIndex < definedNormals.size(); ++firstIndex) {
        for (std::size_t secondIndex = firstIndex + 1; secondIndex < definedNormals.size(); ++secondIndex) {
            maximumError = std::max(
                maximumError,
                clampedAngleDegrees(definedNormals[firstIndex], definedNormals[secondIndex]));
        }
    }
    return maximumError;
}

bool refineLocalTrianglesForNormals(
    const TopoDS_Face& originalFace,
    const double maximumNormalErrorDegrees,
    const double discontinuityLengthThreshold,
    const std::size_t maximumTriangles,
    LocalFaceMesh& faceMesh,
    std::size_t& refinementCount,
    std::size_t& discontinuityCount,
    std::string& failureMessage) {
    for (auto& triangle : faceMesh.triangles) {
        orientLocalTriangle(originalFace, faceMesh, triangle);
    }

    using LocalEdge = std::pair<std::size_t, std::size_t>;
    const auto edgeKey = [](const std::size_t first, const std::size_t second) {
        return LocalEdge {std::min(first, second), std::max(first, second)};
    };
    const auto edgeNormalError = [&](const LocalEdge& edge) {
        const auto& first = faceMesh.vertices[edge.first].value.sources.front();
        const auto& second = faceMesh.vertices[edge.second].value.sources.front();
        return first.normalDefined && second.normalDefined
            ? clampedAngleDegrees(first.normal, second.normal)
            : 0.0;
    };
    const auto isHardBoundaryEdge = [&](const LocalEdge& edge) {
        const auto& first = faceMesh.vertices[edge.first].boundaryEdges;
        const auto& second = faceMesh.vertices[edge.second].boundaryEdges;
        return std::any_of(first.begin(), first.end(), [&](const auto edgeId) {
            return std::find(second.begin(), second.end(), edgeId) != second.end();
        });
    };

    for (int iteration = 0; iteration < kMaximumNormalRefinementIterations; ++iteration) {
        std::set<LocalEdge> splitEdges;
        for (const auto& triangle : faceMesh.triangles) {
            if (localTriangleNormalError(
                    originalFace,
                    faceMesh,
                    triangle,
                    nullptr) <= maximumNormalErrorDegrees) {
                continue;
            }
            const std::array<LocalEdge, 3> edges {
                edgeKey(triangle.vertices[0], triangle.vertices[1]),
                edgeKey(triangle.vertices[1], triangle.vertices[2]),
                edgeKey(triangle.vertices[2], triangle.vertices[0])};
            bool selected = false;
            for (const auto& edge : edges) {
                if (!isHardBoundaryEdge(edge) &&
                    edgeNormalError(edge) > maximumNormalErrorDegrees) {
                    const auto edgeLength = faceMesh.vertices[edge.first].value.point.Distance(
                        faceMesh.vertices[edge.second].value.point);
                    if (edgeLength <= discontinuityLengthThreshold) {
                        for (const auto vertexIndex : {edge.first, edge.second}) {
                            auto& vertex = faceMesh.vertices[vertexIndex].value;
                            if (!vertex.normalDiscontinuity) {
                                vertex.normalDiscontinuity = true;
                                ++discontinuityCount;
                            }
                        }
                    } else {
                        splitEdges.insert(edge);
                    }
                    selected = true;
                }
            }
            if (!selected) {
                const auto longest = std::max_element(edges.begin(), edges.end(), [&](const auto& lhs, const auto& rhs) {
                    return faceMesh.vertices[lhs.first].value.point.SquareDistance(
                               faceMesh.vertices[lhs.second].value.point) <
                        faceMesh.vertices[rhs.first].value.point.SquareDistance(
                               faceMesh.vertices[rhs.second].value.point);
                });
                if (longest != edges.end() && !isHardBoundaryEdge(*longest)) {
                    splitEdges.insert(*longest);
                }
            }
        }
        if (splitEdges.empty()) {
            const auto remaining = std::find_if(
                faceMesh.triangles.begin(),
                faceMesh.triangles.end(),
                [&](const auto& triangle) {
                    return localTriangleNormalError(
                        originalFace,
                        faceMesh,
                        triangle,
                        nullptr) > maximumNormalErrorDegrees;
                });
            if (remaining == faceMesh.triangles.end()) {
                return true;
            }
            std::ostringstream message;
            message << "FaceId " << faceMesh.faceId
                    << " contains a normal-error triangle that cannot be refined without changing a hard STEP boundary.";
            failureMessage = message.str();
            return false;
        }

        BRepAdaptor_Surface surface(originalFace, true);
        std::map<LocalEdge, std::size_t> midpointVertices;
        for (const auto& edge : splitEdges) {
            const auto& first = faceMesh.vertices[edge.first];
            const auto& second = faceMesh.vertices[edge.second];
            auto secondU = second.value.sources.front().uv.X();
            auto secondV = second.value.sources.front().uv.Y();
            if (surface.IsUPeriodic()) {
                secondU = unwrapPeriodicParameter(
                    secondU,
                    first.value.sources.front().uv.X(),
                    surface.UPeriod());
            }
            if (surface.IsVPeriodic()) {
                secondV = unwrapPeriodicParameter(
                    secondV,
                    first.value.sources.front().uv.Y(),
                    surface.VPeriod());
            }
            const gp_Pnt2d uv(
                (first.value.sources.front().uv.X() + secondU) * 0.5,
                (first.value.sources.front().uv.Y() + secondV) * 0.5);
            LocalVertex midpoint;
            midpoint.value = makeVertex(
                originalFace,
                faceMesh.faceId,
                surface.Value(uv.X(), uv.Y()),
                uv);
            for (const auto seamEdge : first.internalSeamEdges) {
                if (std::find(
                        second.internalSeamEdges.begin(),
                        second.internalSeamEdges.end(),
                        seamEdge) != second.internalSeamEdges.end()) {
                    midpoint.internalSeamEdges.push_back(seamEdge);
                }
            }
            midpoint.value.internalSeam = !midpoint.internalSeamEdges.empty();
            const auto midpointIndex = faceMesh.vertices.size();
            faceMesh.vertices.push_back(std::move(midpoint));
            midpointVertices.emplace(edge, midpointIndex);
            ++refinementCount;
        }

        std::vector<LocalTriangle> refinedTriangles;
        refinedTriangles.reserve(faceMesh.triangles.size() + splitEdges.size() * 2);
        const auto append = [&](const std::size_t first, const std::size_t second, const std::size_t third) {
            LocalTriangle triangle {{first, second, third}};
            orientLocalTriangle(originalFace, faceMesh, triangle);
            refinedTriangles.push_back(triangle);
        };
        for (const auto& triangle : faceMesh.triangles) {
            const auto v0 = triangle.vertices[0];
            const auto v1 = triangle.vertices[1];
            const auto v2 = triangle.vertices[2];
            const auto m0 = midpointVertices.find(edgeKey(v0, v1));
            const auto m1 = midpointVertices.find(edgeKey(v1, v2));
            const auto m2 = midpointVertices.find(edgeKey(v2, v0));
            const int mask = (m0 != midpointVertices.end() ? 1 : 0) |
                (m1 != midpointVertices.end() ? 2 : 0) |
                (m2 != midpointVertices.end() ? 4 : 0);
            switch (mask) {
            case 0:
                refinedTriangles.push_back(triangle);
                break;
            case 1:
                append(v0, m0->second, v2);
                append(m0->second, v1, v2);
                break;
            case 2:
                append(v1, m1->second, v0);
                append(m1->second, v2, v0);
                break;
            case 4:
                append(v2, m2->second, v1);
                append(m2->second, v0, v1);
                break;
            case 3:
                append(v1, m1->second, m0->second);
                append(v0, m0->second, v2);
                append(m0->second, m1->second, v2);
                break;
            case 6:
                append(v2, m2->second, m1->second);
                append(v1, m1->second, v0);
                append(m1->second, m2->second, v0);
                break;
            case 5:
                append(v0, m0->second, m2->second);
                append(v2, m2->second, v1);
                append(m2->second, m0->second, v1);
                break;
            case 7:
                append(v0, m0->second, m2->second);
                append(m0->second, v1, m1->second);
                append(m2->second, m1->second, v2);
                append(m0->second, m1->second, m2->second);
                break;
            }
        }
        if (refinedTriangles.size() > maximumTriangles) {
            std::ostringstream message;
            message << "FaceId " << faceMesh.faceId
                    << " normal refinement exceeded " << maximumTriangles
                    << " triangles; refined_triangles=" << refinedTriangles.size() << '.';
            failureMessage = message.str();
            return false;
        }
        faceMesh.triangles = std::move(refinedTriangles);
    }

    double remainingMaximum = 0.0;
    std::size_t remainingTriangleIndex = 0;
    for (std::size_t triangleIndex = 0; triangleIndex < faceMesh.triangles.size(); ++triangleIndex) {
        const auto error = localTriangleNormalError(
                originalFace,
                faceMesh,
                faceMesh.triangles[triangleIndex],
                nullptr);
        if (error > remainingMaximum) {
            remainingMaximum = error;
            remainingTriangleIndex = triangleIndex;
        }
    }
    if (remainingMaximum > maximumNormalErrorDegrees) {
        const auto& triangle = faceMesh.triangles[remainingTriangleIndex];
        const auto& first = faceMesh.vertices[triangle.vertices[0]];
        const auto& second = faceMesh.vertices[triangle.vertices[1]];
        const auto& third = faceMesh.vertices[triangle.vertices[2]];
        gp_Vec triangleNormal(first.value.point, second.value.point);
        triangleNormal.Cross(gp_Vec(first.value.point, third.value.point));
        const auto maximumEdgeSquared = std::max({
            first.value.point.SquareDistance(second.value.point),
            second.value.point.SquareDistance(third.value.point),
            third.value.point.SquareDistance(first.value.point)});
        const auto quality = maximumEdgeSquared > 0.0
            ? triangleNormal.Magnitude() / maximumEdgeSquared
            : 0.0;
        gp_Pnt2d uv;
        localTriangleNormalError(originalFace, faceMesh, triangle, &uv);
        const auto exact = evaluateSource(originalFace, faceMesh.faceId, uv);
        const auto dot = triangleNormal.SquareMagnitude() > 1.0e-24 && exact.normalDefined
            ? gp_Dir(triangleNormal).Dot(exact.normal)
            : 0.0;
        const auto normalErrorFor = [&](const LocalVertex& vertex) {
            const auto& source = vertex.value.sources.front();
            return source.normalDefined && exact.normalDefined
                ? clampedAngleDegrees(source.normal, exact.normal)
                : -1.0;
        };
        std::ostringstream message;
        message << "FaceId " << faceMesh.faceId
                << " normal refinement did not converge after "
                << kMaximumNormalRefinementIterations << " iterations; local_triangles="
                << faceMesh.triangles.size() << ", refinements=" << refinementCount
                << ", remaining_normal_degrees=" << remainingMaximum
                << ", triangle_quality=" << quality << ", normal_dot=" << dot
                << ", uv=" << uv.X() << ',' << uv.Y()
                << ", p0=" << first.value.point.X() << ',' << first.value.point.Y() << ',' << first.value.point.Z()
                << ", p1=" << second.value.point.X() << ',' << second.value.point.Y() << ',' << second.value.point.Z()
                << ", p2=" << third.value.point.X() << ',' << third.value.point.Y() << ',' << third.value.point.Z()
                << ", flags0=" << first.value.boundary << first.value.boundaryEndpoint << first.value.internalSeam
                << ", flags1=" << second.value.boundary << second.value.boundaryEndpoint << second.value.internalSeam
                << ", flags2=" << third.value.boundary << third.value.boundaryEndpoint << third.value.internalSeam
                << ", source_normal_errors=" << normalErrorFor(first) << ','
                << normalErrorFor(second) << ',' << normalErrorFor(third)
                << '.';
        failureMessage = message.str();
        return false;
    }
    return true;
}

void validateInterior(
    const ShapeDocument& document,
    StepAdaptiveRegionSamplingReport& report,
    const RegionReferenceMesh& mesh) {
    for (std::size_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
        const auto& triangle = mesh.triangles[triangleIndex];
        const auto* first = sourceForFace(mesh.vertices[triangle.vertices[0]], triangle.faceId);
        const auto* second = sourceForFace(mesh.vertices[triangle.vertices[1]], triangle.faceId);
        const auto* third = sourceForFace(mesh.vertices[triangle.vertices[2]], triangle.faceId);
        if (first == nullptr || second == nullptr || third == nullptr) {
            continue;
        }
        const auto& face = document.topology().face(triangle.faceId);
        BRepAdaptor_Surface surface(face, true);
        const auto uv = triangleUvCentroid(surface, *first, *second, *third);
        const auto exactPoint = surface.Value(uv.X(), uv.Y());
        const auto linearPoint = gp_Pnt(
            (mesh.vertices[triangle.vertices[0]].point.X() +
                mesh.vertices[triangle.vertices[1]].point.X() +
                mesh.vertices[triangle.vertices[2]].point.X()) / 3.0,
            (mesh.vertices[triangle.vertices[0]].point.Y() +
                mesh.vertices[triangle.vertices[1]].point.Y() +
                mesh.vertices[triangle.vertices[2]].point.Y()) / 3.0,
            (mesh.vertices[triangle.vertices[0]].point.Z() +
                mesh.vertices[triangle.vertices[1]].point.Z() +
                mesh.vertices[triangle.vertices[2]].point.Z()) / 3.0);
        report.maximumInteriorChordError = std::max(
            report.maximumInteriorChordError,
            exactPoint.Distance(linearPoint));

        const auto exactSource = evaluateSource(face, triangle.faceId, uv);
        if (!exactSource.normalDefined) {
            continue;
        }
        double normalError = 0.0;
        for (const auto* source : {first, second, third}) {
            const auto vertexIndex = source == first
                ? triangle.vertices[0]
                : (source == second ? triangle.vertices[1] : triangle.vertices[2]);
            if (source->normalDefined && !mesh.vertices[vertexIndex].normalDiscontinuity) {
                normalError = std::max(
                    normalError,
                    clampedAngleDegrees(source->normal, exactSource.normal));
            }
        }
        if (normalError > report.maximumInteriorNormalErrorDegrees) {
            report.maximumInteriorNormalErrorDegrees = normalError;
            report.maximumInteriorNormalFaceId = triangle.faceId;
            report.maximumInteriorNormalTriangleIndex = triangleIndex;
        }
    }
}

}

bool StepAdaptiveRegionSamplingReport::success() const {
    return status == RegionRefitStatus::Success;
}

StepAdaptiveRegionSamplingResult StepAdaptiveRegionSampler::sample(
    const ShapeDocument& document,
    const RegionTopologyConstraints& constraints,
    const RegionRefitOptions& options) const {
    if (!document.hasShape() || constraints.selectedFaces.empty() || constraints.boundaryEdges.empty()) {
        return fail(RegionRefitStatus::InvalidRequest, "Adaptive sampling requires validated region constraints.");
    }

    StepAdaptiveRegionSamplingResult result;
    auto& report = result.report;
    report.sourceFaceCount = constraints.selectedFaces.size();
    report.linearDeflection = std::min(options.tolerance3d / 4.0, 0.01);
    report.angularDeflectionDegrees = std::min(options.normalToleranceDegrees / 3.0, 5.0);
    report.boundaryChordTolerance = options.curveOnSurfaceTolerance * 0.5;
    if (report.linearDeflection <= 0.0 || report.angularDeflectionDegrees <= 0.0 ||
        report.boundaryChordTolerance <= 0.0 || options.maximumSampleTriangles == 0) {
        return fail(RegionRefitStatus::InvalidRequest, "Adaptive sampling tolerances and budgets must be positive.");
    }

    BRepBuilderAPI_Copy copier(document.shape(), true, false);
    if (!copier.IsDone() || copier.Shape().IsNull()) {
        return fail(RegionRefitStatus::Failed, "Could not create an isolated STEP sampling copy.");
    }

    BRep_Builder compoundBuilder;
    TopoDS_Compound selectedCompound;
    compoundBuilder.MakeCompound(selectedCompound);
    std::map<FaceId, TopoDS_Face> copiedFaces;
    for (const auto faceId : constraints.selectedFaces) {
        const auto copiedShape = copier.ModifiedShape(document.topology().face(faceId));
        if (copiedShape.IsNull() || copiedShape.ShapeType() != TopAbs_FACE) {
            return fail(RegionRefitStatus::Failed, "Could not map a selected face into the isolated sampling copy.");
        }
        const auto copiedFace = TopoDS::Face(copiedShape);
        copiedFaces.emplace(faceId, copiedFace);
        compoundBuilder.Add(selectedCompound, copiedFace);
    }

    BRepMesh_IncrementalMesh mesher(
        selectedCompound,
        report.linearDeflection,
        false,
        report.angularDeflectionDegrees * kPi / 180.0,
        true);
    mesher.Perform();
    if (!mesher.IsDone()) {
        return fail(RegionRefitStatus::Failed, "Open CASCADE adaptive meshing did not complete.");
    }

    const std::set<EdgeId> internalEdges(
        constraints.internalEdges.begin(),
        constraints.internalEdges.end());
    std::vector<LocalFaceMesh> localMeshes;
    localMeshes.reserve(constraints.selectedFaces.size());
    for (const auto faceId : constraints.selectedFaces) {
        LocalFaceMesh faceMesh;
        faceMesh.faceId = faceId;
        faceMesh.face = copiedFaces.at(faceId);
        faceMesh.triangulation = BRep_Tool::Triangulation(faceMesh.face, faceMesh.location);
        if (faceMesh.triangulation.IsNull() || !faceMesh.triangulation->HasUVNodes()) {
            return fail(RegionRefitStatus::Failed, "A selected STEP face has no UV-aware triangulation.");
        }

        const auto transform = faceMesh.location.Transformation();
        faceMesh.vertices.reserve(static_cast<std::size_t>(faceMesh.triangulation->NbNodes()));
        for (int node = 1; node <= faceMesh.triangulation->NbNodes(); ++node) {
            const auto point = faceMesh.triangulation->Node(node).Transformed(transform);
            const auto uv = faceMesh.triangulation->UVNode(node);
            LocalVertex vertex;
            vertex.value = makeVertex(
                document.topology().face(faceId),
                faceId,
                point,
                uv);
            faceMesh.vertices.push_back(std::move(vertex));
        }

        faceMesh.triangles.reserve(static_cast<std::size_t>(faceMesh.triangulation->NbTriangles()));
        for (int triangle = 1; triangle <= faceMesh.triangulation->NbTriangles(); ++triangle) {
            int first = 0;
            int second = 0;
            int third = 0;
            faceMesh.triangulation->Triangle(triangle).Get(first, second, third);
            if (faceMesh.face.Orientation() == TopAbs_REVERSED) {
                std::swap(second, third);
            }
            faceMesh.triangles.push_back({{
                static_cast<std::size_t>(first - 1),
                static_cast<std::size_t>(second - 1),
                static_cast<std::size_t>(third - 1)}});
        }

        for (const auto edgeId : document.topology().edgesForFace(faceId)) {
            const auto copiedShape = copier.ModifiedShape(document.topology().edge(edgeId));
            if (copiedShape.IsNull() || copiedShape.ShapeType() != TopAbs_EDGE) {
                continue;
            }
            const auto copiedEdge = TopoDS::Edge(copiedShape);
            const auto& polygon = BRep_Tool::PolygonOnTriangulation(
                copiedEdge,
                faceMesh.triangulation,
                faceMesh.location);
            if (polygon.IsNull()) {
                continue;
            }
            if (internalEdges.find(edgeId) != internalEdges.end()) {
                for (const auto node : polygon->Nodes()) {
                    auto& vertex = faceMesh.vertices[static_cast<std::size_t>(node - 1)];
                    vertex.value.internalSeam = true;
                    vertex.internalSeamEdges.push_back(edgeId);
                }
            }
        }

        for (const auto& constraint : constraints.boundaryEdges) {
            if (constraint.selectedFaceId != faceId) {
                continue;
            }
            double maximumChordError = 0.0;
            const auto requiredParameters = adaptiveBoundaryParameters(
                document,
                constraint,
                report.boundaryChordTolerance,
                report.angularDeflectionDegrees,
                maximumChordError);
            report.maximumBoundaryChordError = std::max(
                report.maximumBoundaryChordError,
                maximumChordError);
            if (requiredParameters.size() < 2) {
                return fail(RegionRefitStatus::Failed, "Could not adaptively sample an outer STEP edge.");
            }
            const auto copiedShape = copier.ModifiedShape(document.topology().edge(constraint.edgeId));
            if (copiedShape.IsNull() || copiedShape.ShapeType() != TopAbs_EDGE ||
                !addBoundarySamples(
                    document,
                    constraints,
                    constraint,
                    TopoDS::Edge(copiedShape),
                    requiredParameters,
                    faceMesh)) {
                return fail(RegionRefitStatus::Failed, "Could not force adaptive boundary samples into the reference mesh.");
            }
        }
        std::string normalRefinementFailure;
        const auto normalDiscontinuityLengthThreshold = std::max(
            Precision::Confusion() * 100.0,
            std::min(report.linearDeflection * 0.05, report.boundaryChordTolerance));
        if (!refineLocalTrianglesForNormals(
                document.topology().face(faceId),
                report.angularDeflectionDegrees,
                normalDiscontinuityLengthThreshold,
                options.maximumSampleTriangles,
                faceMesh,
                report.normalRefinementCount,
                report.normalDiscontinuityVertexCount,
                normalRefinementFailure)) {
            return fail(RegionRefitStatus::SamplingBudgetExceeded, std::move(normalRefinementFailure));
        }
        localMeshes.push_back(std::move(faceMesh));
    }

    const auto weldTolerance = std::max(
        Precision::Confusion() * 10.0,
        std::min(options.tolerance3d * 0.1, options.curveOnSurfaceTolerance * 0.25));
    std::unordered_map<WeldKey, std::size_t, WeldKeyHash> weldedVertices;
    std::vector<gp_Vec> normalSums;
    std::vector<std::vector<std::size_t>> localToGlobal(localMeshes.size());
    for (std::size_t faceIndex = 0; faceIndex < localMeshes.size(); ++faceIndex) {
        const auto& faceMesh = localMeshes[faceIndex];
        auto& mapping = localToGlobal[faceIndex];
        mapping.resize(faceMesh.vertices.size());
        for (std::size_t localIndex = 0; localIndex < faceMesh.vertices.size(); ++localIndex) {
            const auto& local = faceMesh.vertices[localIndex];
            std::optional<std::size_t> globalIndex;
            for (const auto edgeId : local.internalSeamEdges) {
                const auto found = weldedVertices.find(weldKey(edgeId, local.value.point, weldTolerance));
                if (found != weldedVertices.end()) {
                    globalIndex = found->second;
                    break;
                }
            }
            if (!globalIndex.has_value()) {
                globalIndex = result.mesh.vertices.size();
                result.mesh.vertices.push_back(local.value);
                normalSums.emplace_back(local.value.normal);
            } else {
                auto& existing = result.mesh.vertices[*globalIndex];
                existing.boundary = existing.boundary || local.value.boundary;
                existing.hardBoundary = existing.hardBoundary || local.value.hardBoundary;
                existing.boundaryEndpoint = existing.boundaryEndpoint || local.value.boundaryEndpoint;
                existing.internalSeam = existing.internalSeam || local.value.internalSeam;
                existing.sources.insert(
                    existing.sources.end(),
                    local.value.sources.begin(),
                    local.value.sources.end());
                for (const auto edgeId : local.value.boundaryEdgeIds) {
                    if (std::find(existing.boundaryEdgeIds.begin(), existing.boundaryEdgeIds.end(), edgeId) ==
                        existing.boundaryEdgeIds.end()) {
                        existing.boundaryEdgeIds.push_back(edgeId);
                    }
                }
                normalSums[*globalIndex] += gp_Vec(local.value.normal);
            }
            for (const auto edgeId : local.internalSeamEdges) {
                weldedVertices.emplace(weldKey(edgeId, local.value.point, weldTolerance), *globalIndex);
            }
            mapping[localIndex] = *globalIndex;
        }
    }

    for (std::size_t index = 0; index < result.mesh.vertices.size(); ++index) {
        if (normalSums[index].SquareMagnitude() > 1.0e-24) {
            result.mesh.vertices[index].normal = gp_Dir(normalSums[index]);
        }
        if (result.mesh.vertices[index].internalSeam) {
            ++report.internalSeamVertexCount;
            if (result.mesh.vertices[index].sources.size() > 1) {
                ++report.weldedInternalSeamVertexCount;
            }
        }
    }

    for (std::size_t faceIndex = 0; faceIndex < localMeshes.size(); ++faceIndex) {
        const auto& faceMesh = localMeshes[faceIndex];
        const auto& mapping = localToGlobal[faceIndex];
        for (const auto& localTriangle : faceMesh.triangles) {
            const auto& firstPoint = faceMesh.vertices[localTriangle.vertices[0]].value.point;
            const auto& secondPoint = faceMesh.vertices[localTriangle.vertices[1]].value.point;
            const auto& thirdPoint = faceMesh.vertices[localTriangle.vertices[2]].value.point;
            gp_Vec areaVector(firstPoint, secondPoint);
            areaVector.Cross(gp_Vec(firstPoint, thirdPoint));
            const auto maximumEdgeSquared = std::max({
                firstPoint.SquareDistance(secondPoint),
                secondPoint.SquareDistance(thirdPoint),
                thirdPoint.SquareDistance(firstPoint)});
            if (maximumEdgeSquared <= 1.0e-24 ||
                areaVector.Magnitude() / maximumEdgeSquared <= kMinimumTriangleShapeQuality) {
                ++report.discardedDegenerateTriangleCount;
                continue;
            }
            RegionReferenceTriangle triangle;
            triangle.faceId = faceMesh.faceId;
            for (int corner = 0; corner < 3; ++corner) {
                triangle.vertices[static_cast<std::size_t>(corner)] =
                    mapping[localTriangle.vertices[static_cast<std::size_t>(corner)]];
            }
            if (triangle.vertices[0] == triangle.vertices[1] ||
                triangle.vertices[1] == triangle.vertices[2] ||
                triangle.vertices[2] == triangle.vertices[0]) {
                continue;
            }
            result.mesh.triangles.push_back(triangle);
            if (result.mesh.triangles.size() > options.maximumSampleTriangles) {
                return fail(RegionRefitStatus::SamplingBudgetExceeded, "Adaptive sampling exceeded the triangle budget.");
            }
        }
        for (const auto& sample : faceMesh.boundarySamples) {
            result.mesh.boundarySamples.push_back({
                sample.edgeId,
                sample.parameter,
                mapping[sample.vertexIndex],
                sample.endpoint,
                false});
        }
    }

    std::sort(result.mesh.boundarySamples.begin(), result.mesh.boundarySamples.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.edgeId != rhs.edgeId) return lhs.edgeId < rhs.edgeId;
        return lhs.parameter < rhs.parameter;
    });
    result.mesh.boundarySamples.erase(
        std::unique(result.mesh.boundarySamples.begin(), result.mesh.boundarySamples.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.edgeId == rhs.edgeId && lhs.vertexIndex == rhs.vertexIndex;
        }),
        result.mesh.boundarySamples.end());

    for (auto& sample : result.mesh.boundarySamples) {
        auto& vertex = result.mesh.vertices[sample.vertexIndex];
        if (sample.endpoint) {
            ++report.boundaryEndpointCount;
        }
        vertex.boundary = true;
        vertex.hardBoundary = true;
    }

    validateInterior(document, report, result.mesh);
    report.vertexCount = result.mesh.vertices.size();
    report.triangleCount = result.mesh.triangles.size();
    report.boundarySampleCount = result.mesh.boundarySamples.size();
    report.meshFingerprint = result.mesh.deterministicFingerprint();
    if (result.mesh.empty()) {
        return fail(RegionRefitStatus::Failed, "Adaptive sampling produced an empty mesh.");
    }
    if (report.maximumInteriorChordError > report.linearDeflection * 1.5 ||
        report.maximumInteriorNormalErrorDegrees > report.angularDeflectionDegrees * 1.5 ||
        report.maximumBoundaryChordError > report.boundaryChordTolerance * 1.01) {
        report.status = RegionRefitStatus::Failed;
        std::ostringstream message;
        message << "Adaptive reference mesh exceeded a geometric limit: interior_chord="
                << report.maximumInteriorChordError << ", interior_normal_degrees="
                << report.maximumInteriorNormalErrorDegrees << ", normal_face="
                << report.maximumInteriorNormalFaceId << ", normal_triangle="
                << report.maximumInteriorNormalTriangleIndex << ", boundary_chord="
                << report.maximumBoundaryChordError << '.';
        report.message = message.str();
        return result;
    }

    report.status = RegionRefitStatus::Success;
    report.message = "Adaptive STEP reference mesh satisfies its sampling limits.";
    return result;
}

}
