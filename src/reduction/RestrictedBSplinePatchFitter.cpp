#include "reduction/RestrictedBSplinePatchFitter.h"
#include "reduction/StlHybridParameterization.h"

#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/BoundaryConstrainedSurfaceRetrim.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
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
#include <TopLoc_Location.hxx>

#include <Eigen/Sparse>
#include <Eigen/SparseQR>
#include <Eigen/Eigenvalues>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
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
    bool hasStlReference = false;
    gp_Pnt stlReferencePoint;
    gp_Vec stlReferenceNormal;
    double stlReferenceDistance = 0.0;
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
    int totalSampleBudget,
    std::vector<SourceSample>& samples) {
    const std::set<FaceId> sourceFaceSet(sourceFaces.begin(), sourceFaces.end());
    double totalLength = 0.0;
    for (const auto edgeId : edges) {
        totalLength += edgeLength(document.topology().edge(edgeId));
    }
    for (const auto edgeId : edges) {
        const auto& edge = document.topology().edge(edgeId);
        try {
            BRepAdaptor_Curve curve(edge);
            const double first = curve.FirstParameter();
            const double last = curve.LastParameter();
            const int samplesPerEdge = std::max(1, static_cast<int>(std::lround(
                static_cast<double>(totalSampleBudget) *
                edgeLength(edge) / std::max(totalLength, Precision::Confusion()))));
            for (int index = 0; index < samplesPerEdge; ++index) {
                const double t = first + (last - first) *
                    (static_cast<double>(index) + 0.5) / static_cast<double>(samplesPerEdge);
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
        } else if (orientationSign != currentSign) {
            error = "参数初始化出现方向翻转。";
            return false;
        }
        minU = std::min(minU, sample.u);
        minV = std::min(minV, sample.v);
        maxU = std::max(maxU, sample.u);
        maxV = std::max(maxV, sample.v);
    }
    return true;
}

bool projectHybridParameters(
    std::vector<SourceSample>& samples,
    const StlHybridParameterization& parameterization,
    double& minU,
    double& minV,
    double& maxU,
    double& maxV,
    double& maximumReferenceDistance,
    std::string& error) {
    for (auto& sample : samples) {
        const auto projection = parameterization.project(sample.point);
        if (!projection.valid) {
            error = "无法将 STEP 精确采样映射到裁剪 STL 参数图。";
            return false;
        }
        sample.u = projection.u;
        sample.v = projection.v;
        sample.hasStlReference = true;
        sample.stlReferencePoint = projection.referencePoint;
        sample.stlReferenceNormal = projection.referenceNormal;
        sample.stlReferenceDistance = projection.distance;
        maximumReferenceDistance = std::max(maximumReferenceDistance, projection.distance);
        minU = std::min(minU, sample.u);
        minV = std::min(minV, sample.v);
        maxU = std::max(maxU, sample.u);
        maxV = std::max(maxV, sample.v);
    }
    return true;
}

bool assignLocalHybridParameters(
    std::vector<SourceSample>& training,
    std::vector<SourceSample>& selection,
    std::vector<SourceSample>& holdout,
    double& minU,
    double& minV,
    double& maxU,
    double& maxV,
    std::string& error) {
    std::vector<SourceSample*> samples;
    samples.reserve(training.size() + selection.size() + holdout.size());
    for (auto& sample : training) samples.push_back(&sample);
    for (auto& sample : selection) samples.push_back(&sample);
    for (auto& sample : holdout) samples.push_back(&sample);
    if (samples.size() < 3 || std::any_of(samples.begin(), samples.end(), [](const auto* sample) {
            return !sample->hasStlReference;
        })) {
        error = "裁剪 STL 局部参数轴缺少有效参考点。";
        return false;
    }

    Eigen::Vector3d center = Eigen::Vector3d::Zero();
    for (const auto* sample : samples) {
        center += Eigen::Vector3d(
            sample->stlReferencePoint.X(),
            sample->stlReferencePoint.Y(),
            sample->stlReferencePoint.Z());
    }
    center /= static_cast<double>(samples.size());
    Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
    for (const auto* sample : samples) {
        const Eigen::Vector3d delta(
            sample->stlReferencePoint.X() - center.x(),
            sample->stlReferencePoint.Y() - center.y(),
            sample->stlReferencePoint.Z() - center.z());
        covariance += delta * delta.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(covariance);
    if (solver.info() != Eigen::Success ||
        solver.eigenvalues()[1] <= 1.0e-16 ||
        !solver.eigenvectors().allFinite()) {
        error = "裁剪 STL 局部参数轴退化。";
        return false;
    }
    const Eigen::Vector3d uAxis = solver.eigenvectors().col(2);
    const Eigen::Vector3d vAxis = solver.eigenvectors().col(1);
    minU = std::numeric_limits<double>::infinity();
    minV = std::numeric_limits<double>::infinity();
    maxU = -std::numeric_limits<double>::infinity();
    maxV = -std::numeric_limits<double>::infinity();
    for (auto* sample : samples) {
        const Eigen::Vector3d delta(
            sample->stlReferencePoint.X() - center.x(),
            sample->stlReferencePoint.Y() - center.y(),
            sample->stlReferencePoint.Z() - center.z());
        sample->u = delta.dot(uAxis);
        sample->v = delta.dot(vAxis);
        minU = std::min(minU, sample->u);
        minV = std::min(minV, sample->v);
        maxU = std::max(maxU, sample->u);
        maxV = std::max(maxV, sample->v);
    }
    return true;
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

Handle(Geom_BSplineSurface) solveSurface(
    const std::vector<SourceSample>& samples,
    const std::vector<double>& uInnerKnots,
    const std::vector<double>& vInnerKnots,
    const RestrictedBSplinePatchFitOptions& options) {
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
    Eigen::VectorXd rhs(static_cast<int>(rightHandSide.size()));
    for (int row = 0; row < rhs.size(); ++row) {
        rhs[row] = rightHandSide[static_cast<std::size_t>(row)];
    }
    Eigen::SparseQR<Eigen::SparseMatrix<double>, Eigen::COLAMDOrdering<int>> solver;
    solver.compute(matrix);
    if (solver.info() != Eigen::Success) {
        return {};
    }
    const Eigen::VectorXd solution = solver.solve(rhs);
    if (solver.info() != Eigen::Success || !solution.allFinite()) {
        return {};
    }

    TColgp_Array2OfPnt poles(1, uControlCount, 1, vControlCount);
    for (int uIndex = 0; uIndex < uControlCount; ++uIndex) {
        for (int vIndex = 0; vIndex < vControlCount; ++vIndex) {
            const int poleIndex = uIndex * vControlCount + vIndex;
            poles.SetValue(
                uIndex + 1,
                vIndex + 1,
                gp_Pnt(solution[poleIndex], solution[poleCount + poleIndex], solution[2 * poleCount + poleIndex]));
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
    return new Geom_BSplineSurface(
        poles,
        uUnique,
        vUnique,
        uMultiplicities,
        vMultiplicities,
        degree,
        degree,
        Standard_False,
        Standard_False);
}

bool reprojectTrainingSamples(
    std::vector<SourceSample>& samples,
    const Handle(Geom_BSplineSurface)& surface,
    double& maximumShift) {
    maximumShift = 0.0;
    for (auto& sample : samples) {
        GeomAPI_ProjectPointOnSurf projector(sample.point, surface);
        if (projector.NbPoints() < 1) {
            return false;
        }
        double u = 0.0;
        double v = 0.0;
        projector.LowerDistanceParameters(u, v);
        maximumShift = std::max(maximumShift, std::hypot(u - sample.u, v - sample.v));
        sample.u = std::clamp(u, 0.0, 1.0);
        sample.v = std::clamp(v, 0.0, 1.0);
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
    double minimumNormalized,
    double& observedMinimum) {
    observedMinimum = std::numeric_limits<double>::infinity();
    gp_Vec reference;
    for (int uIndex = 0; uIndex < 10; ++uIndex) {
        for (int vIndex = 0; vIndex < 10; ++vIndex) {
            const double u = (static_cast<double>(uIndex) + 0.5) / 10.0;
            const double v = (static_cast<double>(vIndex) + 0.5) / 10.0;
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
            if (reference.SquareMagnitude() <= Precision::SquareConfusion()) {
                reference = cross;
            } else if (reference.Dot(cross) <= 0.0) {
                return false;
            }
        }
    }
    return true;
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

bool evaluateBoundaryHoldout(
    const ShapeDocument& document,
    const std::vector<EdgeId>& edges,
    const Handle(Geom_BSplineSurface)& surface,
    double tolerance,
    int& sampleCount,
    double& maximumDistance) {
    sampleCount = 0;
    maximumDistance = 0.0;
    constexpr int samplesPerEdge = 17;
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
                ++sampleCount;
                if (projector.NbPoints() < 1) {
                    return false;
                }
                maximumDistance = std::max(maximumDistance, projector.LowerDistance());
                if (maximumDistance > tolerance) {
                    return false;
                }
            }
        } catch (...) {
            return false;
        }
    }
    return sampleCount > 0;
}

} // namespace

RestrictedBSplinePatchFitResult RestrictedBSplinePatchFitter::fit(
    const ShapeDocument& document,
    const std::vector<FaceId>& sourceFaces,
    const RestrictedBSplinePatchFitOptions& options) const {
    RestrictedBSplinePatchFitResult result;
    if (!document.hasShape() || sourceFaces.size() < 2) {
        result.report.message = "受限曲面拟合至少需要两个原始面。";
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
        const int boundarySampleBudget = std::clamp(
            static_cast<int>(std::ceil(totalBoundaryLength / tolerance * 0.1)),
            16,
            128);
        appendBoundarySamples(document, faces, candidate.boundary_edges, boundarySampleBudget, training);
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
        const bool useHybrid = options.hybridParameterization != nullptr;
        bool parametersInitialized = false;
        if (useHybrid) {
            parametersInitialized =
                projectHybridParameters(
                    training, *options.hybridParameterization,
                    minU, minV, maxU, maxV,
                    result.report.maxStepToStlDistance, parameterError) &&
                projectHybridParameters(
                    selection, *options.hybridParameterization,
                    minU, minV, maxU, maxV,
                    result.report.maxStepToStlDistance, parameterError) &&
                projectHybridParameters(
                    holdout, *options.hybridParameterization,
                    minU, minV, maxU, maxV,
                    result.report.maxStepToStlDistance, parameterError) &&
                assignLocalHybridParameters(
                    training, selection, holdout,
                    minU, minV, maxU, maxV, parameterError);
            result.report.hybridParameterizationUsed = parametersInitialized;
        } else {
            parametersInitialized = !supportSurface.IsNull() &&
                projectInitialParameters(
                    training, supportSurface, orientationSign, minU, minV, maxU, maxV, parameterError) &&
                projectInitialParameters(
                    selection, supportSurface, orientationSign, minU, minV, maxU, maxV, parameterError) &&
                projectInitialParameters(
                    holdout, supportSurface, orientationSign, minU, minV, maxU, maxV, parameterError);
        }
        if (!parametersInitialized) {
            result.report.message = parameterError.empty() ? "参数初始化失败。" : parameterError;
            return result;
        }
        if (maxU - minU <= Precision::PConfusion() || maxV - minV <= Precision::PConfusion()) {
            result.report.message = "参数初始化退化为近奇异区间。";
            return result;
        }
        normalizeParameters(training, minU, minV, maxU, maxV);
        normalizeParameters(selection, minU, minV, maxU, maxV);
        normalizeParameters(holdout, minU, minV, maxU, maxV);
        if (parametersOverlap(training, selection, holdout, tolerance)) {
            result.report.message = "参数初始化出现重叠映射。";
            return result;
        }

        std::vector<SourceSample> fittingSamples = training;
        if (useHybrid && options.stlReferenceWeight > 0.0) {
            fittingSamples.reserve(training.size() * 2);
            for (const auto& sample : training) {
                if (sample.boundary || !sample.hasStlReference ||
                    sample.stlReferenceDistance > options.maximumStlReferenceDistance) {
                    continue;
                }
                auto reference = sample;
                reference.point = sample.stlReferencePoint;
                reference.normal = sample.stlReferenceNormal;
                if (reference.normal.Dot(sample.normal) < 0.0) {
                    reference.normal.Reverse();
                }
                reference.positionWeight = options.stlReferenceWeight;
                reference.normalWeight = 0.0;
                reference.boundary = false;
                fittingSamples.push_back(reference);
                ++result.report.stlReferenceSampleCount;
            }
        }

        const int controlPointLimit = std::min(
            std::max(0, options.maximumControlPoints),
            static_cast<int>(training.size() / 2));
        if (controlPointLimit < 16) {
            result.report.message = "控制点上限小于三次 4×4 起始控制网。";
            return result;
        }
        std::vector<double> uInnerKnots;
        std::vector<double> vInnerKnots;
        Handle(Geom_BSplineSurface) acceptedSurface;
        ModelQuality selectionQuality;
        while ((degree + 1 + static_cast<int>(uInnerKnots.size())) *
                   (degree + 1 + static_cast<int>(vInnerKnots.size())) <= controlPointLimit) {
            Handle(Geom_BSplineSurface) surface;
            for (int iteration = 0; iteration < std::max(1, options.maximumIterations); ++iteration) {
                surface = solveSurface(
                    useHybrid ? fittingSamples : training,
                    uInnerKnots,
                    vInnerKnots,
                    options);
                if (surface.IsNull()) {
                    result.report.message = "Eigen SparseQR 未能求解控制点。";
                    return result;
                }
                result.report.iterationCount = std::max(result.report.iterationCount, iteration + 1);
                if (useHybrid) {
                    break;
                }
                double maximumShift = 0.0;
                if (!reprojectTrainingSamples(training, surface, maximumShift)) {
                    result.report.message = "更新曲面后训练点重新投影失败。";
                    return result;
                }
                if (maximumShift < 1.0e-8) {
                    break;
                }
            }
            selectionQuality = evaluateModel(selection, surface, options);
            if (selectionQuality.passed) {
                acceptedSurface = surface;
                break;
            }
            const double uWidth = containingIntervalWidth(uInnerKnots, selectionQuality.worstU);
            const double vWidth = containingIntervalWidth(vInnerKnots, selectionQuality.worstV);
            const bool insertU = uWidth >= vWidth;
            auto nextU = uInnerKnots;
            auto nextV = vInnerKnots;
            if (insertU) {
                insertWorstIntervalKnot(nextU, selectionQuality.worstU);
            } else {
                insertWorstIntervalKnot(nextV, selectionQuality.worstV);
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
            result.report.message = "达到控制点上限后模型选择样本仍未通过质量门。";
            return result;
        }

        const auto holdoutQuality = evaluateModel(holdout, acceptedSurface, options);
        result.report.maxHoldoutDistance = holdoutQuality.maxDistance;
        result.report.maxHoldoutNormalAngleDegrees = holdoutQuality.maxNormalAngle;
        result.report.holdoutPassed = holdoutQuality.passed;
        if (!holdoutQuality.passed) {
            result.report.message = "独立终验样本未通过距离或法向质量门。";
            return result;
        }

        result.report.parameterizationValid = evaluateJacobian(
            acceptedSurface,
            options.minimumNormalizedJacobian,
            result.report.minimumNormalizedJacobian);
        if (!result.report.parameterizationValid) {
            result.report.message = "拟合曲面参数化发生翻转或近奇异。";
            return result;
        }

        BRepBuilderAPI_MakeFace supportFaceBuilder(acceptedSurface, Precision::Confusion());
        if (!supportFaceBuilder.IsDone()) {
            result.report.message = "无法从拟合支撑曲面创建基础面。";
            return result;
        }
        BoundaryConstrainedSurfaceRetrimOptions retrimOptions;
        retrimOptions.samplesPerEdge = 16;
        retrimOptions.projectionTolerance = options.tolerance3d;
        retrimOptions.rebuildBoundaryPcurves = true;
        const auto retrim = BoundaryConstrainedSurfaceRetrim().retrim(
            document,
            boundary,
            {supportFaceBuilder.Face()},
            document.topology().face(supportFaceId),
            retrimOptions);
        if (!retrim.success || retrim.replacementFace.IsNull()) {
            result.report.message = "原始边界无法可靠裁剪到拟合支撑曲面：" + retrim.message;
            return result;
        }
        result.report.maxBoundaryDistance = std::max(
            retrim.maxProjectionDistance,
            retrim.surfaceCoverageMaxProjectionDistance);
        double boundaryHoldoutMaximum = 0.0;
        if (!evaluateBoundaryHoldout(
                document,
                candidate.boundary_edges,
                acceptedSurface,
                options.tolerance3d,
                result.report.boundaryHoldoutSampleCount,
                boundaryHoldoutMaximum)) {
            result.report.message = "独立边界终验样本未通过距离质量门。";
            return result;
        }
        result.report.maxBoundaryDistance = std::max(
            result.report.maxBoundaryDistance,
            boundaryHoldoutMaximum);

        result.report.brepCheckValid = BRepCheck_Analyzer(retrim.replacementFace, Standard_True).IsValid();
        if (!result.report.brepCheckValid) {
            result.report.message = "裁剪后的 B-rep 面未通过 BRepCheck。";
            return result;
        }
        result.face = retrim.replacementFace;
        result.surface = acceptedSurface;
        result.report.uControlPointCount = acceptedSurface->NbUPoles();
        result.report.vControlPointCount = acceptedSurface->NbVPoles();
        result.report.controlPointCount = acceptedSurface->NbUPoles() * acceptedSurface->NbVPoles();
        result.report.success = true;
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
