#include "reduction/RestrictedBSplinePatchFitter.h"
#include "reduction/StlHybridParameterization.h"

#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/BoundaryConstrainedSurfaceRetrim.h"
#include "stl/StlRegionExtractor.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_ListIteratorOfListOfStatus.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <Eigen/Sparse>
#include <Eigen/SparseQR>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>

namespace spo {

namespace {

constexpr int degree = 3;

struct SourceSample {
    gp_Pnt point;
    gp_Vec normal;
    FaceId faceId = 0;
    bool boundary = false;
    double u = 0.0;
    double v = 0.0;
    double positionWeight = 1.0;
    double normalWeight = 1.0;
};

struct SourceTriangle {
    FaceId faceId = 0;
    TopoDS_Face face;
    gp_Pnt2d uv[3];
    double area = 0.0;
};

struct ModelQuality {
    bool passed = false;
    double maxDistance = 0.0;
    double maxNormalAngle = 0.0;
    double worstU = 0.5;
    double worstV = 0.5;
};

struct SurfaceSolveResult {
    Handle(Geom_BSplineSurface) surface;
    double maximumConstraintResidual = std::numeric_limits<double>::infinity();
    double objective = std::numeric_limits<double>::infinity();
};

struct BoundaryHoldoutQuality {
    bool valid = false;
    int sampleCount = 0;
    double maximumDistance = 0.0;
    gp_Pnt worstPoint;
};

struct SupportSurfaceExtentQuality {
    bool valid = false;
    double extentRatio = std::numeric_limits<double>::infinity();
    double maximumPoleOverflowRatio = std::numeric_limits<double>::infinity();
};

std::string invalidBRepStatuses(const TopoDS_Face& face, BRepCheck_Analyzer& analyzer) {
    std::ostringstream stream;
    const auto appendStatuses = [&](const TopAbs_ShapeEnum shapeType, const char* label) {
        TopTools_IndexedMapOfShape shapes;
        TopExp::MapShapes(face, shapeType, shapes);
        for (int index = 1; index <= shapes.Extent(); ++index) {
            const Handle(BRepCheck_Result) check = analyzer.Result(shapes(index));
            if (check.IsNull()) {
                continue;
            }
            for (BRepCheck_ListIteratorOfListOfStatus status(check->Status()); status.More(); status.Next()) {
                if (status.Value() == BRepCheck_NoError) {
                    continue;
                }
                if (stream.tellp() > 0) {
                    stream << ',';
                }
                stream << label << index << ':' << static_cast<int>(status.Value());
            }
            check->InitContextIterator();
            while (check->MoreShapeInContext()) {
                for (BRepCheck_ListIteratorOfListOfStatus status(check->StatusOnShape()); status.More(); status.Next()) {
                    if (status.Value() == BRepCheck_NoError) {
                        continue;
                    }
                    if (stream.tellp() > 0) {
                        stream << ',';
                    }
                    stream << label << index << "-context:" << static_cast<int>(status.Value());
                }
                check->NextShapeInContext();
            }
        }
    };
    appendStatuses(TopAbs_FACE, "face");
    appendStatuses(TopAbs_WIRE, "wire");
    appendStatuses(TopAbs_EDGE, "edge");
    appendStatuses(TopAbs_VERTEX, "vertex");
    return stream.str();
}

double halton(int index, int base) {
    double value = 0.0;
    double fraction = 1.0;
    while (index > 0) {
        fraction /= static_cast<double>(base);
        value += fraction * static_cast<double>(index % base);
        index /= base;
    }
    return value;
}

double faceArea(const TopoDS_Face& face) {
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(face, properties);
    return std::max(0.0, properties.Mass());
}

double edgeLength(const TopoDS_Edge& edge) {
    GProp_GProps properties;
    BRepGProp::LinearProperties(edge, properties);
    return std::max(0.0, properties.Mass());
}

bool evaluateFace(
    const TopoDS_Face& face,
    FaceId faceId,
    double u,
    double v,
    SourceSample& sample) {
    try {
        BRepAdaptor_Surface surface(face, Standard_True);
        gp_Vec du;
        gp_Vec dv;
        surface.D1(u, v, sample.point, du, dv);
        sample.normal = du.Crossed(dv);
        if (sample.normal.SquareMagnitude() <= Precision::SquareConfusion()) {
            return false;
        }
        sample.normal.Normalize();
        if (face.Orientation() == TopAbs_REVERSED) {
            sample.normal.Reverse();
        }
        sample.faceId = faceId;
        return true;
    } catch (...) {
        return false;
    }
}

MergeCandidate makeCandidate(
    const ShapeDocument& document,
    const std::vector<FaceId>& faces) {
    MergeCandidate candidate;
    candidate.candidate_type = MergeCandidateType::FeatureBoundedRefit;
    candidate.faces = faces;
    candidate.face_count = static_cast<int>(faces.size());
    const std::set<FaceId> faceSet(faces.begin(), faces.end());
    std::set<EdgeId> allEdges;
    for (const auto faceId : faces) {
        const auto& face = document.topology().face(faceId);
        candidate.total_area += faceArea(face);
        for (const auto edgeId : document.topology().edgesForFace(faceId)) {
            allEdges.insert(edgeId);
        }
    }
    for (const auto edgeId : allEdges) {
        const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
        int inside = 0;
        if (adjacency != nullptr) {
            for (const auto faceId : adjacency->faces) {
                inside += faceSet.contains(faceId) ? 1 : 0;
            }
        }
        if (inside >= 2) {
            candidate.internal_edges.push_back(edgeId);
        } else if (inside == 1) {
            candidate.boundary_edges.push_back(edgeId);
        }
    }
    candidate.internal_edge_count = static_cast<int>(candidate.internal_edges.size());
    candidate.boundary_edge_count = static_cast<int>(candidate.boundary_edges.size());
    return candidate;
}

bool collectTriangles(
    const ShapeDocument& document,
    const std::vector<FaceId>& faces,
    double deflection,
    std::vector<SourceTriangle>& triangles,
    double& totalArea) {
    totalArea = 0.0;
    for (const auto faceId : faces) {
        const auto& face = document.topology().face(faceId);
        BRepMesh_IncrementalMesh mesh(face, deflection, Standard_False, 0.2, Standard_True);
        TopLoc_Location location;
        const auto triangulation = BRep_Tool::Triangulation(face, location);
        if (triangulation.IsNull() || !triangulation->HasUVNodes()) {
            continue;
        }
        for (int triangleIndex = 1; triangleIndex <= triangulation->NbTriangles(); ++triangleIndex) {
            int a = 0;
            int b = 0;
            int c = 0;
            triangulation->Triangle(triangleIndex).Get(a, b, c);
            SourceTriangle triangle;
            triangle.faceId = faceId;
            triangle.face = face;
            triangle.uv[0] = triangulation->UVNode(a);
            triangle.uv[1] = triangulation->UVNode(b);
            triangle.uv[2] = triangulation->UVNode(c);
            SourceSample p0;
            SourceSample p1;
            SourceSample p2;
            if (!evaluateFace(face, faceId, triangle.uv[0].X(), triangle.uv[0].Y(), p0) ||
                !evaluateFace(face, faceId, triangle.uv[1].X(), triangle.uv[1].Y(), p1) ||
                !evaluateFace(face, faceId, triangle.uv[2].X(), triangle.uv[2].Y(), p2)) {
                continue;
            }
            triangle.area = 0.5 * gp_Vec(p0.point, p1.point).Crossed(gp_Vec(p0.point, p2.point)).Magnitude();
            if (triangle.area <= Precision::SquareConfusion()) {
                continue;
            }
            totalArea += triangle.area;
            triangles.push_back(triangle);
        }
    }
    return !triangles.empty() && totalArea > Precision::SquareConfusion();
}

bool isSpatiallyDistinct(
    const SourceSample& sample,
    const std::vector<SourceSample>& first,
    const std::vector<SourceSample>& second = {}) {
    constexpr double minimumSquaredDistance = 1.0e-18;
    for (const auto& existing : first) {
        if (sample.point.SquareDistance(existing.point) <= minimumSquaredDistance) {
            return false;
        }
    }
    for (const auto& existing : second) {
        if (sample.point.SquareDistance(existing.point) <= minimumSquaredDistance) {
            return false;
        }
    }
    return true;
}

std::vector<SourceSample> sampleTriangles(
    const std::vector<SourceTriangle>& triangles,
    double totalArea,
    int count,
    int offset,
    const std::vector<SourceSample>& avoidFirst = {},
    const std::vector<SourceSample>& avoidSecond = {}) {
    std::vector<double> cumulative;
    cumulative.reserve(triangles.size());
    double sum = 0.0;
    for (const auto& triangle : triangles) {
        sum += triangle.area;
        cumulative.push_back(sum);
    }

    std::vector<SourceSample> samples;
    samples.reserve(static_cast<std::size_t>(count));
    for (int attempt = 1; attempt <= count * 20 && static_cast<int>(samples.size()) < count; ++attempt) {
        const int index = attempt + offset;
        const double target = halton(index, 5) * totalArea;
        const auto iterator = std::lower_bound(cumulative.begin(), cumulative.end(), target);
        const auto triangleIndex = static_cast<std::size_t>(std::distance(cumulative.begin(), iterator));
        const auto& triangle = triangles[std::min(triangleIndex, triangles.size() - 1)];
        double r1 = halton(index, 2);
        double r2 = halton(index, 3);
        if (r1 + r2 > 1.0) {
            r1 = 1.0 - r1;
            r2 = 1.0 - r2;
        }
        const double u = triangle.uv[0].X() +
            r1 * (triangle.uv[1].X() - triangle.uv[0].X()) +
            r2 * (triangle.uv[2].X() - triangle.uv[0].X());
        const double v = triangle.uv[0].Y() +
            r1 * (triangle.uv[1].Y() - triangle.uv[0].Y()) +
            r2 * (triangle.uv[2].Y() - triangle.uv[0].Y());
        SourceSample sample;
        if (!evaluateFace(triangle.face, triangle.faceId, u, v, sample) ||
            !isSpatiallyDistinct(sample, samples, avoidFirst) ||
            !isSpatiallyDistinct(sample, avoidSecond)) {
            continue;
        }
        samples.push_back(sample);
    }
    return samples;
}

void appendBoundarySamples(
    const ShapeDocument& document,
    const std::vector<FaceId>& sourceFaces,
    const std::vector<EdgeId>& edges,
    double tolerance,
    std::vector<SourceSample>& samples) {
    const std::set<FaceId> sourceFaceSet(sourceFaces.begin(), sourceFaces.end());
    for (const auto edgeId : edges) {
        const auto& edge = document.topology().edge(edgeId);
        try {
            BRepAdaptor_Curve curve(edge);
            const double first = curve.FirstParameter();
            const double last = curve.LastParameter();
            const double length = edgeLength(edge);
            const double spacing = std::max(tolerance * 8.0, length / 16.0);
            const int segmentCount = std::clamp(
                static_cast<int>(std::ceil(length / std::max(spacing, Precision::Confusion()))),
                2,
                16);
            std::set<double> fractions {0.0, 0.5, 1.0};
            for (int index = 1; index < segmentCount; ++index) {
                fractions.insert(static_cast<double>(index) / static_cast<double>(segmentCount));
            }
            for (const double fraction : fractions) {
                const double t = first + (last - first) * fraction;
                SourceSample sample;
                const gp_Pnt edgePoint = curve.Value(t);
                sample.point = edgePoint;
                sample.boundary = true;
                const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
                if (adjacency == nullptr || adjacency->faces.empty()) {
                    continue;
                }
                const auto sourceFace = std::find_if(
                    adjacency->faces.begin(), adjacency->faces.end(),
                    [&](FaceId faceId) { return sourceFaceSet.contains(faceId); });
                if (sourceFace == adjacency->faces.end()) {
                    continue;
                }
                const auto faceId = *sourceFace;
                const auto& face = document.topology().face(faceId);
                const auto surface = BRep_Tool::Surface(face);
                GeomAPI_ProjectPointOnSurf projector(edgePoint, surface);
                if (projector.NbPoints() < 1) {
                    continue;
                }
                double u = 0.0;
                double v = 0.0;
                projector.LowerDistanceParameters(u, v);
                if (!evaluateFace(face, faceId, u, v, sample) || !isSpatiallyDistinct(sample, samples)) {
                    continue;
                }
                sample.point = edgePoint;
                sample.boundary = true;
                sample.normalWeight = 0.0;
                samples.push_back(sample);
            }
        } catch (...) {
        }
    }
}

bool projectInitialParameters(
    std::vector<SourceSample>& samples,
    const Handle(Geom_Surface)& supportSurface,
    int& orientationSign,
    double& minU,
    double& minV,
    double& maxU,
    double& maxV,
    std::string& error) {
    for (auto& sample : samples) {
        GeomAPI_ProjectPointOnSurf projector(sample.point, supportSurface);
        if (projector.NbPoints() < 1) {
            error = "无法将原始采样点投影到参数初始化支撑面。";
            return false;
        }
        projector.LowerDistanceParameters(sample.u, sample.v);
        gp_Pnt supportPoint;
        gp_Vec supportDu;
        gp_Vec supportDv;
        supportSurface->D1(sample.u, sample.v, supportPoint, supportDu, supportDv);
        const gp_Vec supportNormal = supportDu.Crossed(supportDv);
        if (supportNormal.SquareMagnitude() <= Precision::SquareConfusion()) {
            error = "参数初始化支撑面在投影点近奇异。";
            return false;
        }
        const double orientation = supportNormal.Dot(sample.normal);
        if (std::abs(orientation) <= 1.0e-10) {
            error = "参数初始化在投影点发生方向退化。";
            return false;
        }
        const int currentSign = orientation > 0.0 ? 1 : -1;
        if (orientationSign == 0) {
            orientationSign = currentSign;
        }
        minU = std::min(minU, sample.u);
        minV = std::min(minV, sample.v);
        maxU = std::max(maxU, sample.u);
        maxV = std::max(maxV, sample.v);
    }
    return true;
}

bool projectStepParameters(
    std::vector<SourceSample>& samples,
    const StlHybridParameterization& parameterization,
    std::string& error) {
    for (auto& sample : samples) {
        const auto projection = parameterization.project(sample.point);
        if (!projection.valid) {
            error = "无法将 STEP 精确采样映射到 STEP 参数图。";
            return false;
        }
        sample.u = projection.u;
        sample.v = projection.v;
    }
    return true;
}

void appendExteriorGuardSamples(
    const StlHybridParameterization& parameterization,
    const std::vector<SourceSample>& boundaryConstraints,
    std::vector<SourceSample>& samples) {
    if (boundaryConstraints.empty()) {
        return;
    }
    constexpr int gridSegments = 12;
    for (int uIndex = 0; uIndex <= gridSegments; ++uIndex) {
        const double u = static_cast<double>(uIndex) / static_cast<double>(gridSegments);
        for (int vIndex = 0; vIndex <= gridSegments; ++vIndex) {
            const double v = static_cast<double>(vIndex) / static_cast<double>(gridSegments);
            if (parameterization.contains(u, v, 1.0e-8)) {
                continue;
            }
            const auto nearest = std::min_element(
                boundaryConstraints.begin(),
                boundaryConstraints.end(),
                [&](const auto& lhs, const auto& rhs) {
                    return std::hypot(lhs.u - u, lhs.v - v) <
                        std::hypot(rhs.u - u, rhs.v - v);
                });
            SourceSample guard = *nearest;
            guard.u = u;
            guard.v = v;
            guard.boundary = false;
            guard.positionWeight = 0.2;
            guard.normalWeight = 0.0;
            samples.push_back(guard);
        }
    }
}

StlMesh scanPatchForCandidate(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RestrictedBSplinePatchFitOptions& options) {
    if (options.scanReference == nullptr || options.scanReference->empty()) {
        return {};
    }
    if (options.scanReferenceCoverage == ScanReferenceCoverage::SelectionCrop) {
        return *options.scanReference;
    }
    if (options.scanReferenceCoverage != ScanReferenceCoverage::WholeModel) {
        return {};
    }
    StlRegionExtractorOptions cropOptions;
    cropOptions.mode = StlCropMode::ConservativeBoundaryBand;
    cropOptions.minMargin = std::max(0.1, options.tolerance3d * 4.0);
    cropOptions.boundaryLoopCoverageTolerance = std::max(0.1, options.tolerance3d * 4.0);
    const auto crop = StlRegionExtractor().extract(
        document,
        candidate,
        *options.scanReference,
        cropOptions);
    return crop.success ? crop.localMesh : StlMesh {};
}

std::vector<SourceSample> sampleScanReference(
    const StlMesh& mesh,
    int count,
    const StlHybridParameterization* parameterization,
    const Handle(Geom_Surface)& supportSurface,
    double minU,
    double minV,
    double maxU,
    double maxV,
    const RestrictedBSplinePatchFitOptions& options,
    double& maximumStepToScanDistance) {
    struct WeightedTriangle {
        const StlTriangle* triangle = nullptr;
        double cumulativeArea = 0.0;
    };
    std::vector<WeightedTriangle> triangles;
    triangles.reserve(mesh.triangleCount());
    double totalArea = 0.0;
    for (const auto& triangle : mesh.triangles()) {
        const gp_Pnt first(triangle.v0.x, triangle.v0.y, triangle.v0.z);
        const gp_Pnt second(triangle.v1.x, triangle.v1.y, triangle.v1.z);
        const gp_Pnt third(triangle.v2.x, triangle.v2.y, triangle.v2.z);
        const double area = 0.5 * gp_Vec(first, second).Crossed(gp_Vec(first, third)).Magnitude();
        if (area <= Precision::SquareConfusion()) {
            continue;
        }
        totalArea += area;
        triangles.push_back({&triangle, totalArea});
    }
    if (triangles.empty() || totalArea <= Precision::SquareConfusion()) {
        return {};
    }

    std::vector<SourceSample> samples;
    samples.reserve(static_cast<std::size_t>(count));
    for (int attempt = 1; attempt <= count * 20 && static_cast<int>(samples.size()) < count; ++attempt) {
        const int sequence = attempt + 30011;
        const double target = halton(sequence, 5) * totalArea;
        const auto iterator = std::lower_bound(
            triangles.begin(), triangles.end(), target,
            [](const WeightedTriangle& value, double area) {
                return value.cumulativeArea < area;
            });
        const auto* triangle = (iterator == triangles.end() ? triangles.back() : *iterator).triangle;
        double firstWeight = halton(sequence, 2);
        double secondWeight = halton(sequence, 3);
        if (firstWeight + secondWeight > 1.0) {
            firstWeight = 1.0 - firstWeight;
            secondWeight = 1.0 - secondWeight;
        }
        const double thirdWeight = 1.0 - firstWeight - secondWeight;
        SourceSample sample;
        sample.point = gp_Pnt(
            firstWeight * triangle->v0.x + secondWeight * triangle->v1.x + thirdWeight * triangle->v2.x,
            firstWeight * triangle->v0.y + secondWeight * triangle->v1.y + thirdWeight * triangle->v2.y,
            firstWeight * triangle->v0.z + secondWeight * triangle->v1.z + thirdWeight * triangle->v2.z);
        double distance = std::numeric_limits<double>::infinity();
        if (parameterization != nullptr) {
            const auto projection = parameterization->project(sample.point);
            if (!projection.valid) {
                continue;
            }
            sample.u = projection.u;
            sample.v = projection.v;
            distance = projection.distance;
        } else if (!supportSurface.IsNull()) {
            GeomAPI_ProjectPointOnSurf projector(sample.point, supportSurface);
            if (projector.NbPoints() < 1) {
                continue;
            }
            double u = 0.0;
            double v = 0.0;
            projector.LowerDistanceParameters(u, v);
            sample.u = (u - minU) / (maxU - minU);
            sample.v = (v - minV) / (maxV - minV);
            distance = projector.LowerDistance();
            if (sample.u < 0.0 || sample.u > 1.0 || sample.v < 0.0 || sample.v > 1.0) {
                continue;
            }
        } else {
            continue;
        }
        if (distance > options.maximumScanReferenceDistance ||
            !isSpatiallyDistinct(sample, samples)) {
            continue;
        }
        sample.positionWeight = options.scanReferenceWeight;
        sample.normalWeight = 0.0;
        samples.push_back(sample);
        maximumStepToScanDistance = std::max(maximumStepToScanDistance, distance);
    }
    return samples;
}

bool parameterizePoint(
    const gp_Pnt& point,
    const StlHybridParameterization* parameterization,
    const Handle(Geom_Surface)& supportSurface,
    double minU,
    double minV,
    double maxU,
    double maxV,
    SourceSample& sample) {
    sample.point = point;
    sample.boundary = true;
    sample.normalWeight = 0.0;
    if (parameterization != nullptr) {
        const auto projection = parameterization->project(point);
        if (!projection.valid) {
            return false;
        }
        sample.u = projection.u;
        sample.v = projection.v;
        return parameterization->contains(sample.u, sample.v);
    }
    if (supportSurface.IsNull()) {
        return false;
    }
    GeomAPI_ProjectPointOnSurf projector(point, supportSurface);
    if (projector.NbPoints() < 1) {
        return false;
    }
    double u = 0.0;
    double v = 0.0;
    projector.LowerDistanceParameters(u, v);
    sample.u = (u - minU) / (maxU - minU);
    sample.v = (v - minV) / (maxV - minV);
    return sample.u >= 0.0 && sample.u <= 1.0 && sample.v >= 0.0 && sample.v <= 1.0;
}

void normalizeParameters(
    std::vector<SourceSample>& samples,
    double minU,
    double minV,
    double maxU,
    double maxV) {
    for (auto& sample : samples) {
        sample.u = std::clamp((sample.u - minU) / (maxU - minU), 0.0, 1.0);
        sample.v = std::clamp((sample.v - minV) / (maxV - minV), 0.0, 1.0);
    }
}

bool parametersOverlap(
    const std::vector<SourceSample>& training,
    const std::vector<SourceSample>& selection,
    const std::vector<SourceSample>& holdout,
    double tolerance) {
    std::vector<const SourceSample*> samples;
    samples.reserve(training.size() + selection.size() + holdout.size());
    for (const auto& sample : training) {
        samples.push_back(&sample);
    }
    for (const auto& sample : selection) {
        samples.push_back(&sample);
    }
    for (const auto& sample : holdout) {
        samples.push_back(&sample);
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        for (std::size_t j = i + 1; j < samples.size(); ++j) {
            const double du = samples[i]->u - samples[j]->u;
            const double dv = samples[i]->v - samples[j]->v;
            if (du * du + dv * dv < 1.0e-16 &&
                samples[i]->point.Distance(samples[j]->point) > tolerance) {
                return true;
            }
        }
    }
    return false;
}

std::vector<double> makeExpandedKnots(const std::vector<double>& innerKnots) {
    std::vector<double> knots(static_cast<std::size_t>(degree + 1), 0.0);
    knots.insert(knots.end(), innerKnots.begin(), innerKnots.end());
    knots.insert(knots.end(), static_cast<std::size_t>(degree + 1), 1.0);
    return knots;
}

std::vector<double> basisValues(int controlCount, int basisDegree, double parameter, const std::vector<double>& knots) {
    const double u = std::clamp(parameter, 0.0, 1.0);
    std::vector<double> values(static_cast<std::size_t>(controlCount + basisDegree), 0.0);
    for (int index = 0; index < controlCount; ++index) {
        const bool inside = knots[static_cast<std::size_t>(index)] <= u &&
            u < knots[static_cast<std::size_t>(index + 1)];
        values[static_cast<std::size_t>(index)] = inside ||
            (u == 1.0 && index == controlCount - 1) ? 1.0 : 0.0;
    }
    for (int currentDegree = 1; currentDegree <= basisDegree; ++currentDegree) {
        std::vector<double> next(values.size(), 0.0);
        for (int index = 0; index < controlCount; ++index) {
            const double leftDenominator = knots[static_cast<std::size_t>(index + currentDegree)] -
                knots[static_cast<std::size_t>(index)];
            const double rightDenominator = knots[static_cast<std::size_t>(index + currentDegree + 1)] -
                knots[static_cast<std::size_t>(index + 1)];
            if (leftDenominator > Precision::PConfusion()) {
                next[static_cast<std::size_t>(index)] +=
                    (u - knots[static_cast<std::size_t>(index)]) / leftDenominator *
                    values[static_cast<std::size_t>(index)];
            }
            if (rightDenominator > Precision::PConfusion()) {
                next[static_cast<std::size_t>(index)] +=
                    (knots[static_cast<std::size_t>(index + currentDegree + 1)] - u) / rightDenominator *
                    values[static_cast<std::size_t>(index + 1)];
            }
        }
        values.swap(next);
    }
    values.resize(static_cast<std::size_t>(controlCount));
    return values;
}

std::vector<double> basisDerivatives(int controlCount, double parameter, const std::vector<double>& knots) {
    const auto lower = basisValues(controlCount + 1, degree - 1, parameter, knots);
    std::vector<double> derivatives(static_cast<std::size_t>(controlCount), 0.0);
    for (int index = 0; index < controlCount; ++index) {
        const double leftDenominator = knots[static_cast<std::size_t>(index + degree)] -
            knots[static_cast<std::size_t>(index)];
        const double rightDenominator = knots[static_cast<std::size_t>(index + degree + 1)] -
            knots[static_cast<std::size_t>(index + 1)];
        if (leftDenominator > Precision::PConfusion()) {
            derivatives[static_cast<std::size_t>(index)] +=
                degree * lower[static_cast<std::size_t>(index)] / leftDenominator;
        }
        if (rightDenominator > Precision::PConfusion()) {
            derivatives[static_cast<std::size_t>(index)] -=
                degree * lower[static_cast<std::size_t>(index + 1)] / rightDenominator;
        }
    }
    return derivatives;
}

SurfaceSolveResult solveSurface(
    const std::vector<SourceSample>& samples,
    const std::vector<SourceSample>& boundaryConstraints,
    const std::vector<double>& uInnerKnots,
    const std::vector<double>& vInnerKnots,
    const RestrictedBSplinePatchFitOptions& options) {
    SurfaceSolveResult result;
    if (samples.empty() || boundaryConstraints.empty()) {
        return result;
    }
    const int uControlCount = degree + 1 + static_cast<int>(uInnerKnots.size());
    const int vControlCount = degree + 1 + static_cast<int>(vInnerKnots.size());
    const int poleCount = uControlCount * vControlCount;
    const int variableCount = poleCount * 3;
    const auto uKnots = makeExpandedKnots(uInnerKnots);
    const auto vKnots = makeExpandedKnots(vInnerKnots);
    std::vector<Eigen::Triplet<double>> coefficients;
    std::vector<double> rightHandSide;
    const double positionNormalization = 1.0 /
        (std::max(options.tolerance3d, 1.0e-9) * std::sqrt(static_cast<double>(samples.size())));
    const double normalNormalization = 0.1 / std::sqrt(static_cast<double>(samples.size()));

    auto addRow = [&](double rhs) {
        rightHandSide.push_back(rhs);
        return static_cast<int>(rightHandSide.size() - 1);
    };
    auto variable = [&](int coordinate, int uIndex, int vIndex) {
        return coordinate * poleCount + uIndex * vControlCount + vIndex;
    };

    for (const auto& sample : samples) {
        const auto uBasis = basisValues(uControlCount, degree, sample.u, uKnots);
        const auto vBasis = basisValues(vControlCount, degree, sample.v, vKnots);
        const auto uDerivative = basisDerivatives(uControlCount, sample.u, uKnots);
        const auto vDerivative = basisDerivatives(vControlCount, sample.v, vKnots);
        const double positionWeight = positionNormalization *
            (sample.boundary ? 2.0 : 1.0) * sample.positionWeight;
        const double coordinates[3] = {sample.point.X(), sample.point.Y(), sample.point.Z()};
        for (int coordinate = 0; coordinate < 3; ++coordinate) {
            const int row = addRow(positionWeight * coordinates[coordinate]);
            for (int uIndex = 0; uIndex < uControlCount; ++uIndex) {
                for (int vIndex = 0; vIndex < vControlCount; ++vIndex) {
                    const double value = positionWeight * uBasis[static_cast<std::size_t>(uIndex)] *
                        vBasis[static_cast<std::size_t>(vIndex)];
                    if (std::abs(value) > 1.0e-14) {
                        coefficients.emplace_back(row, variable(coordinate, uIndex, vIndex), value);
                    }
                }
            }
        }

        if (sample.normalWeight <= 0.0) {
            continue;
        }
        const double normalCoordinates[3] = {sample.normal.X(), sample.normal.Y(), sample.normal.Z()};
        const int uRow = addRow(0.0);
        const int vRow = addRow(0.0);
        for (int coordinate = 0; coordinate < 3; ++coordinate) {
            for (int uIndex = 0; uIndex < uControlCount; ++uIndex) {
                for (int vIndex = 0; vIndex < vControlCount; ++vIndex) {
                    const double uValue = normalNormalization * sample.normalWeight * normalCoordinates[coordinate] *
                        uDerivative[static_cast<std::size_t>(uIndex)] * vBasis[static_cast<std::size_t>(vIndex)];
                    const double vValue = normalNormalization * sample.normalWeight * normalCoordinates[coordinate] *
                        uBasis[static_cast<std::size_t>(uIndex)] * vDerivative[static_cast<std::size_t>(vIndex)];
                    if (std::abs(uValue) > 1.0e-14) {
                        coefficients.emplace_back(uRow, variable(coordinate, uIndex, vIndex), uValue);
                    }
                    if (std::abs(vValue) > 1.0e-14) {
                        coefficients.emplace_back(vRow, variable(coordinate, uIndex, vIndex), vValue);
                    }
                }
            }
        }
    }

    const double fairnessWeight = options.fairnessWeight / std::sqrt(static_cast<double>(poleCount));
    for (int coordinate = 0; coordinate < 3; ++coordinate) {
        for (int uIndex = 1; uIndex + 1 < uControlCount; ++uIndex) {
            for (int vIndex = 0; vIndex < vControlCount; ++vIndex) {
                const int row = addRow(0.0);
                coefficients.emplace_back(row, variable(coordinate, uIndex - 1, vIndex), fairnessWeight);
                coefficients.emplace_back(row, variable(coordinate, uIndex, vIndex), -2.0 * fairnessWeight);
                coefficients.emplace_back(row, variable(coordinate, uIndex + 1, vIndex), fairnessWeight);
            }
        }
        for (int uIndex = 0; uIndex < uControlCount; ++uIndex) {
            for (int vIndex = 1; vIndex + 1 < vControlCount; ++vIndex) {
                const int row = addRow(0.0);
                coefficients.emplace_back(row, variable(coordinate, uIndex, vIndex - 1), fairnessWeight);
                coefficients.emplace_back(row, variable(coordinate, uIndex, vIndex), -2.0 * fairnessWeight);
                coefficients.emplace_back(row, variable(coordinate, uIndex, vIndex + 1), fairnessWeight);
            }
        }
    }

    Eigen::SparseMatrix<double> matrix(static_cast<int>(rightHandSide.size()), variableCount);
    matrix.setFromTriplets(coefficients.begin(), coefficients.end());
    Eigen::VectorXd softRhs(static_cast<int>(rightHandSide.size()));
    for (int row = 0; row < softRhs.size(); ++row) {
        softRhs[row] = rightHandSide[static_cast<std::size_t>(row)];
    }
    Eigen::SparseMatrix<double> normalMatrix = matrix.transpose() * matrix;
    const Eigen::VectorXd normalRhs = matrix.transpose() * softRhs;

    const int constraintRowCount = static_cast<int>(boundaryConstraints.size()) * 3;
    const int systemSize = variableCount + constraintRowCount;
    std::vector<Eigen::Triplet<double>> systemCoefficients;
    systemCoefficients.reserve(
        static_cast<std::size_t>(normalMatrix.nonZeros()) +
        boundaryConstraints.size() * static_cast<std::size_t>(poleCount) * 6 +
        static_cast<std::size_t>(variableCount));
    for (int outer = 0; outer < normalMatrix.outerSize(); ++outer) {
        for (Eigen::SparseMatrix<double>::InnerIterator value(normalMatrix, outer); value; ++value) {
            systemCoefficients.emplace_back(
                static_cast<int>(value.row()),
                static_cast<int>(value.col()),
                value.value());
        }
    }
    for (int variableIndex = 0; variableIndex < variableCount; ++variableIndex) {
        systemCoefficients.emplace_back(variableIndex, variableIndex, 1.0e-12);
    }

    Eigen::VectorXd systemRhs = Eigen::VectorXd::Zero(systemSize);
    systemRhs.head(variableCount) = normalRhs;
    for (std::size_t sampleIndex = 0; sampleIndex < boundaryConstraints.size(); ++sampleIndex) {
        const auto& constraint = boundaryConstraints[sampleIndex];
        const auto uBasis = basisValues(uControlCount, degree, constraint.u, uKnots);
        const auto vBasis = basisValues(vControlCount, degree, constraint.v, vKnots);
        const double coordinates[3] = {
            constraint.point.X(), constraint.point.Y(), constraint.point.Z()};
        for (int coordinate = 0; coordinate < 3; ++coordinate) {
            const int constraintRow = variableCount + static_cast<int>(sampleIndex) * 3 + coordinate;
            systemRhs[constraintRow] = coordinates[coordinate];
            for (int uIndex = 0; uIndex < uControlCount; ++uIndex) {
                for (int vIndex = 0; vIndex < vControlCount; ++vIndex) {
                    const double value = uBasis[static_cast<std::size_t>(uIndex)] *
                        vBasis[static_cast<std::size_t>(vIndex)];
                    if (std::abs(value) <= 1.0e-14) {
                        continue;
                    }
                    const int controlVariable = variable(coordinate, uIndex, vIndex);
                    systemCoefficients.emplace_back(constraintRow, controlVariable, value);
                    systemCoefficients.emplace_back(controlVariable, constraintRow, value);
                }
            }
        }
    }

    Eigen::SparseMatrix<double> system(systemSize, systemSize);
    system.setFromTriplets(systemCoefficients.begin(), systemCoefficients.end());
    Eigen::SparseQR<Eigen::SparseMatrix<double>, Eigen::COLAMDOrdering<int>> solver;
    solver.compute(system);
    if (solver.info() != Eigen::Success) {
        return result;
    }
    const Eigen::VectorXd solution = solver.solve(systemRhs);
    if (solver.info() != Eigen::Success || !solution.allFinite()) {
        return result;
    }

    const Eigen::VectorXd controlSolution = solution.head(variableCount);
    const Eigen::VectorXd residual = matrix * controlSolution - softRhs;
    result.objective = residual.squaredNorm() /
        static_cast<double>(std::max<Eigen::Index>(1, residual.size()));

    TColgp_Array2OfPnt poles(1, uControlCount, 1, vControlCount);
    for (int uIndex = 0; uIndex < uControlCount; ++uIndex) {
        for (int vIndex = 0; vIndex < vControlCount; ++vIndex) {
            const int poleIndex = uIndex * vControlCount + vIndex;
            poles.SetValue(
                uIndex + 1,
                vIndex + 1,
                gp_Pnt(
                    controlSolution[poleIndex],
                    controlSolution[poleCount + poleIndex],
                    controlSolution[2 * poleCount + poleIndex]));
        }
    }

    auto makeUniqueKnots = [](const std::vector<double>& inner, TColStd_Array1OfReal& knots, TColStd_Array1OfInteger& multiplicities) {
        knots.SetValue(1, 0.0);
        multiplicities.SetValue(1, degree + 1);
        for (int index = 0; index < static_cast<int>(inner.size()); ++index) {
            knots.SetValue(index + 2, inner[static_cast<std::size_t>(index)]);
            multiplicities.SetValue(index + 2, 1);
        }
        knots.SetValue(static_cast<int>(inner.size()) + 2, 1.0);
        multiplicities.SetValue(static_cast<int>(inner.size()) + 2, degree + 1);
    };
    TColStd_Array1OfReal uUnique(1, static_cast<int>(uInnerKnots.size()) + 2);
    TColStd_Array1OfReal vUnique(1, static_cast<int>(vInnerKnots.size()) + 2);
    TColStd_Array1OfInteger uMultiplicities(1, static_cast<int>(uInnerKnots.size()) + 2);
    TColStd_Array1OfInteger vMultiplicities(1, static_cast<int>(vInnerKnots.size()) + 2);
    makeUniqueKnots(uInnerKnots, uUnique, uMultiplicities);
    makeUniqueKnots(vInnerKnots, vUnique, vMultiplicities);
    result.surface = new Geom_BSplineSurface(
        poles,
        uUnique,
        vUnique,
        uMultiplicities,
        vMultiplicities,
        degree,
        degree,
        Standard_False,
        Standard_False);
    result.maximumConstraintResidual = 0.0;
    for (const auto& constraint : boundaryConstraints) {
        result.maximumConstraintResidual = std::max(
            result.maximumConstraintResidual,
            result.surface->Value(constraint.u, constraint.v).Distance(constraint.point));
    }
    const double constraintTolerance = std::max(1.0e-9, options.tolerance3d * 1.0e-6);
    if (result.maximumConstraintResidual > constraintTolerance) {
        result.surface.Nullify();
    }
    return result;
}

bool reprojectTrainingSamples(
    const std::vector<SourceSample>& samples,
    const Handle(Geom_BSplineSurface)& surface,
    std::vector<std::array<double, 2>>& projectedParameters,
    double& maximumShift) {
    maximumShift = 0.0;
    projectedParameters.clear();
    projectedParameters.reserve(samples.size());
    for (const auto& sample : samples) {
        GeomAPI_ProjectPointOnSurf projector(sample.point, surface);
        if (projector.NbPoints() < 1) {
            return false;
        }
        double u = 0.0;
        double v = 0.0;
        projector.LowerDistanceParameters(u, v);
        maximumShift = std::max(maximumShift, std::hypot(u - sample.u, v - sample.v));
        projectedParameters.push_back({u, v});
    }
    return true;
}

bool applyProjectedParameters(
    std::vector<SourceSample>& samples,
    const std::vector<std::array<double, 2>>& projectedParameters,
    double step,
    const StlHybridParameterization* parameterization) {
    if (samples.size() != projectedParameters.size()) {
        return false;
    }
    for (std::size_t index = 0; index < samples.size(); ++index) {
        auto& sample = samples[index];
        const double u = sample.u + step * (projectedParameters[index][0] - sample.u);
        const double v = sample.v + step * (projectedParameters[index][1] - sample.v);
        if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0 ||
            (parameterization != nullptr && !parameterization->contains(u, v))) {
            return false;
        }
        sample.u = u;
        sample.v = v;
    }
    return true;
}

ModelQuality evaluateModel(
    const std::vector<SourceSample>& samples,
    const Handle(Geom_BSplineSurface)& surface,
    const RestrictedBSplinePatchFitOptions& options) {
    ModelQuality quality;
    for (const auto& sample : samples) {
        GeomAPI_ProjectPointOnSurf projector(sample.point, surface);
        if (projector.NbPoints() < 1) {
            quality.maxDistance = std::numeric_limits<double>::infinity();
            quality.maxNormalAngle = 180.0;
            return quality;
        }
        double u = 0.0;
        double v = 0.0;
        projector.LowerDistanceParameters(u, v);
        const double distance = projector.LowerDistance();
        gp_Pnt point;
        gp_Vec du;
        gp_Vec dv;
        surface->D1(u, v, point, du, dv);
        gp_Vec normal = du.Crossed(dv);
        double angle = 180.0;
        if (normal.SquareMagnitude() > Precision::SquareConfusion()) {
            normal.Normalize();
            angle = std::acos(std::clamp(std::abs(normal.Dot(sample.normal)), 0.0, 1.0)) *
                180.0 / 3.14159265358979323846;
        }
        if (distance >= quality.maxDistance) {
            quality.maxDistance = distance;
            quality.worstU = u;
            quality.worstV = v;
        }
        quality.maxNormalAngle = std::max(quality.maxNormalAngle, angle);
    }
    quality.passed = quality.maxDistance <= options.tolerance3d &&
        quality.maxNormalAngle <= options.normalToleranceDegrees;
    return quality;
}

bool evaluateJacobian(
    const Handle(Geom_BSplineSurface)& surface,
    const StlHybridParameterization* parameterization,
    double minimumNormalized,
    double& observedMinimum) {
    observedMinimum = std::numeric_limits<double>::infinity();
    bool evaluated = false;
    for (int uIndex = 0; uIndex < 10; ++uIndex) {
        for (int vIndex = 0; vIndex < 10; ++vIndex) {
            const double u = (static_cast<double>(uIndex) + 0.5) / 10.0;
            const double v = (static_cast<double>(vIndex) + 0.5) / 10.0;
            if (parameterization != nullptr && !parameterization->contains(u, v)) {
                continue;
            }
            evaluated = true;
            gp_Pnt point;
            gp_Vec du;
            gp_Vec dv;
            surface->D1(u, v, point, du, dv);
            const gp_Vec cross = du.Crossed(dv);
            const double denominator = du.Magnitude() * dv.Magnitude();
            if (denominator <= Precision::SquareConfusion()) {
                return false;
            }
            const double normalized = cross.Magnitude() / denominator;
            observedMinimum = std::min(observedMinimum, normalized);
            if (normalized < minimumNormalized) {
                return false;
            }
        }
    }
    return evaluated;
}

void insertWorstIntervalKnot(std::vector<double>& innerKnots, double parameter) {
    std::vector<double> all {0.0};
    all.insert(all.end(), innerKnots.begin(), innerKnots.end());
    all.push_back(1.0);
    const auto upper = std::upper_bound(all.begin(), all.end(), std::clamp(parameter, 0.0, 1.0));
    const std::size_t upperIndex = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::distance(all.begin(), upper)), 1, all.size() - 1);
    const double knot = 0.5 * (all[upperIndex - 1] + all[upperIndex]);
    innerKnots.insert(std::lower_bound(innerKnots.begin(), innerKnots.end(), knot), knot);
}

void insertLargestIntervalKnot(std::vector<double>& innerKnots) {
    std::vector<double> all {0.0};
    all.insert(all.end(), innerKnots.begin(), innerKnots.end());
    all.push_back(1.0);
    double largestWidth = -1.0;
    double knot = 0.5;
    for (std::size_t index = 1; index < all.size(); ++index) {
        const double width = all[index] - all[index - 1];
        if (width > largestWidth) {
            largestWidth = width;
            knot = 0.5 * (all[index] + all[index - 1]);
        }
    }
    innerKnots.insert(std::lower_bound(innerKnots.begin(), innerKnots.end(), knot), knot);
}

double containingIntervalWidth(const std::vector<double>& innerKnots, double parameter) {
    std::vector<double> all {0.0};
    all.insert(all.end(), innerKnots.begin(), innerKnots.end());
    all.push_back(1.0);
    const auto upper = std::upper_bound(all.begin(), all.end(), std::clamp(parameter, 0.0, 1.0));
    const std::size_t upperIndex = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::distance(all.begin(), upper)), 1, all.size() - 1);
    return all[upperIndex] - all[upperIndex - 1];
}

std::pair<double, double> estimateShapeVariation(const std::vector<SourceTriangle>& triangles) {
    double maximumNormalAngle = 0.0;
    double maximumCurvatureIndicator = 0.0;
    gp_Vec referenceNormal;
    const std::size_t stride = std::max<std::size_t>(1, triangles.size() / 64);
    for (std::size_t index = 0; index < triangles.size(); index += stride) {
        const auto& triangle = triangles[index];
        const double u = (triangle.uv[0].X() + triangle.uv[1].X() + triangle.uv[2].X()) / 3.0;
        const double v = (triangle.uv[0].Y() + triangle.uv[1].Y() + triangle.uv[2].Y()) / 3.0;
        try {
            BRepAdaptor_Surface surface(triangle.face, Standard_True);
            gp_Pnt point;
            gp_Vec du;
            gp_Vec dv;
            gp_Vec duu;
            gp_Vec dvv;
            gp_Vec duv;
            surface.D2(u, v, point, du, dv, duu, dvv, duv);
            gp_Vec normal = du.Crossed(dv);
            if (normal.SquareMagnitude() <= Precision::SquareConfusion()) {
                continue;
            }
            normal.Normalize();
            if (referenceNormal.SquareMagnitude() <= Precision::SquareConfusion()) {
                referenceNormal = normal;
            } else {
                maximumNormalAngle = std::max(
                    maximumNormalAngle,
                    std::acos(std::clamp(std::abs(referenceNormal.Dot(normal)), 0.0, 1.0)) *
                        180.0 / 3.14159265358979323846);
            }
            const double firstDerivativeScale = du.SquareMagnitude() + dv.SquareMagnitude();
            const double secondDerivativeScale = duu.Magnitude() + dvv.Magnitude() + 2.0 * duv.Magnitude();
            maximumCurvatureIndicator = std::max(
                maximumCurvatureIndicator,
                secondDerivativeScale / std::max(firstDerivativeScale, 1.0e-12));
        } catch (...) {
        }
    }
    return {maximumNormalAngle, maximumCurvatureIndicator};
}

BoundaryHoldoutQuality evaluateBoundaryHoldout(
    const ShapeDocument& document,
    const std::vector<EdgeId>& edges,
    const Handle(Geom_BSplineSurface)& surface) {
    BoundaryHoldoutQuality quality;
    constexpr int samplesPerEdge = 97;
    constexpr double phase = 0.3819660112501051;
    for (const auto edgeId : edges) {
        try {
            BRepAdaptor_Curve curve(document.topology().edge(edgeId));
            const double first = curve.FirstParameter();
            const double last = curve.LastParameter();
            for (int index = 0; index < samplesPerEdge; ++index) {
                const double fraction = (static_cast<double>(index) + phase) /
                    static_cast<double>(samplesPerEdge);
                const gp_Pnt point = curve.Value(first + (last - first) * fraction);
                GeomAPI_ProjectPointOnSurf projector(point, surface);
                ++quality.sampleCount;
                if (projector.NbPoints() < 1) {
                    return quality;
                }
                if (projector.LowerDistance() >= quality.maximumDistance) {
                    quality.maximumDistance = projector.LowerDistance();
                    quality.worstPoint = point;
                }
            }
        } catch (...) {
            return quality;
        }
    }
    quality.valid = quality.sampleCount > 0;
    return quality;
}

SupportSurfaceExtentQuality evaluateSupportSurfaceExtent(
    const std::vector<SourceSample>& training,
    const std::vector<SourceSample>& selection,
    const std::vector<SourceSample>& holdout,
    const Handle(Geom_BSplineSurface)& surface) {
    SupportSurfaceExtentQuality quality;
    if (surface.IsNull()) {
        return quality;
    }
    double sourceMinimum[3] = {
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    double sourceMaximum[3] = {
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    auto appendSource = [&](const std::vector<SourceSample>& samples) {
        for (const auto& sample : samples) {
            const double coordinates[3] = {
                sample.point.X(), sample.point.Y(), sample.point.Z()};
            for (int coordinate = 0; coordinate < 3; ++coordinate) {
                sourceMinimum[coordinate] = std::min(sourceMinimum[coordinate], coordinates[coordinate]);
                sourceMaximum[coordinate] = std::max(sourceMaximum[coordinate], coordinates[coordinate]);
            }
        }
    };
    appendSource(training);
    appendSource(selection);
    appendSource(holdout);
    const double sourceDiagonal = std::hypot(
        std::hypot(
            sourceMaximum[0] - sourceMinimum[0],
            sourceMaximum[1] - sourceMinimum[1]),
        sourceMaximum[2] - sourceMinimum[2]);
    if (!std::isfinite(sourceDiagonal) || sourceDiagonal <= Precision::Confusion()) {
        return quality;
    }

    double poleMinimum[3] = {
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    double poleMaximum[3] = {
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    double maximumOverflow = 0.0;
    for (int u = 1; u <= surface->NbUPoles(); ++u) {
        for (int v = 1; v <= surface->NbVPoles(); ++v) {
            const auto pole = surface->Pole(u, v);
            const double coordinates[3] = {pole.X(), pole.Y(), pole.Z()};
            double squaredOverflow = 0.0;
            for (int coordinate = 0; coordinate < 3; ++coordinate) {
                poleMinimum[coordinate] = std::min(poleMinimum[coordinate], coordinates[coordinate]);
                poleMaximum[coordinate] = std::max(poleMaximum[coordinate], coordinates[coordinate]);
                const double overflow = coordinates[coordinate] < sourceMinimum[coordinate]
                    ? sourceMinimum[coordinate] - coordinates[coordinate]
                    : std::max(0.0, coordinates[coordinate] - sourceMaximum[coordinate]);
                squaredOverflow += overflow * overflow;
            }
            maximumOverflow = std::max(maximumOverflow, std::sqrt(squaredOverflow));
        }
    }
    const double poleDiagonal = std::hypot(
        std::hypot(
            poleMaximum[0] - poleMinimum[0],
            poleMaximum[1] - poleMinimum[1]),
        poleMaximum[2] - poleMinimum[2]);
    quality.valid = std::isfinite(poleDiagonal) && std::isfinite(maximumOverflow);
    quality.extentRatio = poleDiagonal / sourceDiagonal;
    quality.maximumPoleOverflowRatio = maximumOverflow / sourceDiagonal;
    return quality;
}

} // namespace

const char* toString(RestrictedBSplinePatchFitFailureStage stage) {
    switch (stage) {
    case RestrictedBSplinePatchFitFailureStage::None:
        return "None";
    case RestrictedBSplinePatchFitFailureStage::Parameterization:
        return "Parameterization";
    case RestrictedBSplinePatchFitFailureStage::BoundaryConstraints:
        return "BoundaryConstraints";
    case RestrictedBSplinePatchFitFailureStage::UvIteration:
        return "UvIteration";
    case RestrictedBSplinePatchFitFailureStage::ModelSelection:
        return "ModelSelection";
    case RestrictedBSplinePatchFitFailureStage::Holdout:
        return "Holdout";
    case RestrictedBSplinePatchFitFailureStage::Jacobian:
        return "Jacobian";
    case RestrictedBSplinePatchFitFailureStage::Retrim:
        return "Retrim";
    case RestrictedBSplinePatchFitFailureStage::BoundaryHoldout:
        return "BoundaryHoldout";
    case RestrictedBSplinePatchFitFailureStage::BrepCheck:
        return "BrepCheck";
    }
    return "None";
}

RestrictedBSplinePatchFitResult RestrictedBSplinePatchFitter::fit(
    const ShapeDocument& document,
    const std::vector<FaceId>& sourceFaces,
    const RestrictedBSplinePatchFitOptions& options) const {
    RestrictedBSplinePatchFitResult result;
    result.report.failureStage = RestrictedBSplinePatchFitFailureStage::Parameterization;
    result.report.scanReferenceCoverage = options.scanReferenceCoverage;
    if (!document.hasShape() || sourceFaces.size() < 2) {
        result.report.message = "受限曲面拟合至少需要两个原始面。";
        return result;
    }
    if (options.maximumBoundaryConstraintRefinements < 0 ||
        options.maximumUvIterations < 1 ||
        options.maximumUvBacktrackingSteps < 1 ||
        options.uvConvergenceTolerance <= 0.0 ||
        options.scanReferenceWeight < 0.0 ||
        options.maximumScanReferenceDistance <= 0.0 ||
        ((options.scanReference == nullptr) !=
         (options.scanReferenceCoverage == ScanReferenceCoverage::None))) {
        result.report.message = "受限曲面拟合选项或扫描参考职责不一致。";
        return result;
    }
    std::vector<FaceId> faces = sourceFaces;
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    if (faces.size() < 2 || std::any_of(faces.begin(), faces.end(), [&](FaceId id) {
            return id >= document.topology().faceCount();
        })) {
        result.report.message = "受限曲面拟合引用了无效面。";
        return result;
    }

    try {
        const auto candidate = makeCandidate(document, faces);
        const auto boundary = RegionBoundaryAnalyzer().analyze(document, candidate);
        if (!boundary.valid) {
            result.report.message = "分组边界不支持单张曲面参数化：" + boundary.message;
            return result;
        }

        FaceId supportFaceId = faces.front();
        double supportArea = -1.0;
        double totalFaceArea = 0.0;
        for (const auto faceId : faces) {
            const double area = faceArea(document.topology().face(faceId));
            totalFaceArea += area;
            if (area > supportArea) {
                supportArea = area;
                supportFaceId = faceId;
            }
        }
        double totalBoundaryLength = 0.0;
        for (const auto edgeId : candidate.boundary_edges) {
            totalBoundaryLength += edgeLength(document.topology().edge(edgeId));
        }
        const double tolerance = std::max(options.tolerance3d, 1.0e-6);
        const double boundaryTolerance = std::min(tolerance, 2.5e-3);
        const double retrimTolerance = boundaryTolerance;
        std::vector<SourceTriangle> triangles;
        double triangleArea = 0.0;
        if (!collectTriangles(document, faces, std::max(tolerance * 2.0, 0.01), triangles, triangleArea)) {
            result.report.message = "无法从原始面的局部 UV 三角细分生成精确采样。";
            return result;
        }
        const auto [normalVariationDegrees, curvatureIndicator] = estimateShapeVariation(triangles);
        const int totalSampleBudget = std::clamp(
            static_cast<int>(std::ceil(
                std::sqrt(std::max(totalFaceArea, tolerance * tolerance)) / tolerance * 0.35 +
                totalBoundaryLength / tolerance * 0.15 +
                normalVariationDegrees * 0.5 +
                curvatureIndicator * std::sqrt(std::max(totalFaceArea, tolerance * tolerance)) * 12.0)),
            96,
            480);
        const int trainingInteriorCount = std::max(48, totalSampleBudget / 2);
        const int selectionCount = std::max(24, totalSampleBudget / 4);
        const int holdoutCount = std::max(24, totalSampleBudget - trainingInteriorCount - selectionCount);
        auto training = sampleTriangles(triangles, triangleArea, trainingInteriorCount, 17);
        auto selection = sampleTriangles(triangles, triangleArea, selectionCount, 10007, training);
        auto holdout = sampleTriangles(triangles, triangleArea, holdoutCount, 20011, training, selection);
        appendBoundarySamples(document, faces, candidate.boundary_edges, tolerance, training);
        if (static_cast<int>(training.size()) < 32 ||
            static_cast<int>(selection.size()) < selectionCount ||
            static_cast<int>(holdout.size()) < holdoutCount) {
            result.report.message = "训练、模型选择和独立终验样本不足或无法保持空间互斥。";
            return result;
        }
        result.report.trainingSampleCount = static_cast<int>(training.size());
        result.report.selectionSampleCount = static_cast<int>(selection.size());
        result.report.holdoutSampleCount = static_cast<int>(holdout.size());

        const auto supportSurface = BRep_Tool::Surface(document.topology().face(supportFaceId));
        std::string parameterError;
        int orientationSign = 0;
        double minU = std::numeric_limits<double>::infinity();
        double minV = std::numeric_limits<double>::infinity();
        double maxU = -std::numeric_limits<double>::infinity();
        double maxV = -std::numeric_limits<double>::infinity();
        std::shared_ptr<const StlHybridParameterization> parameterization;
        const auto originalTraining = training;
        const auto originalSelection = selection;
        const auto originalHoldout = holdout;
        bool parametersInitialized = !supportSurface.IsNull() &&
            projectInitialParameters(
                training, supportSurface, orientationSign, minU, minV, maxU, maxV, parameterError) &&
            projectInitialParameters(
                selection, supportSurface, orientationSign, minU, minV, maxU, maxV, parameterError) &&
            projectInitialParameters(
                holdout, supportSurface, orientationSign, minU, minV, maxU, maxV, parameterError);
        if (parametersInitialized &&
            maxU - minU > Precision::PConfusion() &&
            maxV - minV > Precision::PConfusion()) {
            normalizeParameters(training, minU, minV, maxU, maxV);
            normalizeParameters(selection, minU, minV, maxU, maxV);
            normalizeParameters(holdout, minU, minV, maxU, maxV);
            parametersInitialized = !parametersOverlap(training, selection, holdout, tolerance);
            if (!parametersInitialized) {
                parameterError = "主 STEP 支撑面投影参数化出现重叠映射。";
            }
        } else if (parametersInitialized) {
            parametersInitialized = false;
            parameterError = "主 STEP 支撑面投影参数区间退化。";
        }
        if (parametersInitialized) {
            result.report.parameterization.success = true;
            result.report.parameterization.source =
                SurfaceParameterizationSource::SupportSurfaceProjection;
            result.report.parameterization.message =
                "主 STEP 支撑面投影保持单值，已作为拟合参数域。";
            result.report.parameterizationSource =
                SurfaceParameterizationSource::SupportSurfaceProjection;
        } else {
            const std::string supportProjectionFailure = parameterError;
            training = originalTraining;
            selection = originalSelection;
            holdout = originalHoldout;
            parameterError.clear();
            parameterization = StlHybridParameterization::buildFromStepSelection(
                document,
                faces,
                tolerance,
                result.report.parameterization);
            parametersInitialized = parameterization &&
                projectStepParameters(training, *parameterization, parameterError) &&
                projectStepParameters(selection, *parameterization, parameterError) &&
                projectStepParameters(holdout, *parameterization, parameterError);
            if (parametersInitialized &&
                !parametersOverlap(training, selection, holdout, tolerance)) {
                minU = 0.0;
                minV = 0.0;
                maxU = 1.0;
                maxV = 1.0;
                result.report.parameterizationSource = SurfaceParameterizationSource::StepHarmonic;
                if (!supportProjectionFailure.empty()) {
                    result.report.parameterization.message +=
                        "；主 STEP 支撑面投影未采用：" + supportProjectionFailure;
                }
            } else {
                parametersInitialized = false;
                if (parameterError.empty()) {
                    parameterError = result.report.parameterization.message.empty()
                        ? "STEP 调和参数化出现重叠映射。"
                        : result.report.parameterization.message;
                }
                if (!supportProjectionFailure.empty()) {
                    parameterError = supportProjectionFailure + "；" + parameterError;
                }
                parameterization.reset();
            }
        }
        if (!parametersInitialized) {
            result.report.message = parameterError.empty() ? "参数初始化失败。" : parameterError;
            return result;
        }

        std::vector<SourceSample> boundaryConstraints;
        std::vector<SourceSample> fittingSamples;
        boundaryConstraints.reserve(training.size());
        fittingSamples.reserve(training.size());
        for (const auto& sample : training) {
            (sample.boundary ? boundaryConstraints : fittingSamples).push_back(sample);
        }
        if (boundaryConstraints.empty() || fittingSamples.empty()) {
            result.report.message = "参数化未生成内部软样本或外边界硬约束。";
            return result;
        }
        result.report.boundaryConstraintCount = static_cast<int>(boundaryConstraints.size());

        if (parameterization != nullptr) {
            const auto sampleCountBeforeGuards = fittingSamples.size();
            appendExteriorGuardSamples(
                *parameterization,
                boundaryConstraints,
                fittingSamples);
            result.report.exteriorGuardSampleCount = static_cast<int>(
                fittingSamples.size() - sampleCountBeforeGuards);
        }

        if (options.scanReference != nullptr && options.scanReferenceWeight > 0.0) {
            const auto scanPatch = scanPatchForCandidate(document, candidate, options);
            auto scanSamples = sampleScanReference(
                scanPatch,
                std::min(128, trainingInteriorCount),
                parameterization.get(),
                supportSurface,
                minU,
                minV,
                maxU,
                maxV,
                options,
                result.report.maxStepToScanDistance);
            result.report.scanReferenceSampleCount = static_cast<int>(scanSamples.size());
            fittingSamples.insert(fittingSamples.end(), scanSamples.begin(), scanSamples.end());
        }

        const int requestedControlPointLimit = std::max(0, options.maximumControlPoints);
        if (requestedControlPointLimit < 16) {
            result.report.message = "控制点上限小于三次 4×4 起始控制网。";
            return result;
        }
        const int minimumConstraintControlPoints =
            static_cast<int>(boundaryConstraints.size()) +
            options.maximumBoundaryConstraintRefinements + 16;
        const int evidenceSupportedControlPoints =
            static_cast<int>(fittingSamples.size() + selection.size() + boundaryConstraints.size());
        const int controlPointLimit = std::min(
            requestedControlPointLimit,
            std::max(minimumConstraintControlPoints, evidenceSupportedControlPoints));
        std::vector<double> uInnerKnots;
        std::vector<double> vInnerKnots;
        const int initialControlPointTarget = std::min(
            controlPointLimit,
            static_cast<int>(boundaryConstraints.size()) +
                options.maximumBoundaryConstraintRefinements + 16);
        while ((degree + 1 + static_cast<int>(uInnerKnots.size())) *
                   (degree + 1 + static_cast<int>(vInnerKnots.size())) <
               initialControlPointTarget) {
            auto nextU = uInnerKnots;
            auto nextV = vInnerKnots;
            if (nextU.size() <= nextV.size()) {
                insertLargestIntervalKnot(nextU);
            } else {
                insertLargestIntervalKnot(nextV);
            }
            const int nextCount = (degree + 1 + static_cast<int>(nextU.size())) *
                (degree + 1 + static_cast<int>(nextV.size()));
            if (nextCount > controlPointLimit) {
                result.report.failureStage =
                    RestrictedBSplinePatchFitFailureStage::BoundaryConstraints;
                result.report.message =
                    "控制点上限不足以承载原始边界等式约束。";
                return result;
            }
            uInnerKnots.swap(nextU);
            vInnerKnots.swap(nextV);
            ++result.report.knotInsertionCount;
        }
        Handle(Geom_BSplineSurface) acceptedSurface;
        ModelQuality selectionQuality;
        bool modelJacobianValid = false;
        auto solveWithBoundaryRefinement = [&](
            const std::vector<SourceSample>& softSamples,
            std::vector<SourceSample>& constraints,
            const std::vector<double>& currentUKnots,
            const std::vector<double>& currentVKnots) {
            SurfaceSolveResult solved;
            while (true) {
                solved = solveSurface(
                    softSamples,
                    constraints,
                    currentUKnots,
                    currentVKnots,
                    options);
                result.report.maxBoundaryConstraintResidual = std::max(
                    result.report.maxBoundaryConstraintResidual,
                    solved.maximumConstraintResidual);
                if (solved.surface.IsNull()) {
                    result.report.failureStage =
                        RestrictedBSplinePatchFitFailureStage::BoundaryConstraints;
                    return solved;
                }
                const auto boundaryQuality = evaluateBoundaryHoldout(
                    document,
                    candidate.boundary_edges,
                    solved.surface);
                result.report.boundaryHoldoutSampleCount = boundaryQuality.sampleCount;
                result.report.maxBoundaryDistance = std::max(
                    result.report.maxBoundaryDistance,
                    boundaryQuality.maximumDistance);
                if (!boundaryQuality.valid) {
                    solved.surface.Nullify();
                    result.report.failureStage =
                        RestrictedBSplinePatchFitFailureStage::BoundaryHoldout;
                    return solved;
                }
                if (boundaryQuality.maximumDistance <= boundaryTolerance) {
                    return solved;
                }
                if (result.report.boundaryConstraintRefinementCount >=
                    options.maximumBoundaryConstraintRefinements) {
                    solved.surface.Nullify();
                    result.report.failureStage =
                        RestrictedBSplinePatchFitFailureStage::BoundaryHoldout;
                    return solved;
                }
                SourceSample addedConstraint;
                if (!parameterizePoint(
                        boundaryQuality.worstPoint,
                        parameterization.get(),
                        supportSurface,
                        minU,
                        minV,
                        maxU,
                        maxV,
                        addedConstraint) ||
                    !isSpatiallyDistinct(addedConstraint, constraints)) {
                    solved.surface.Nullify();
                    result.report.failureStage =
                        RestrictedBSplinePatchFitFailureStage::BoundaryHoldout;
                    return solved;
                }
                constraints.push_back(addedConstraint);
                ++result.report.boundaryConstraintRefinementCount;
                result.report.boundaryConstraintCount = static_cast<int>(constraints.size());
            }
        };
        while ((degree + 1 + static_cast<int>(uInnerKnots.size())) *
                   (degree + 1 + static_cast<int>(vInnerKnots.size())) <= controlPointLimit) {
            Handle(Geom_BSplineSurface) surface;
            auto currentSamples = fittingSamples;
            std::vector<UvIterationReport> iterationReports;
            bool surfaceNeedsBoundaryRefinement = false;
            for (int iteration = 0; iteration < options.maximumUvIterations; ++iteration) {
                const auto solved = solveWithBoundaryRefinement(
                    currentSamples,
                    boundaryConstraints,
                    uInnerKnots,
                    vInnerKnots);
                if (solved.surface.IsNull()) {
                    result.report.message =
                        result.report.failureStage ==
                            RestrictedBSplinePatchFitFailureStage::BoundaryHoldout
                        ? "边界硬约束细化耗尽后稠密边界仍未通过质量门。"
                        : "稀疏 KKT 未能满足原始边界等式约束。";
                    return result;
                }
                surface = solved.surface;
                surfaceNeedsBoundaryRefinement = false;
                UvIterationReport iterationReport;
                iterationReport.iteration = iteration + 1;
                iterationReport.objectiveBefore = solved.objective;
                std::vector<std::array<double, 2>> projectedParameters;
                if (!reprojectTrainingSamples(
                        currentSamples,
                        surface,
                        projectedParameters,
                        iterationReport.maximumShift)) {
                    result.report.failureStage =
                        RestrictedBSplinePatchFitFailureStage::UvIteration;
                    result.report.message = "更新曲面后内部样本重新投影失败。";
                    return result;
                }
                if (iterationReport.maximumShift < options.uvConvergenceTolerance) {
                    iterationReport.objectiveAfter = solved.objective;
                    iterationReport.reason = "Converged";
                    iterationReports.push_back(iterationReport);
                    break;
                }

                bool acceptedStep = false;
                for (int backtrack = 0;
                     backtrack < options.maximumUvBacktrackingSteps;
                     ++backtrack) {
                    const double step = std::ldexp(1.0, -backtrack);
                    auto trialSamples = currentSamples;
                    if (!applyProjectedParameters(
                            trialSamples,
                            projectedParameters,
                            step,
                            parameterization.get()) ||
                        parametersOverlap(trialSamples, {}, {}, tolerance)) {
                        continue;
                    }
                    const auto trial = solveSurface(
                        trialSamples,
                        boundaryConstraints,
                        uInnerKnots,
                        vInnerKnots,
                        options);
                    double observedJacobian = 0.0;
                    if (trial.surface.IsNull() ||
                        !(trial.objective + 1.0e-14 < solved.objective) ||
                        !evaluateJacobian(
                            trial.surface,
                            parameterization.get(),
                            options.minimumNormalizedJacobian,
                            observedJacobian)) {
                        continue;
                    }
                    currentSamples.swap(trialSamples);
                    surface = trial.surface;
                    surfaceNeedsBoundaryRefinement = true;
                    iterationReport.objectiveAfter = trial.objective;
                    iterationReport.backtrackingSteps = backtrack;
                    iterationReport.accepted = true;
                    iterationReport.reason = "Accepted";
                    acceptedStep = true;
                    break;
                }
                if (!acceptedStep) {
                    iterationReport.objectiveAfter = solved.objective;
                    iterationReport.reason = "NoImprovingStep";
                    iterationReports.push_back(iterationReport);
                    break;
                }
                iterationReports.push_back(iterationReport);
            }
            if (surfaceNeedsBoundaryRefinement) {
                const auto solved = solveWithBoundaryRefinement(
                    currentSamples,
                    boundaryConstraints,
                    uInnerKnots,
                    vInnerKnots);
                if (solved.surface.IsNull()) {
                    result.report.message =
                        result.report.failureStage ==
                            RestrictedBSplinePatchFitFailureStage::BoundaryHoldout
                        ? "UV 迭代结束后的边界硬约束细化未通过质量门。"
                        : "UV 迭代结束后无法重新满足原始边界等式约束。";
                    return result;
                }
                surface = solved.surface;
            }
            result.report.uvIterations = iterationReports;
            result.report.iterationCount = static_cast<int>(iterationReports.size());
            result.report.uControlPointCount = surface->NbUPoles();
            result.report.vControlPointCount = surface->NbVPoles();
            result.report.controlPointCount = surface->NbUPoles() * surface->NbVPoles();
            selectionQuality = evaluateModel(selection, surface, options);
            if (selectionQuality.passed) {
                double observedJacobian = 0.0;
                modelJacobianValid = evaluateJacobian(
                    surface,
                    parameterization.get(),
                    options.minimumNormalizedJacobian,
                    observedJacobian);
                result.report.minimumNormalizedJacobian = observedJacobian;
                if (modelJacobianValid) {
                    acceptedSurface = surface;
                    break;
                }
            }
            auto nextU = uInnerKnots;
            auto nextV = vInnerKnots;
            if (selectionQuality.passed) {
                if (nextU.size() <= nextV.size()) {
                    insertLargestIntervalKnot(nextU);
                } else {
                    insertLargestIntervalKnot(nextV);
                }
            } else {
                const double uWidth = containingIntervalWidth(uInnerKnots, selectionQuality.worstU);
                const double vWidth = containingIntervalWidth(vInnerKnots, selectionQuality.worstV);
                if (uWidth >= vWidth) {
                    insertWorstIntervalKnot(nextU, selectionQuality.worstU);
                } else {
                    insertWorstIntervalKnot(nextV, selectionQuality.worstV);
                }
            }
            const int nextCount = (degree + 1 + static_cast<int>(nextU.size())) *
                (degree + 1 + static_cast<int>(nextV.size()));
            if (nextCount > controlPointLimit) {
                break;
            }
            uInnerKnots.swap(nextU);
            vInnerKnots.swap(nextV);
            ++result.report.knotInsertionCount;
        }

        result.report.maxSelectionDistance = selectionQuality.maxDistance;
        result.report.maxSelectionNormalAngleDegrees = selectionQuality.maxNormalAngle;
        result.report.selectionPassed = selectionQuality.passed;
        if (acceptedSurface.IsNull()) {
            result.report.failureStage = selectionQuality.passed
                ? RestrictedBSplinePatchFitFailureStage::Jacobian
                : RestrictedBSplinePatchFitFailureStage::ModelSelection;
            result.report.message = selectionQuality.passed
                ? "达到数据支持的控制点上限后参数化仍发生翻转或近奇异。"
                : "达到数据支持的控制点上限后模型选择样本仍未通过质量门。";
            return result;
        }

        const auto extentQuality = evaluateSupportSurfaceExtent(
            training,
            selection,
            holdout,
            acceptedSurface);
        result.report.supportSurfaceExtentRatio = extentQuality.extentRatio;
        result.report.supportSurfaceMaxPoleOverflowRatio =
            extentQuality.maximumPoleOverflowRatio;
        if (!extentQuality.valid ||
            extentQuality.extentRatio > 2.0 ||
            extentQuality.maximumPoleOverflowRatio > 0.5) {
            result.report.failureStage = RestrictedBSplinePatchFitFailureStage::ModelSelection;
            result.report.message =
                "拟合支撑曲面控制网超出原始选区包络，拒绝可能在商业 CAD 中展开为超大面片的结果。";
            return result;
        }

        const auto holdoutQuality = evaluateModel(holdout, acceptedSurface, options);
        result.report.maxHoldoutDistance = holdoutQuality.maxDistance;
        result.report.maxHoldoutNormalAngleDegrees = holdoutQuality.maxNormalAngle;
        result.report.holdoutPassed = holdoutQuality.passed;
        if (!holdoutQuality.passed) {
            result.report.failureStage = RestrictedBSplinePatchFitFailureStage::Holdout;
            result.report.message = "独立终验样本未通过距离或法向质量门。";
            return result;
        }

        result.report.parameterizationValid = evaluateJacobian(
            acceptedSurface,
            parameterization.get(),
            options.minimumNormalizedJacobian,
            result.report.minimumNormalizedJacobian);
        if (!result.report.parameterizationValid) {
            result.report.failureStage = RestrictedBSplinePatchFitFailureStage::Jacobian;
            result.report.message = "拟合曲面参数化发生翻转或近奇异。";
            return result;
        }

        BRepBuilderAPI_MakeFace supportFaceBuilder(acceptedSurface, Precision::Confusion());
        if (!supportFaceBuilder.IsDone()) {
            result.report.failureStage = RestrictedBSplinePatchFitFailureStage::Retrim;
            result.report.message = "无法从拟合支撑曲面创建基础面。";
            return result;
        }
        BoundaryConstrainedSurfaceRetrimOptions retrimOptions;
        retrimOptions.samplesPerEdge = 16;
        retrimOptions.projectionTolerance = retrimTolerance;
        retrimOptions.rebuildBoundaryPcurves = true;
        const auto retrim = BoundaryConstrainedSurfaceRetrim().retrim(
            document,
            boundary,
            {supportFaceBuilder.Face()},
            document.topology().face(supportFaceId),
            retrimOptions);
        if (!retrim.success || retrim.replacementFace.IsNull()) {
            result.report.failureStage = RestrictedBSplinePatchFitFailureStage::Retrim;
            result.report.message = "原始边界无法可靠裁剪到拟合支撑曲面：" + retrim.message +
                "；pcurve 尝试/成功/失败=" +
                std::to_string(retrim.boundaryEdgePcurveRebuildAttemptCount) + "/" +
                std::to_string(retrim.boundaryEdgePcurveRebuildSuccessCount) + "/" +
                std::to_string(retrim.boundaryEdgePcurveRebuildFailureCount) +
                "，SameParameter 检查/失败=" +
                std::to_string(retrim.boundaryEdgeSameParameterCheckCount) + "/" +
                std::to_string(retrim.boundaryEdgeSameParameterFailureCount) +
                "，最大偏差=" +
                std::to_string(retrim.boundaryEdgeMaxSameParameterDeviation);
            return result;
        }
        result.report.maxBoundaryDistance = std::max(
            retrim.maxProjectionDistance,
            retrim.surfaceCoverageMaxProjectionDistance);
        const auto boundaryHoldout = evaluateBoundaryHoldout(
            document,
            candidate.boundary_edges,
            acceptedSurface);
        result.report.boundaryHoldoutSampleCount = boundaryHoldout.sampleCount;
        if (!boundaryHoldout.valid || boundaryHoldout.maximumDistance > boundaryTolerance) {
            result.report.failureStage = RestrictedBSplinePatchFitFailureStage::BoundaryHoldout;
            result.report.message = "独立边界终验样本未通过距离质量门。";
            return result;
        }
        result.report.maxBoundaryDistance = std::max(
            result.report.maxBoundaryDistance,
            boundaryHoldout.maximumDistance);

        BRepCheck_Analyzer brepAnalyzer(retrim.replacementFace, Standard_True);
        result.report.brepCheckValid = brepAnalyzer.IsValid();
        if (!result.report.brepCheckValid) {
            result.report.failureStage = RestrictedBSplinePatchFitFailureStage::BrepCheck;
            result.report.message = "裁剪后的 B-rep 面未通过 BRepCheck，状态=" +
                invalidBRepStatuses(retrim.replacementFace, brepAnalyzer) + "。";
            return result;
        }
        result.face = retrim.replacementFace;
        result.surface = acceptedSurface;
        result.report.uControlPointCount = acceptedSurface->NbUPoles();
        result.report.vControlPointCount = acceptedSurface->NbVPoles();
        result.report.controlPointCount = acceptedSurface->NbUPoles() * acceptedSurface->NbVPoles();
        result.report.success = true;
        result.report.failureStage = RestrictedBSplinePatchFitFailureStage::None;
        result.report.message = "受限 B-spline 分组拟合通过模型选择和独立终验。";
        return result;
    } catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        result.report.message = std::string("OCCT 拟合失败：") + (message == nullptr ? "未知错误" : message);
    } catch (const std::exception& error) {
        result.report.message = std::string("分组拟合失败：") + error.what();
    } catch (...) {
        result.report.message = "分组拟合失败：未知错误。";
    }
    return result;
}

} // namespace spo
