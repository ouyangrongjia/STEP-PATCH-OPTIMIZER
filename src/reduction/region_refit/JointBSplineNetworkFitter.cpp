#include "reduction/region_refit/JointBSplineNetworkFitter.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseLU>
#include <Eigen/SparseQR>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomConvert.hxx>
#include <GeomConvert_CompCurveToBSplineCurve.hxx>
#include <GeomFill_BSplineCurves.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_BoundedCurve.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spo {

namespace {

constexpr int kDegree = 3;
constexpr double kPi = 3.14159265358979323846;

struct KnotConfiguration {
    std::vector<double> u {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0};
    std::vector<double> v {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0};

    int uControlCount() const { return static_cast<int>(u.size()) + kDegree - 1; }
    int vControlCount() const { return static_cast<int>(v.size()) + kDegree - 1; }
};

struct BoundarySupportNetwork {
    std::vector<KnotConfiguration> knots;
    std::vector<std::optional<gp_Pnt>> fixedControls;
    int fixedControlCount = 0;
    double maximumOuterBoundaryDeviation = 0.0;
    double maximumInternalBoundaryDeviation = 0.0;
    bool productQualified = false;
    std::string message;
};

struct PatchLookup {
    std::unordered_map<std::size_t, std::size_t> localByGlobal;
};

struct SeamSample {
    int seamIndex = -1;
    int firstPatch = -1;
    int secondPatch = -1;
    std::size_t vertex = 0;
    bool requiredConstraintPoint = false;
};

struct MappedReferenceSample {
    std::size_t referenceVertex = 0;
    std::size_t carrierVertex = 0;
    int patch = -1;
    std::array<double, 2> uv {};
};

struct LinearRow {
    std::vector<std::pair<int, double>> coefficients;
    gp_Pnt target;
    std::uint64_t constraintGroup = 0;
    double constraintParameter = 0.0;
    bool requiredConstraintPoint = false;
    int sourcePatch = -1;
    std::array<double, 2> sourceUv {};
    bool originalBoundary = false;
};

struct NormalRow {
    std::vector<std::pair<int, double>> coefficients;
    gp_Dir normal {0.0, 0.0, 1.0};
};

struct ResidualSample {
    int patch = -1;
    double u = 0.0;
    double v = 0.0;
    double error = 0.0;
};

struct ValidationMetrics {
    double trainingMaximumDistance = 0.0;
    double selectionMaximumDistance = 0.0;
    double selectionMaximumScore = 0.0;
    double holdoutMaximumDistance = 0.0;
    double holdoutMaximumNormalDegrees = 0.0;
    double holdout95Distance = 0.0;
    double holdout95NormalDegrees = 0.0;
    std::size_t holdoutWorstDistanceVertex = 0;
    std::size_t holdoutWorstNormalVertex = 0;
    int holdoutWorstDistancePatch = -1;
    int holdoutWorstNormalPatch = -1;
    bool holdoutWorstDistanceInternalSeam = false;
    bool holdoutWorstNormalInternalSeam = false;
    double boundaryMaximumDistance = 0.0;
    double seamMaximumDistance = 0.0;
    double seamMaximumCurveDeviation = 0.0;
    double seamMaximumNormalDegrees = 0.0;
    double seamMaximumCurvatureDifference = 0.0;
    bool seamContinuityGatePassed = true;
    std::vector<ResidualSample> selectionResiduals;
    std::vector<double> holdoutDistances;
    std::vector<double> holdoutNormalDegrees;
    std::size_t projectionCount = 0;
    std::size_t uvIterationCount = 0;
    std::size_t projectionFallbackCount = 0;
};

std::vector<double> fullKnotVector(const std::vector<double>& unique) {
    std::vector<double> full;
    full.reserve(unique.size() + 2 * static_cast<std::size_t>(kDegree));
    for (int repeat = 0; repeat <= kDegree; ++repeat) full.push_back(unique.front());
    for (std::size_t index = 1; index + 1 < unique.size(); ++index) full.push_back(unique[index]);
    for (int repeat = 0; repeat <= kDegree; ++repeat) full.push_back(unique.back());
    return full;
}

std::vector<double> basisValues(
    const std::vector<double>& unique,
    const double parameter) {
    const auto knots = fullKnotVector(unique);
    const auto controlCount = static_cast<int>(unique.size()) + kDegree - 1;
    const auto clamped = std::clamp(parameter, unique.front(), unique.back());
    const auto lastControl = controlCount - 1;
    int span = lastControl;
    if (clamped < unique.back()) {
        const auto upper = std::upper_bound(
            knots.begin() + kDegree,
            knots.begin() + controlCount + 1,
            clamped);
        span = std::clamp(static_cast<int>(std::distance(knots.begin(), upper)) - 1,
            kDegree, lastControl);
    }

    std::array<double, kDegree + 1> local {};
    std::array<double, kDegree + 1> left {};
    std::array<double, kDegree + 1> right {};
    local[0] = 1.0;
    for (int degree = 1; degree <= kDegree; ++degree) {
        left[static_cast<std::size_t>(degree)] =
            clamped - knots[static_cast<std::size_t>(span + 1 - degree)];
        right[static_cast<std::size_t>(degree)] =
            knots[static_cast<std::size_t>(span + degree)] - clamped;
        double saved = 0.0;
        for (int index = 0; index < degree; ++index) {
            const auto denominator = right[static_cast<std::size_t>(index + 1)] +
                left[static_cast<std::size_t>(degree - index)];
            const auto term = denominator > 0.0
                ? local[static_cast<std::size_t>(index)] / denominator
                : 0.0;
            local[static_cast<std::size_t>(index)] =
                saved + right[static_cast<std::size_t>(index + 1)] * term;
            saved = left[static_cast<std::size_t>(degree - index)] * term;
        }
        local[static_cast<std::size_t>(degree)] = saved;
    }

    std::vector<double> result(static_cast<std::size_t>(controlCount), 0.0);
    for (int index = 0; index <= kDegree; ++index) {
        result[static_cast<std::size_t>(span - kDegree + index)] = local[static_cast<std::size_t>(index)];
    }
    return result;
}

std::vector<double> grevilleParameters(const std::vector<double>& unique) {
    const auto knots = fullKnotVector(unique);
    const auto controlCount = static_cast<int>(unique.size()) + kDegree - 1;
    std::vector<double> parameters(static_cast<std::size_t>(controlCount), 0.0);
    for (int control = 0; control < controlCount; ++control) {
        for (int offset = 1; offset <= kDegree; ++offset) {
            parameters[static_cast<std::size_t>(control)] +=
                knots[static_cast<std::size_t>(control + offset)];
        }
        parameters[static_cast<std::size_t>(control)] /= static_cast<double>(kDegree);
    }
    return parameters;
}

std::array<double, 2> normalizedParameter(const std::array<double, 2>& uv) {
    return {
        std::clamp(uv[0], 0.0, 1.0),
        std::clamp(uv[1], 0.0, 1.0)};
}

const std::vector<std::array<double, 2>>& surfaceParameters(
    const RegionPatchParameterization& parameterization) {
    return parameterization.surfaceParameters.size() ==
            parameterization.vertexIndices.size()
        ? parameterization.surfaceParameters
        : parameterization.parameters;
}

std::vector<PatchLookup> buildLookups(const RegionPatchLayout& layout) {
    std::vector<PatchLookup> lookups(layout.patches.size());
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        const auto& vertices = layout.patches[patch].vertexIndices;
        lookups[patch].localByGlobal.reserve(vertices.size());
        for (std::size_t local = 0; local < vertices.size(); ++local) {
            lookups[patch].localByGlobal.emplace(vertices[local], local);
        }
    }
    return lookups;
}

const RegionCurveNetwork* curveNetworkFor(
    const RegionPatchLayout& layout,
    const std::vector<RegionCurveNetwork>& networks) {
    const auto found = std::find_if(
        networks.begin(), networks.end(),
        [&](const auto& network) {
            return !layout.curveNetworkHash.empty() &&
                network.hash == layout.curveNetworkHash;
        });
    return found == networks.end() ? nullptr : &*found;
}

std::optional<std::array<double, 2>> curveParameterOnPatch(
    const RegionPatchLayout& layout,
    const RegionCurveNetworkEdge& edge,
    const int patch,
    const double parameter) {
    if (patch < 0 || patch >= layout.patchCount ||
        edge.meshVertices.size() < 2 ||
        edge.meshParameters.size() != edge.meshVertices.size()) {
        return std::nullopt;
    }
    const auto& parameterization =
        layout.patches[static_cast<std::size_t>(patch)];
    const auto exactPcurve = std::find_if(
        parameterization.edgePcurves.begin(),
        parameterization.edgePcurves.end(),
        [&](const auto& pcurve) { return pcurve.edgeIndex == edge.id; });
    if (exactPcurve != parameterization.edgePcurves.end()) {
        const auto uv = evaluateRegionPatchEdgePcurve(
            *exactPcurve, parameter);
        return uv.has_value()
            ? std::optional<std::array<double, 2>> {
                normalizedParameter(*uv)}
            : std::nullopt;
    }
    std::unordered_map<std::size_t, std::size_t> localByGlobal;
    localByGlobal.reserve(parameterization.vertexIndices.size());
    for (std::size_t local = 0;
         local < parameterization.vertexIndices.size(); ++local) {
        localByGlobal.emplace(parameterization.vertexIndices[local], local);
    }
    const auto clamped = std::clamp(
        parameter, edge.meshParameters.front(), edge.meshParameters.back());
    const auto upper = std::upper_bound(
        edge.meshParameters.begin(), edge.meshParameters.end(), clamped);
    const auto second = upper == edge.meshParameters.begin()
        ? std::size_t {1}
        : upper == edge.meshParameters.end()
        ? edge.meshParameters.size() - 1
        : static_cast<std::size_t>(
            std::distance(edge.meshParameters.begin(), upper));
    const auto first = second - 1;
    const auto firstFound = localByGlobal.find(edge.meshVertices[first]);
    const auto secondFound = localByGlobal.find(edge.meshVertices[second]);
    if (firstFound == localByGlobal.end() ||
        secondFound == localByGlobal.end()) {
        return std::nullopt;
    }
    const auto denominator =
        edge.meshParameters[second] - edge.meshParameters[first];
    const auto alpha = denominator > 1.0e-14
        ? std::clamp(
            (clamped - edge.meshParameters[first]) / denominator,
            0.0, 1.0)
        : 0.0;
    const auto firstUv = normalizedParameter(
        surfaceParameters(parameterization)[firstFound->second]);
    const auto secondUv = normalizedParameter(
        surfaceParameters(parameterization)[secondFound->second]);
    return std::array<double, 2> {
        firstUv[0] + alpha * (secondUv[0] - firstUv[0]),
        firstUv[1] + alpha * (secondUv[1] - firstUv[1])};
}

std::vector<MappedReferenceSample> mapReferenceSamples(
    const RegionReferenceMesh& fittingReference,
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    const std::vector<PatchLookup>& lookups,
    std::size_t& sameFaceBarycentricCount,
    std::size_t& sameFaceFallbackCount,
    std::size_t& crossFaceFailureCount) {
    (void)lookups;
    const auto mapping = mapRegionReferenceSamplesToSurfaceParameters(
        fittingReference, layoutCarrier, layout);
    sameFaceBarycentricCount = mapping.sameFaceBarycentricCount;
    sameFaceFallbackCount = mapping.sameFaceFallbackCount;
    crossFaceFailureCount = mapping.crossFaceFailureCount;
    std::vector<MappedReferenceSample> mapped;
    mapped.reserve(mapping.samples.size());
    for (const auto& sample : mapping.samples) {
        const auto carrierVertex =
            sample.carrierTriangle < layoutCarrier.triangles.size()
            ? layoutCarrier.triangles[sample.carrierTriangle].vertices.front()
            : layoutCarrier.vertices.size();
        mapped.push_back({
            sample.referenceVertex,
            carrierVertex,
            sample.patch,
            normalizedParameter(sample.surfaceParameter)});
    }
    return mapped;
}

std::vector<MappedReferenceSample> selectRepresentativeFitSamples(
    const RegionReferenceMesh& fittingReference,
    const std::vector<MappedReferenceSample>& mappedSamples,
    const std::vector<KnotConfiguration>& knots) {
    std::vector<std::vector<const MappedReferenceSample*>> samplesByPatch(knots.size());
    for (const auto& sample : mappedSamples) {
        if (sample.patch < 0 ||
            static_cast<std::size_t>(sample.patch) >= samplesByPatch.size()) {
            continue;
        }
        samplesByPatch[static_cast<std::size_t>(sample.patch)].push_back(&sample);
    }

    std::vector<MappedReferenceSample> selectedSamples;
    for (std::size_t patch = 0; patch < samplesByPatch.size(); ++patch) {
        auto& samples = samplesByPatch[patch];
        std::sort(samples.begin(), samples.end(), [](const auto* first, const auto* second) {
            return first->referenceVertex < second->referenceVertex;
        });
        const auto controlPointCount = static_cast<std::size_t>(
            knots[patch].uControlCount() * knots[patch].vControlCount());
        // A cubic basis has degree + 1 active functions per parameter axis.
        // Use that polynomial order as the deterministic oversampling factor.
        const auto sampleBudget = std::min(
            samples.size(), controlPointCount * static_cast<std::size_t>(kDegree + 1));
        if (samples.size() <= sampleBudget) {
            for (const auto* sample : samples) selectedSamples.push_back(*sample);
            continue;
        }

        std::vector<std::size_t> selected {0};
        std::vector<double> minimumDistances(
            samples.size(), std::numeric_limits<double>::infinity());
        while (selected.size() < sampleBudget) {
            const auto* last = samples[selected.back()];
            auto next = samples.size();
            auto maximumDistance = -1.0;
            for (std::size_t index = 0; index < samples.size(); ++index) {
                const auto du = samples[index]->uv[0] - last->uv[0];
                const auto dv = samples[index]->uv[1] - last->uv[1];
                minimumDistances[index] = std::min(
                    minimumDistances[index], du * du + dv * dv);
                if (minimumDistances[index] > maximumDistance ||
                    (minimumDistances[index] == maximumDistance &&
                        next < samples.size() &&
                        samples[index]->referenceVertex < samples[next]->referenceVertex)) {
                    maximumDistance = minimumDistances[index];
                    next = index;
                }
            }
            if (next >= samples.size() || maximumDistance <= 1.0e-20) break;
            selected.push_back(next);
        }
        std::sort(selected.begin(), selected.end(), [&](const auto first, const auto second) {
            return samples[first]->referenceVertex < samples[second]->referenceVertex;
        });
        for (const auto index : selected) selectedSamples.push_back(*samples[index]);
    }
    return selectedSamples;
}

Eigen::VectorXd buildInitialControlPrior(
    const RegionReferenceMesh& fittingReference,
    const std::vector<MappedReferenceSample>& representativeSamples,
    const std::vector<KnotConfiguration>& knots,
    const std::vector<int>& variableOffsets,
    const Eigen::VectorXd& columnScales) {
    const auto coupledVariableCount = columnScales.size();
    Eigen::VectorXd prior = Eigen::VectorXd::Zero(coupledVariableCount);
    std::vector<std::vector<const MappedReferenceSample*>> samplesByPatch(knots.size());
    for (const auto& sample : representativeSamples) {
        if (sample.patch >= 0 &&
            static_cast<std::size_t>(sample.patch) < samplesByPatch.size()) {
            samplesByPatch[static_cast<std::size_t>(sample.patch)].push_back(&sample);
        }
    }
    for (std::size_t patch = 0; patch < knots.size(); ++patch) {
        if (samplesByPatch[patch].empty()) continue;
        const auto uParameters = grevilleParameters(knots[patch].u);
        const auto vParameters = grevilleParameters(knots[patch].v);
        for (int u = 0; u < knots[patch].uControlCount(); ++u) {
            for (int v = 0; v < knots[patch].vControlCount(); ++v) {
                const MappedReferenceSample* closest = nullptr;
                auto minimumDistance = std::numeric_limits<double>::infinity();
                auto totalWeight = 0.0;
                gp_XYZ weightedPoint;
                for (const auto* sample : samplesByPatch[patch]) {
                    const auto du = sample->uv[0] - uParameters[static_cast<std::size_t>(u)];
                    const auto dv = sample->uv[1] - vParameters[static_cast<std::size_t>(v)];
                    const auto distance = du * du + dv * dv;
                    if (distance < minimumDistance ||
                        (distance == minimumDistance &&
                            closest != nullptr &&
                            sample->referenceVertex < closest->referenceVertex)) {
                        minimumDistance = distance;
                        closest = sample;
                    }
                    const auto uBasis = basisValues(knots[patch].u, sample->uv[0]);
                    const auto vBasis = basisValues(knots[patch].v, sample->uv[1]);
                    const auto weight =
                        uBasis[static_cast<std::size_t>(u)] *
                        vBasis[static_cast<std::size_t>(v)];
                    if (weight > 0.0) {
                        weightedPoint +=
                            fittingReference.vertices[sample->referenceVertex]
                                .point.XYZ() * weight;
                        totalWeight += weight;
                    }
                }
                if (closest == nullptr) continue;
                const auto point = totalWeight > 0.0
                    ? gp_Pnt(weightedPoint / totalWeight)
                    : fittingReference.vertices[closest->referenceVertex].point;
                const std::array<double, 3> coordinates {
                    point.X(), point.Y(), point.Z()};
                const auto variable = variableOffsets[patch] +
                    u * knots[patch].vControlCount() + v;
                for (int axis = 0; axis < 3; ++axis) {
                    const auto coupled = variable * 3 + axis;
                    prior[coupled] =
                        coordinates[static_cast<std::size_t>(axis)] *
                        columnScales[coupled];
                }
            }
        }
    }
    return prior;
}

std::optional<Eigen::MatrixXd> buildAffineControlBaseline(
    const RegionReferenceMesh& /*fittingReference*/,
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    const std::vector<MappedReferenceSample>& /*representativeSamples*/,
    const std::vector<KnotConfiguration>& knots,
    const std::vector<int>& variableOffsets,
    const int controlVariableCount) {
    Eigen::MatrixXd solution = Eigen::MatrixXd::Zero(controlVariableCount, 3);
    for (std::size_t patch = 0; patch < knots.size(); ++patch) {
        const auto& parameterization = layout.patches[patch];
        if (parameterization.vertexIndices.size() < 3 ||
            surfaceParameters(parameterization).size() !=
                parameterization.vertexIndices.size()) {
            return std::nullopt;
        }
        Eigen::Matrix3d gram = Eigen::Matrix3d::Zero();
        Eigen::Matrix3d targets = Eigen::Matrix3d::Zero();
        std::array<double, 3> minimum {
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity()};
        std::array<double, 3> maximum {
            -std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity()};
        Eigen::Vector2d meanUv = Eigen::Vector2d::Zero();
        Eigen::Vector3d meanPoint = Eigen::Vector3d::Zero();
        for (std::size_t local = 0;
             local < parameterization.vertexIndices.size();
             ++local) {
            const auto vertex = parameterization.vertexIndices[local];
            if (vertex >= layoutCarrier.vertices.size()) return std::nullopt;
            const auto& uv = surfaceParameters(parameterization)[local];
            const Eigen::Vector3d row(1.0, uv[0], uv[1]);
            const auto& point = layoutCarrier.vertices[vertex].point;
            const Eigen::Vector3d target(point.X(), point.Y(), point.Z());
            gram += row * row.transpose();
            targets += row * target.transpose();
            meanUv += Eigen::Vector2d(uv[0], uv[1]);
            meanPoint += target;
            for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
                minimum[axis] = std::min(minimum[axis], target[static_cast<int>(axis)]);
                maximum[axis] = std::max(maximum[axis], target[static_cast<int>(axis)]);
            }
        }
        Eigen::FullPivLU<Eigen::Matrix3d> decomposition(gram);
        if (decomposition.rank() < 3) return std::nullopt;
        const Eigen::Matrix3d coefficients = decomposition.solve(targets);
        if (!coefficients.allFinite()) return std::nullopt;
        meanUv /=
            static_cast<double>(parameterization.vertexIndices.size());
        meanPoint /=
            static_cast<double>(parameterization.vertexIndices.size());

        gp_Vec referenceNormal;
        std::unordered_map<std::size_t, std::size_t> localByGlobal;
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            localByGlobal.emplace(parameterization.vertexIndices[local], local);
        }
        for (const auto triangleIndex : parameterization.triangleIndices) {
            if (triangleIndex >= layoutCarrier.triangles.size()) return std::nullopt;
            const auto& triangle = layoutCarrier.triangles[triangleIndex];
            std::array<std::array<double, 2>, 3> uv;
            for (std::size_t corner = 0; corner < triangle.vertices.size(); ++corner) {
                const auto found = localByGlobal.find(triangle.vertices[corner]);
                if (found == localByGlobal.end()) return std::nullopt;
                uv[corner] =
                    surfaceParameters(parameterization)[found->second];
            }
            const auto signedUvArea =
                (uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1]) -
                (uv[1][1] - uv[0][1]) * (uv[2][0] - uv[0][0]);
            gp_Vec first(
                layoutCarrier.vertices[triangle.vertices[0]].point,
                layoutCarrier.vertices[triangle.vertices[1]].point);
            gp_Vec second(
                layoutCarrier.vertices[triangle.vertices[0]].point,
                layoutCarrier.vertices[triangle.vertices[2]].point);
            auto triangleNormal = first.Crossed(second);
            if (signedUvArea < 0.0) triangleNormal.Reverse();
            referenceNormal += triangleNormal;
        }
        if (referenceNormal.SquareMagnitude() <= 1.0e-24) return std::nullopt;
        referenceNormal.Normalize();

        gp_Vec tangentU(
            coefficients(1, 0), coefficients(1, 1), coefficients(1, 2));
        gp_Vec tangentV(
            coefficients(2, 0), coefficients(2, 1), coefficients(2, 2));
        const auto patchDiagonal = std::sqrt(
            std::pow(maximum[0] - minimum[0], 2.0) +
            std::pow(maximum[1] - minimum[1], 2.0) +
            std::pow(maximum[2] - minimum[2], 2.0));
        if (!std::isfinite(patchDiagonal) || patchDiagonal <= 1.0e-12) {
            return std::nullopt;
        }
        const auto minimumTangentMagnitude = patchDiagonal * 1.0e-4;
        const auto maximumTangentMagnitude = patchDiagonal * 2.0;
        const auto uMagnitude = std::clamp(
            std::sqrt(std::max(
                tangentU.SquareMagnitude(), tangentV.SquareMagnitude())),
            minimumTangentMagnitude,
            maximumTangentMagnitude);
        const auto vMagnitude = std::clamp(
            std::sqrt(std::max(
                tangentV.SquareMagnitude(), tangentU.SquareMagnitude())),
            minimumTangentMagnitude,
            maximumTangentMagnitude);
        tangentU -= referenceNormal * tangentU.Dot(referenceNormal);
        if (tangentU.SquareMagnitude() <= 1.0e-24) {
            const gp_Vec axis =
                std::abs(referenceNormal.X()) < std::abs(referenceNormal.Y())
                ? gp_Vec(1.0, 0.0, 0.0)
                : gp_Vec(0.0, 1.0, 0.0);
            tangentU = referenceNormal.Crossed(axis);
        }
        if (tangentU.SquareMagnitude() <= 1.0e-24) return std::nullopt;
        tangentU.Normalize();
        tangentU *= uMagnitude;
        tangentV = referenceNormal.Crossed(tangentU);
        if (tangentV.SquareMagnitude() <= 1.0e-24) return std::nullopt;
        tangentV.Normalize();
        tangentV *= vMagnitude;

        const auto uParameters = grevilleParameters(knots[patch].u);
        const auto vParameters = grevilleParameters(knots[patch].v);
        for (int u = 0; u < knots[patch].uControlCount(); ++u) {
            for (int v = 0; v < knots[patch].vControlCount(); ++v) {
                const gp_Pnt point(
                    meanPoint.x() +
                        tangentU.X() * (uParameters[static_cast<std::size_t>(u)] - meanUv.x()) +
                        tangentV.X() * (vParameters[static_cast<std::size_t>(v)] - meanUv.y()),
                    meanPoint.y() +
                        tangentU.Y() * (uParameters[static_cast<std::size_t>(u)] - meanUv.x()) +
                        tangentV.Y() * (vParameters[static_cast<std::size_t>(v)] - meanUv.y()),
                    meanPoint.z() +
                        tangentU.Z() * (uParameters[static_cast<std::size_t>(u)] - meanUv.x()) +
                        tangentV.Z() * (vParameters[static_cast<std::size_t>(v)] - meanUv.y()));
                const auto variable = variableOffsets[patch] +
                    u * knots[patch].vControlCount() + v;
                solution(variable, 0) = point.X();
                solution(variable, 1) = point.Y();
                solution(variable, 2) = point.Z();
            }
        }
    }
    return solution;
}

std::vector<std::pair<int, double>> surfaceCoefficients(
    const std::array<double, 2>& uv,
    const KnotConfiguration& knots,
    const int variableOffset) {
    const auto uBasis = basisValues(knots.u, uv[0]);
    const auto vBasis = basisValues(knots.v, uv[1]);
#ifndef NDEBUG
    assert(std::abs(std::accumulate(uBasis.begin(), uBasis.end(), 0.0) - 1.0) < 1.0e-12);
    assert(std::abs(std::accumulate(vBasis.begin(), vBasis.end(), 0.0) - 1.0) < 1.0e-12);
#endif
    const auto vCount = knots.vControlCount();
    std::vector<std::pair<int, double>> result;
    result.reserve(static_cast<std::size_t>((kDegree + 1) * (kDegree + 1)));
    for (int u = 0; u < knots.uControlCount(); ++u) {
        if (uBasis[static_cast<std::size_t>(u)] == 0.0) continue;
        for (int v = 0; v < vCount; ++v) {
            const auto value = uBasis[static_cast<std::size_t>(u)] * vBasis[static_cast<std::size_t>(v)];
            if (value != 0.0) {
                result.emplace_back(variableOffset + u * vCount + v, value);
            }
        }
    }
    return result;
}

std::vector<std::pair<int, double>> surfaceDerivativeCoefficients(
    const std::array<double, 2>& uv,
    const KnotConfiguration& knots,
    const int variableOffset,
    const int axis) {
    constexpr double step = 1.0e-5;
    auto lower = uv;
    auto upper = uv;
    lower[static_cast<std::size_t>(axis)] = std::max(0.0, lower[static_cast<std::size_t>(axis)] - step);
    upper[static_cast<std::size_t>(axis)] = std::min(1.0, upper[static_cast<std::size_t>(axis)] + step);
    const auto denominator = upper[static_cast<std::size_t>(axis)] - lower[static_cast<std::size_t>(axis)];
    const auto lowerCoefficients = surfaceCoefficients(lower, knots, variableOffset);
    const auto upperCoefficients = surfaceCoefficients(upper, knots, variableOffset);
    std::map<int, double> combined;
    for (const auto& [column, value] : lowerCoefficients) combined[column] -= value / denominator;
    for (const auto& [column, value] : upperCoefficients) combined[column] += value / denominator;
    std::vector<std::pair<int, double>> result;
    result.reserve(combined.size());
    for (const auto& [column, value] : combined) {
        if (std::abs(value) > 1.0e-14) result.emplace_back(column, value);
    }
    return result;
}

std::vector<std::pair<int, double>> surfaceDirectionalSecondDerivativeCoefficients(
    const std::array<double, 2>& uv,
    const KnotConfiguration& knots,
    const int variableOffset) {
    auto du = 0.5 - uv[0];
    auto dv = 0.5 - uv[1];
    const auto length = std::hypot(du, dv);
    if (length <= 1.0e-12) return {};
    du /= length;
    dv /= length;
    constexpr double step = 1.0e-3;
    const std::array<double, 2> first {
        std::clamp(uv[0], 0.0, 1.0),
        std::clamp(uv[1], 0.0, 1.0)};
    const std::array<double, 2> second {
        std::clamp(uv[0] + du * step, 0.0, 1.0),
        std::clamp(uv[1] + dv * step, 0.0, 1.0)};
    const std::array<double, 2> third {
        std::clamp(uv[0] + du * step * 2.0, 0.0, 1.0),
        std::clamp(uv[1] + dv * step * 2.0, 0.0, 1.0)};
    if (std::hypot(second[0] - first[0], second[1] - first[1]) <= 1.0e-12 ||
        std::hypot(third[0] - second[0], third[1] - second[1]) <= 1.0e-12) {
        return {};
    }
    std::map<int, double> combined;
    for (const auto& [column, value] :
            surfaceCoefficients(first, knots, variableOffset)) {
        combined[column] += value / (step * step);
    }
    for (const auto& [column, value] :
            surfaceCoefficients(second, knots, variableOffset)) {
        combined[column] -= 2.0 * value / (step * step);
    }
    for (const auto& [column, value] :
            surfaceCoefficients(third, knots, variableOffset)) {
        combined[column] += value / (step * step);
    }
    std::vector<std::pair<int, double>> result;
    result.reserve(combined.size());
    for (const auto& [column, value] : combined) {
        if (std::abs(value) > 1.0e-14) {
            result.emplace_back(column, value);
        }
    }
    return result;
}

std::vector<SeamSample> buildSeamSamples(
    const RegionPatchLayout& layout) {
    std::vector<SeamSample> result;
    for (std::size_t seamIndex = 0; seamIndex < layout.seams.size(); ++seamIndex) {
        const auto& seam = layout.seams[seamIndex];
        std::map<std::size_t, std::size_t> vertexDegrees;
        for (const auto& edge : seam.meshEdges) {
            ++vertexDegrees[edge[0]];
            ++vertexDegrees[edge[1]];
        }
        for (const auto& [vertex, degree] : vertexDegrees) {
            result.push_back({
                static_cast<int>(seamIndex),
                seam.firstPatch,
                seam.secondPatch,
                vertex,
                degree != 2});
        }
    }
    return result;
}

std::vector<SeamSample> selectSeamConstraintSamples(
    const RegionPatchLayout& layout,
    const std::vector<KnotConfiguration>& knots,
    const std::vector<PatchLookup>& lookups,
    const std::vector<SeamSample>& seamSamples) {
    std::map<int, std::vector<const SeamSample*>> bySeam;
    for (const auto& seam : seamSamples) bySeam[seam.seamIndex].push_back(&seam);
    std::vector<SeamSample> selectedSamples;
    for (const auto& [seamIndex, samples] : bySeam) {
        (void)seamIndex;
        if (samples.empty()) continue;
        const auto firstPatch = static_cast<std::size_t>(samples.front()->firstPatch);
        const auto secondPatch = static_cast<std::size_t>(samples.front()->secondPatch);
        const auto maximumControlSide = std::max({
            knots[firstPatch].uControlCount(),
            knots[firstPatch].vControlCount(),
            knots[secondPatch].uControlCount(),
            knots[secondPatch].vControlCount()});
        const auto sampleBudget = std::max<std::size_t>(
            16, static_cast<std::size_t>(maximumControlSide) * 4);
        if (samples.size() <= sampleBudget) {
            for (const auto* sample : samples) selectedSamples.push_back(*sample);
            continue;
        }

        std::vector<std::size_t> selected {0};
        std::vector<double> minimumDistances(samples.size(), std::numeric_limits<double>::infinity());
        while (selected.size() < sampleBudget) {
            const auto selectedSample = samples[selected.back()];
            const auto selectedLocal = lookups[firstPatch].localByGlobal.at(selectedSample->vertex);
            const auto selectedUv = normalizedParameter(
                surfaceParameters(layout.patches[firstPatch])[selectedLocal]);
            auto next = samples.size();
            auto maximumDistance = -1.0;
            for (std::size_t index = 0; index < samples.size(); ++index) {
                const auto local = lookups[firstPatch].localByGlobal.at(samples[index]->vertex);
                const auto uv = normalizedParameter(
                    surfaceParameters(layout.patches[firstPatch])[local]);
                const auto du = uv[0] - selectedUv[0];
                const auto dv = uv[1] - selectedUv[1];
                minimumDistances[index] = std::min(minimumDistances[index], du * du + dv * dv);
                if (minimumDistances[index] > maximumDistance ||
                    (minimumDistances[index] == maximumDistance && next < samples.size() &&
                        samples[index]->vertex < samples[next]->vertex)) {
                    maximumDistance = minimumDistances[index];
                    next = index;
                }
            }
            if (next >= samples.size() || maximumDistance <= 1.0e-20) break;
            selected.push_back(next);
        }
        std::sort(selected.begin(), selected.end());
        for (const auto index : selected) selectedSamples.push_back(*samples[index]);
    }
    return selectedSamples;
}

void appendWeightedRow(
    std::vector<LinearRow>& rows,
    std::vector<std::pair<int, double>> coefficients,
    const gp_Pnt& target,
    const double weight) {
    for (auto& coefficient : coefficients) coefficient.second *= weight;
    rows.push_back({std::move(coefficients), gp_Pnt(
        target.X() * weight, target.Y() * weight, target.Z() * weight)});
}

double positiveMedian(std::vector<double> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](const double value) {
        return !std::isfinite(value) || value <= 0.0;
    }), values.end());
    if (values.empty()) return 1.0;
    const auto middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
    return std::max(values[middle], 1.0e-18);
}

double integrationWeight(const double value, const double median) {
    return std::clamp(std::sqrt(std::max(value, 1.0e-18) / std::max(median, 1.0e-18)), 0.1, 10.0);
}

std::vector<LinearRow> buildSystemRows(
    const RegionReferenceMesh& fittingReference,
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    const std::vector<KnotConfiguration>& knots,
    const std::vector<int>& variableOffsets,
    const std::vector<PatchLookup>& lookups,
    const std::vector<SeamSample>& seamSamples,
    const std::vector<MappedReferenceSample>& mappedSamples,
    const RegionCurveNetwork* curveNetwork,
    const RegionRefitOptions& options,
    std::size_t& seamG1RowCount,
    std::size_t& seamG2RowCount) {
    const auto& mesh = layoutCarrier;
    std::vector<LinearRow> rows;
    rows.reserve(mappedSamples.size() + seamSamples.size() * 3);
    std::vector<double> fittingAreaWeights;
    std::vector<double> fittingBoundaryWeights;
    fittingAreaWeights.reserve(fittingReference.vertices.size());
    fittingBoundaryWeights.reserve(fittingReference.boundarySamples.size());
    for (const auto& vertex : fittingReference.vertices) {
        if (vertex.boundary) {
            fittingBoundaryWeights.push_back(vertex.boundaryLengthWeight);
        } else {
            fittingAreaWeights.push_back(vertex.areaWeight);
        }
    }
    const auto fittingMedianAreaWeight = positiveMedian(std::move(fittingAreaWeights));
    const auto fittingMedianBoundaryWeight = positiveMedian(
        std::move(fittingBoundaryWeights));
    for (const auto& sample : mappedSamples) {
        const auto& vertex = fittingReference.vertices[sample.referenceVertex];
        appendWeightedRow(
            rows,
            surfaceCoefficients(
                sample.uv,
                knots[static_cast<std::size_t>(sample.patch)],
                variableOffsets[static_cast<std::size_t>(sample.patch)]),
            vertex.point,
            vertex.boundary
                ? integrationWeight(
                    vertex.boundaryLengthWeight,
                    fittingMedianBoundaryWeight)
                : integrationWeight(vertex.areaWeight, fittingMedianAreaWeight));
    }
    for (const auto& seam : seamSamples) {
        const auto firstFound = lookups[static_cast<std::size_t>(seam.firstPatch)].localByGlobal.find(seam.vertex);
        const auto secondFound = lookups[static_cast<std::size_t>(seam.secondPatch)].localByGlobal.find(seam.vertex);
        if (firstFound == lookups[static_cast<std::size_t>(seam.firstPatch)].localByGlobal.end() ||
            secondFound == lookups[static_cast<std::size_t>(seam.secondPatch)].localByGlobal.end()) {
            continue;
        }
        const auto firstUv = normalizedParameter(
            surfaceParameters(
                layout.patches[static_cast<std::size_t>(seam.firstPatch)])
                    [firstFound->second]);
        const auto secondUv = normalizedParameter(
            surfaceParameters(
                layout.patches[static_cast<std::size_t>(seam.secondPatch)])
                    [secondFound->second]);
        const RegionCurveNetworkEdge* frozenEdge = nullptr;
        if (curveNetwork != nullptr) {
            const auto found = std::find_if(
                curveNetwork->edges.begin(), curveNetwork->edges.end(),
                [&](const auto& edge) {
                    return !edge.originalBoundary &&
                        std::min(edge.firstPatch, edge.secondPatch) ==
                            std::min(seam.firstPatch, seam.secondPatch) &&
                        std::max(edge.firstPatch, edge.secondPatch) ==
                            std::max(seam.firstPatch, seam.secondPatch) &&
                        std::find(
                            edge.meshVertices.begin(),
                            edge.meshVertices.end(),
                            seam.vertex) != edge.meshVertices.end();
                });
            if (found != curveNetwork->edges.end()) {
                frozenEdge = &*found;
            }
        }
        auto coefficients = surfaceCoefficients(
            firstUv,
            knots[static_cast<std::size_t>(seam.firstPatch)],
            variableOffsets[static_cast<std::size_t>(seam.firstPatch)]);
        const auto secondCoefficients = surfaceCoefficients(
            secondUv,
            knots[static_cast<std::size_t>(seam.secondPatch)],
            variableOffsets[static_cast<std::size_t>(seam.secondPatch)]);
        coefficients.reserve(coefficients.size() + secondCoefficients.size());
        for (const auto& [column, value] : secondCoefficients) {
            coefficients.emplace_back(column, -value);
        }
        // This is the statically condensed form of two equal-weight equations
        // that share one latent seam point. It preserves the shared-variable G0
        // objective without adding one solver variable per sampled seam vertex.
        appendWeightedRow(rows, std::move(coefficients), gp_Pnt(0.0, 0.0, 0.0), 100.0 / std::sqrt(2.0));

        auto referenceCoefficients = surfaceCoefficients(
            firstUv,
            knots[static_cast<std::size_t>(seam.firstPatch)],
            variableOffsets[static_cast<std::size_t>(seam.firstPatch)]);
        for (auto& coefficient : referenceCoefficients) coefficient.second *= 0.5;
        for (const auto& [column, value] : secondCoefficients) {
            referenceCoefficients.emplace_back(column, value * 0.5);
        }
        appendWeightedRow(
            rows,
            std::move(referenceCoefficients),
            mesh.vertices[seam.vertex].point,
            10.0);

        const auto inwardDerivative = [&](const std::array<double, 2>& uv,
                                           const KnotConfiguration& patchKnots,
                                           const int offset) {
            auto du = 0.5 - uv[0];
            auto dv = 0.5 - uv[1];
            const auto length = std::hypot(du, dv);
            if (length <= 1.0e-12) {
                return std::vector<std::pair<int, double>> {};
            }
            du /= length;
            dv /= length;
            std::map<int, double> combined;
            for (const auto& [column, value] :
                    surfaceDerivativeCoefficients(uv, patchKnots, offset, 0)) {
                combined[column] += value * du;
            }
            for (const auto& [column, value] :
                    surfaceDerivativeCoefficients(uv, patchKnots, offset, 1)) {
                combined[column] += value * dv;
            }
            std::vector<std::pair<int, double>> derivative;
            derivative.reserve(combined.size());
            for (const auto& [column, value] : combined) {
                if (std::abs(value) > 1.0e-14) derivative.emplace_back(column, value);
            }
            return derivative;
        };
        auto firstDerivative = inwardDerivative(
            firstUv,
            knots[static_cast<std::size_t>(seam.firstPatch)],
            variableOffsets[static_cast<std::size_t>(seam.firstPatch)]);
        auto secondDerivative = inwardDerivative(
            secondUv,
            knots[static_cast<std::size_t>(seam.secondPatch)],
            variableOffsets[static_cast<std::size_t>(seam.secondPatch)]);
        const auto requireG1 = frozenEdge == nullptr ||
            frozenEdge->continuity == RegionCurveContinuity::G1 ||
            frozenEdge->continuity == RegionCurveContinuity::G2;
        if (requireG1 &&
            !firstDerivative.empty() && !secondDerivative.empty()) {
            firstDerivative.insert(
                firstDerivative.end(), secondDerivative.begin(), secondDerivative.end());
            appendWeightedRow(
                rows,
                std::move(firstDerivative),
                gp_Pnt(0.0, 0.0, 0.0),
                options.seamG1RelativeWeight);
            ++seamG1RowCount;
        }
        if (frozenEdge != nullptr &&
            frozenEdge->continuity == RegionCurveContinuity::G2) {
            auto firstSecondDerivative =
                surfaceDirectionalSecondDerivativeCoefficients(
                    firstUv,
                    knots[static_cast<std::size_t>(seam.firstPatch)],
                    variableOffsets[static_cast<std::size_t>(seam.firstPatch)]);
            auto secondSecondDerivative =
                surfaceDirectionalSecondDerivativeCoefficients(
                    secondUv,
                    knots[static_cast<std::size_t>(seam.secondPatch)],
                    variableOffsets[static_cast<std::size_t>(seam.secondPatch)]);
            if (!firstSecondDerivative.empty() &&
                !secondSecondDerivative.empty()) {
                for (auto& coefficient : secondSecondDerivative) {
                    coefficient.second = -coefficient.second;
                }
                firstSecondDerivative.insert(
                    firstSecondDerivative.end(),
                    secondSecondDerivative.begin(),
                    secondSecondDerivative.end());
                appendWeightedRow(
                    rows,
                    std::move(firstSecondDerivative),
                    gp_Pnt(0.0, 0.0, 0.0),
                    options.seamG2RelativeWeight);
                ++seamG2RowCount;
            }
        }
    }
    constexpr double fairnessWeight = 1.0;
    for (int patch = 0; patch < layout.patchCount; ++patch) {
        const auto& patchKnots = knots[static_cast<std::size_t>(patch)];
        const auto uCount = patchKnots.uControlCount();
        const auto vCount = patchKnots.vControlCount();
        const auto offset = variableOffsets[static_cast<std::size_t>(patch)];
        for (int u = 1; u + 1 < uCount; ++u) {
            for (int v = 0; v < vCount; ++v) {
                appendWeightedRow(rows, {
                    {offset + (u - 1) * vCount + v, 1.0},
                    {offset + u * vCount + v, -2.0},
                    {offset + (u + 1) * vCount + v, 1.0}},
                    gp_Pnt(0.0, 0.0, 0.0), fairnessWeight);
            }
        }
        for (int u = 0; u < uCount; ++u) {
            for (int v = 1; v + 1 < vCount; ++v) {
                appendWeightedRow(rows, {
                    {offset + u * vCount + v - 1, 1.0},
                    {offset + u * vCount + v, -2.0},
                    {offset + u * vCount + v + 1, 1.0}},
                    gp_Pnt(0.0, 0.0, 0.0), fairnessWeight);
            }
        }
    }
    return rows;
}

std::vector<NormalRow> buildNormalRows(
    const RegionReferenceMesh& fittingReference,
    const RegionReferenceMesh& layoutCarrier,
    const RegionTopologyConstraints& constraints,
    const RegionPatchLayout& layout,
    const std::vector<KnotConfiguration>& knots,
    const std::vector<int>& variableOffsets,
    const std::vector<MappedReferenceSample>& mappedSamples,
    std::size_t& adjacentNormalRowCount) {
    const auto& mesh = layoutCarrier;
    std::vector<NormalRow> rows;
    rows.reserve(mappedSamples.size() * 2);
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        const auto& parameterization = layout.patches[patch];
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            const auto vertexIndex = parameterization.vertexIndices[local];
            const auto& vertex = mesh.vertices[vertexIndex];
            const RegionBoundaryEdgeConstraint* smoothConstraint = nullptr;
            if (vertex.boundary) {
                for (const auto edgeId : vertex.boundaryEdgeIds) {
                    const auto found = std::find_if(
                        constraints.boundaryEdges.begin(),
                        constraints.boundaryEdges.end(),
                        [&](const auto& constraint) {
                            return constraint.edgeId == edgeId &&
                                constraint.softAdjacentNormalConstraint;
                        });
                    if (found != constraints.boundaryEdges.end()) {
                        smoothConstraint = &*found;
                        break;
                    }
                }
            }
            if (smoothConstraint == nullptr) continue;
            const auto uv = normalizedParameter(
                surfaceParameters(parameterization)[local]);
            constexpr double adjacentNormalWeight = 1.0;
            auto targetNormal = vertex.normal;
            if (smoothConstraint != nullptr &&
                !smoothConstraint->adjacentFace.IsNull() &&
                !smoothConstraint->adjacentPcurve.IsNull()) {
                try {
                    double first = 0.0;
                    double last = 0.0;
                    const auto curve = BRep_Tool::Curve(
                        smoothConstraint->originalEdge, first, last);
                    GeomAPI_ProjectPointOnCurve projection(
                        vertex.point, curve, first, last);
                    if (!curve.IsNull() && projection.NbPoints() > 0 && last > first) {
                        const auto alpha = std::clamp(
                            (projection.LowerDistanceParameter() - first) /
                                (last - first),
                            0.0,
                            1.0);
                        const auto adjacentParameter =
                            smoothConstraint->adjacentFirstParameter +
                            alpha * (smoothConstraint->adjacentLastParameter -
                                smoothConstraint->adjacentFirstParameter);
                        const auto adjacentUv =
                            smoothConstraint->adjacentPcurve->Value(adjacentParameter);
                        const auto adjacentSurface =
                            BRep_Tool::Surface(smoothConstraint->adjacentFace);
                        gp_Pnt adjacentPoint;
                        gp_Vec adjacentU;
                        gp_Vec adjacentV;
                        adjacentSurface->D1(
                            adjacentUv.X(), adjacentUv.Y(),
                            adjacentPoint, adjacentU, adjacentV);
                        const auto normal = adjacentU.Crossed(adjacentV);
                        if (normal.SquareMagnitude() > 1.0e-24) {
                            targetNormal = gp_Dir(normal);
                        }
                    }
                } catch (const Standard_Failure&) {
                }
            }
            for (int axis = 0; axis < 2; ++axis) {
                auto coefficients = surfaceDerivativeCoefficients(
                    uv,
                    knots[patch],
                    variableOffsets[patch],
                    axis);
                for (auto& coefficient : coefficients) {
                    coefficient.second *= adjacentNormalWeight;
                }
                rows.push_back({
                    std::move(coefficients),
                    targetNormal});
                ++adjacentNormalRowCount;
            }
        }
    }
    std::vector<double> areaWeights;
    areaWeights.reserve(fittingReference.vertices.size());
    for (const auto& vertex : fittingReference.vertices) {
        if (!vertex.boundary) areaWeights.push_back(vertex.areaWeight);
    }
    const auto medianAreaWeight = positiveMedian(std::move(areaWeights));
    for (const auto& sample : mappedSamples) {
        const auto& vertex = fittingReference.vertices[sample.referenceVertex];
        if (vertex.boundary) continue;
        const auto weight = integrationWeight(vertex.areaWeight, medianAreaWeight);
        for (int axis = 0; axis < 2; ++axis) {
            auto coefficients = surfaceDerivativeCoefficients(
                sample.uv,
                knots[static_cast<std::size_t>(sample.patch)],
                variableOffsets[static_cast<std::size_t>(sample.patch)],
                axis);
            for (auto& coefficient : coefficients) coefficient.second *= weight;
            rows.push_back({std::move(coefficients), vertex.normal});
        }
    }
    return rows;
}

std::vector<LinearRow> buildBoundaryEqualityRows(
    const RegionReferenceMesh& layoutCarrier,
    const RegionTopologyConstraints& constraints,
    const RegionPatchLayout& layout,
    const std::vector<KnotConfiguration>& knots,
    const std::vector<int>& variableOffsets) {
    std::vector<LinearRow> rows;
    if (constraints.boundaryEdges.empty()) return rows;
    std::unordered_map<EdgeId, const RegionBoundaryEdgeConstraint*> constraintByEdge;
    for (const auto& constraint : constraints.boundaryEdges) {
        if (constraint.hardPositionConstraint && !constraint.originalEdge.IsNull()) {
            constraintByEdge.emplace(constraint.edgeId, &constraint);
        }
    }
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        const auto& parameterization = layout.patches[patch];
        std::unordered_map<std::size_t, std::size_t> localByGlobal;
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            localByGlobal.emplace(parameterization.vertexIndices[local], local);
        }
        const auto targetOnOriginalEdge = [&](const EdgeId edgeId,
                                               const gp_Pnt& reference,
                                               gp_Pnt& target,
                                               double& parameter,
                                               double& normalizedEdgeParameter) {
            const auto found = constraintByEdge.find(edgeId);
            if (found == constraintByEdge.end()) return false;
            double first = 0.0;
            double last = 0.0;
            const auto curve = BRep_Tool::Curve(found->second->originalEdge, first, last);
            if (curve.IsNull() || last <= first) return false;
            GeomAPI_ProjectPointOnCurve projection(reference, curve, first, last);
            if (projection.NbPoints() == 0) return false;
            parameter = projection.LowerDistanceParameter();
            normalizedEdgeParameter = std::clamp(
                (parameter - first) / (last - first), 0.0, 1.0);
            target = curve->Value(parameter);
            return true;
        };
        std::set<std::pair<std::size_t, EdgeId>> constrainedVertices;
        for (std::size_t position = 0; position < parameterization.boundaryLoop.size(); ++position) {
            const auto vertexIndex = parameterization.boundaryLoop[position];
            const auto& vertex = layoutCarrier.vertices[vertexIndex];
            const auto local = localByGlobal.at(vertexIndex);
            for (const auto edgeId : vertex.boundaryEdgeIds) {
                gp_Pnt target;
                double parameter = 0.0;
                double normalizedEdgeParameter = 0.0;
                if (!constrainedVertices.emplace(vertexIndex, edgeId).second ||
                    !targetOnOriginalEdge(
                        edgeId,
                        vertex.point,
                        target,
                        parameter,
                        normalizedEdgeParameter)) {
                    continue;
                }
                rows.push_back({
                    surfaceCoefficients(
                        normalizedParameter(
                            surfaceParameters(parameterization)[local]),
                        knots[patch],
                        variableOffsets[patch]),
                    target,
                    static_cast<std::uint64_t>(edgeId),
                    normalizedEdgeParameter,
                    vertex.boundaryEdgeIds.size() > 1 ||
                        normalizedEdgeParameter <= 1.0e-10 ||
                        normalizedEdgeParameter >= 1.0 - 1.0e-10});
            }

            const auto nextVertexIndex = parameterization.boundaryLoop[
                (position + 1) % parameterization.boundaryLoop.size()];
            const auto& nextVertex = layoutCarrier.vertices[nextVertexIndex];
            std::vector<EdgeId> sharedEdgeIds;
            for (const auto edgeId : vertex.boundaryEdgeIds) {
                if (std::find(
                        nextVertex.boundaryEdgeIds.begin(),
                        nextVertex.boundaryEdgeIds.end(),
                        edgeId) != nextVertex.boundaryEdgeIds.end()) {
                    sharedEdgeIds.push_back(edgeId);
                }
            }
            std::sort(sharedEdgeIds.begin(), sharedEdgeIds.end());
            sharedEdgeIds.erase(
                std::unique(sharedEdgeIds.begin(), sharedEdgeIds.end()),
                sharedEdgeIds.end());
            const auto nextLocal = localByGlobal.at(nextVertexIndex);
            for (const auto edgeId : sharedEdgeIds) {
                gp_Pnt firstTarget;
                gp_Pnt secondTarget;
                double firstParameter = 0.0;
                double secondParameter = 0.0;
                double firstNormalizedParameter = 0.0;
                double secondNormalizedParameter = 0.0;
                if (!targetOnOriginalEdge(
                        edgeId,
                        vertex.point,
                        firstTarget,
                        firstParameter,
                        firstNormalizedParameter) ||
                    !targetOnOriginalEdge(
                        edgeId,
                        nextVertex.point,
                        secondTarget,
                        secondParameter,
                        secondNormalizedParameter)) {
                    continue;
                }
                const auto found = constraintByEdge.find(edgeId);
                double first = 0.0;
                double last = 0.0;
                const auto curve = BRep_Tool::Curve(found->second->originalEdge, first, last);
                const auto target = curve->Value(
                    std::clamp((firstParameter + secondParameter) * 0.5, first, last));
                const auto firstUv = normalizedParameter(
                    surfaceParameters(parameterization)[local]);
                const auto secondUv = normalizedParameter(
                    surfaceParameters(parameterization)[nextLocal]);
                rows.push_back({
                    surfaceCoefficients(
                        {(firstUv[0] + secondUv[0]) * 0.5,
                         (firstUv[1] + secondUv[1]) * 0.5},
                        knots[patch],
                        variableOffsets[patch]),
                    target,
                    static_cast<std::uint64_t>(edgeId),
                    (firstNormalizedParameter + secondNormalizedParameter) * 0.5,
                    false});
            }
        }
    }
    return rows;
}

std::vector<LinearRow> buildFrozenCurveEqualityRows(
    const RegionPatchLayout& layout,
    const RegionCurveNetwork& network,
    const std::vector<KnotConfiguration>& knots,
    const std::vector<int>& variableOffsets) {
    constexpr std::uint64_t internalCurveConstraintGroupMask =
        std::uint64_t {1} << 62;
    constexpr std::uint64_t boundaryCurveConstraintGroupMask =
        std::uint64_t {1} << 61;
    std::vector<LinearRow> rows;
    for (const auto& edge : network.edges) {
        if (edge.curve.IsNull() || edge.firstPatch < 0 ||
            (!edge.originalBoundary && edge.secondPatch < 0)) {
            continue;
        }
        auto maximumControlSide = std::max(
            knots[static_cast<std::size_t>(edge.firstPatch)].uControlCount(),
            knots[static_cast<std::size_t>(edge.firstPatch)].vControlCount());
        if (!edge.originalBoundary) {
            maximumControlSide = std::max({
                maximumControlSide,
                knots[static_cast<std::size_t>(edge.secondPatch)].uControlCount(),
                knots[static_cast<std::size_t>(edge.secondPatch)].vControlCount()});
        }
        const auto sampleCount = std::max(16, maximumControlSide * 4);
        std::vector<double> parameters;
        parameters.reserve(
            static_cast<std::size_t>(sampleCount) +
            edge.meshParameters.size());
        for (int sample = 0; sample < sampleCount; ++sample) {
            parameters.push_back(edge.firstParameter +
                static_cast<double>(sample) /
                    static_cast<double>(sampleCount - 1) *
                (edge.lastParameter - edge.firstParameter));
        }
        parameters.insert(
            parameters.end(),
            edge.meshParameters.begin(),
            edge.meshParameters.end());
        std::sort(parameters.begin(), parameters.end());
        parameters.erase(
            std::unique(
                parameters.begin(), parameters.end(),
                [](const auto first, const auto second) {
                    return std::abs(first - second) <= 1.0e-12;
                }),
            parameters.end());
        for (std::size_t sample = 0; sample < parameters.size(); ++sample) {
            const auto parameter = parameters[sample];
            const auto alpha = std::clamp(
                (parameter - edge.firstParameter) /
                    (edge.lastParameter - edge.firstParameter),
                0.0,
                1.0);
            const auto target = edge.curve->Value(parameter);
            const std::array<int, 2> patches {
                edge.firstPatch, edge.secondPatch};
            const auto patchCount = edge.originalBoundary ? 1 : 2;
            for (int side = 0; side < patchCount; ++side) {
                const auto patch = patches[static_cast<std::size_t>(side)];
                const auto uv = curveParameterOnPatch(
                    layout, edge, patch, parameter);
                if (!uv.has_value()) continue;
                rows.push_back({
                    surfaceCoefficients(
                        *uv,
                        knots[static_cast<std::size_t>(patch)],
                        variableOffsets[static_cast<std::size_t>(patch)]),
                    target,
                    (edge.originalBoundary
                        ? boundaryCurveConstraintGroupMask
                        : internalCurveConstraintGroupMask) |
                        static_cast<std::uint64_t>(edge.id),
                    alpha,
                    sample == 0 || sample + 1 == parameters.size() ||
                        std::binary_search(
                            edge.meshParameters.begin(),
                            edge.meshParameters.end(),
                            parameter),
                    patch,
                    *uv,
                    edge.originalBoundary});
            }
        }
    }
    return rows;
}

std::vector<LinearRow> buildSeamEqualityRows(
    const RegionPatchLayout& layout,
    const std::vector<KnotConfiguration>& knots,
    const std::vector<int>& variableOffsets,
    const std::vector<PatchLookup>& lookups,
    const std::vector<SeamSample>& seamSamples) {
    constexpr std::uint64_t seamConstraintGroupMask = std::uint64_t {1} << 63;
    std::vector<LinearRow> rows;
    std::set<int> controlConstrainedSeams;
    const auto matchingSide = [&](const int patch, const std::set<std::size_t>& seamVertices) {
        const auto& parameterization = layout.patches[static_cast<std::size_t>(patch)];
        for (std::size_t side = 0; side < parameterization.boundarySides.size(); ++side) {
            const std::set<std::size_t> sideVertices(
                parameterization.boundarySides[side].begin(),
                parameterization.boundarySides[side].end());
            if (sideVertices == seamVertices) return static_cast<int>(side);
        }
        return -1;
    };
    const auto sideControls = [&](const int patch, const int side) {
        const auto& patchKnots = knots[static_cast<std::size_t>(patch)];
        const auto uCount = patchKnots.uControlCount();
        const auto vCount = patchKnots.vControlCount();
        const auto offset = variableOffsets[static_cast<std::size_t>(patch)];
        const auto count = side % 2 == 0 ? uCount : vCount;
        std::vector<int> variables;
        variables.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            switch (side) {
            case 0: variables.push_back(offset + index * vCount); break;
            case 1: variables.push_back(offset + (uCount - 1) * vCount + index); break;
            case 2: variables.push_back(offset + (uCount - 1 - index) * vCount + vCount - 1); break;
            default: variables.push_back(offset + vCount - 1 - index); break;
            }
        }
        return variables;
    };
    for (std::size_t seamIndex = 0; seamIndex < layout.seams.size(); ++seamIndex) {
        const auto& seam = layout.seams[seamIndex];
        std::set<std::size_t> vertices;
        for (const auto& edge : seam.meshEdges) {
            vertices.insert(edge[0]);
            vertices.insert(edge[1]);
        }
        const auto firstSide = matchingSide(seam.firstPatch, vertices);
        const auto secondSide = matchingSide(seam.secondPatch, vertices);
        if (firstSide < 0 || secondSide < 0) continue;
        auto firstControls = sideControls(seam.firstPatch, firstSide);
        auto secondControls = sideControls(seam.secondPatch, secondSide);
        if (firstControls.size() != secondControls.size()) continue;
        const auto& firstVertices = layout.patches[static_cast<std::size_t>(seam.firstPatch)]
            .boundarySides[static_cast<std::size_t>(firstSide)];
        const auto& secondVertices = layout.patches[static_cast<std::size_t>(seam.secondPatch)]
            .boundarySides[static_cast<std::size_t>(secondSide)];
        if (firstVertices.front() == secondVertices.back()) {
            std::reverse(secondControls.begin(), secondControls.end());
        } else if (firstVertices.front() != secondVertices.front()) {
            continue;
        }
        for (std::size_t index = 0; index < firstControls.size(); ++index) {
            rows.push_back({
                {{firstControls[index], 1.0}, {secondControls[index], -1.0}},
                gp_Pnt(0.0, 0.0, 0.0),
                seamConstraintGroupMask | seamIndex,
                firstControls.size() > 1
                    ? static_cast<double>(index) /
                        static_cast<double>(firstControls.size() - 1)
                    : 0.0,
                index == 0 || index + 1 == firstControls.size()});
        }
        controlConstrainedSeams.insert(static_cast<int>(seamIndex));
    }
    std::map<std::size_t, std::vector<int>> parents;
    for (const auto& seam : seamSamples) {
        if (controlConstrainedSeams.contains(seam.seamIndex)) continue;
        const auto firstPatch = static_cast<std::size_t>(seam.firstPatch);
        const auto secondPatch = static_cast<std::size_t>(seam.secondPatch);
        const auto firstFound = lookups[firstPatch].localByGlobal.find(seam.vertex);
        const auto secondFound = lookups[secondPatch].localByGlobal.find(seam.vertex);
        if (firstFound == lookups[firstPatch].localByGlobal.end() ||
            secondFound == lookups[secondPatch].localByGlobal.end()) {
            continue;
        }
        auto [parentFound, inserted] = parents.try_emplace(seam.vertex, layout.patchCount);
        auto& parent = parentFound->second;
        if (inserted) std::iota(parent.begin(), parent.end(), 0);
        const auto findRoot = [&](int patch) {
            while (parent[static_cast<std::size_t>(patch)] != patch) {
                patch = parent[static_cast<std::size_t>(patch)];
            }
            return patch;
        };
        const auto firstRoot = findRoot(seam.firstPatch);
        const auto secondRoot = findRoot(seam.secondPatch);
        if (firstRoot == secondRoot) continue;
        parent[static_cast<std::size_t>(secondRoot)] = firstRoot;

        auto coefficients = surfaceCoefficients(
            normalizedParameter(
                surfaceParameters(layout.patches[firstPatch])
                    [firstFound->second]),
            knots[firstPatch],
            variableOffsets[firstPatch]);
        const auto secondCoefficients = surfaceCoefficients(
            normalizedParameter(
                surfaceParameters(layout.patches[secondPatch])
                    [secondFound->second]),
            knots[secondPatch],
            variableOffsets[secondPatch]);
        for (const auto& [column, value] : secondCoefficients) {
            coefficients.emplace_back(column, -value);
        }
        const auto firstUv = normalizedParameter(
            surfaceParameters(layout.patches[firstPatch])
                [firstFound->second]);
        rows.push_back({
            std::move(coefficients),
            gp_Pnt(0.0, 0.0, 0.0),
            seamConstraintGroupMask | static_cast<std::uint64_t>(seam.seamIndex),
            (firstUv[0] + firstUv[1]) * 0.5,
            seam.requiredConstraintPoint});
    }
    return rows;
}

double constraintCoefficientDistance(
    const LinearRow& first,
    const LinearRow& second) {
    std::map<int, double> difference;
    for (const auto& [column, value] : first.coefficients) difference[column] += value;
    for (const auto& [column, value] : second.coefficients) difference[column] -= value;
    auto squaredDistance = 0.0;
    for (const auto& [column, value] : difference) {
        (void)column;
        squaredDistance += value * value;
    }
    return squaredDistance;
}

std::vector<LinearRow> compressConstraintRows(
    const std::vector<LinearRow>& rows,
    const std::size_t samplesPerCurve) {
    std::map<std::uint64_t, std::vector<LinearRow>> rowsByGroup;
    for (const auto& row : rows) {
        auto& group = rowsByGroup[row.constraintGroup];
        const auto duplicate = std::find_if(
            group.begin(), group.end(), [&](const auto& existing) {
                return existing.coefficients == row.coefficients &&
                    existing.target.X() == row.target.X() &&
                    existing.target.Y() == row.target.Y() &&
                    existing.target.Z() == row.target.Z();
            });
        if (duplicate == group.end()) {
            group.push_back(row);
        } else {
            duplicate->requiredConstraintPoint =
                duplicate->requiredConstraintPoint || row.requiredConstraintPoint;
            duplicate->constraintParameter = std::min(
                duplicate->constraintParameter, row.constraintParameter);
        }
    }

    std::vector<LinearRow> result;
    for (auto& [groupId, group] : rowsByGroup) {
        (void)groupId;
        std::stable_sort(group.begin(), group.end(), [](const auto& first, const auto& second) {
            return first.constraintParameter < second.constraintParameter;
        });
        if (group.size() <= samplesPerCurve) {
            result.insert(result.end(), group.begin(), group.end());
            continue;
        }

        std::vector<std::size_t> selected;
        for (std::size_t index = 0; index < group.size(); ++index) {
            if (group[index].requiredConstraintPoint) selected.push_back(index);
        }
        if (selected.empty()) selected.push_back(0);
        std::sort(selected.begin(), selected.end());
        selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
        const auto selectedBudget = std::max(samplesPerCurve, selected.size());
        std::vector<double> minimumDistances(
            group.size(), std::numeric_limits<double>::infinity());
        while (selected.size() < selectedBudget) {
            auto next = group.size();
            auto maximumDistance = -1.0;
            for (std::size_t index = 0; index < group.size(); ++index) {
                if (std::find(selected.begin(), selected.end(), index) != selected.end()) {
                    minimumDistances[index] = 0.0;
                    continue;
                }
                for (const auto selectedIndex : selected) {
                    minimumDistances[index] = std::min(
                        minimumDistances[index],
                        constraintCoefficientDistance(group[index], group[selectedIndex]));
                }
                if (minimumDistances[index] > maximumDistance) {
                    maximumDistance = minimumDistances[index];
                    next = index;
                }
            }
            if (next >= group.size() || maximumDistance <= 1.0e-24) break;
            selected.push_back(next);
        }
        std::sort(selected.begin(), selected.end());
        for (const auto index : selected) result.push_back(group[index]);
    }
    return result;
}

struct ConstraintFeasibility {
    std::size_t estimatedRank = 0;
    double minimumMaximumResidual = 0.0;
    Eigen::MatrixXd solution;
    std::vector<ResidualSample> residuals;
};

ConstraintFeasibility assessConstraintFeasibility(
    const std::vector<LinearRow>& rows,
    const int controlVariableCount) {
    ConstraintFeasibility result;
    if (rows.empty()) return result;
    std::vector<Eigen::Triplet<double>> triplets;
    std::size_t nonZeros = 0;
    for (const auto& row : rows) nonZeros += row.coefficients.size();
    triplets.reserve(nonZeros);
    Eigen::MatrixXd targets(static_cast<int>(rows.size()), 3);
    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        for (const auto& [column, value] : rows[rowIndex].coefficients) {
            triplets.emplace_back(static_cast<int>(rowIndex), column, value);
        }
        targets(static_cast<int>(rowIndex), 0) = rows[rowIndex].target.X();
        targets(static_cast<int>(rowIndex), 1) = rows[rowIndex].target.Y();
        targets(static_cast<int>(rowIndex), 2) = rows[rowIndex].target.Z();
    }
    Eigen::SparseMatrix<double> matrix(
        static_cast<int>(rows.size()), controlVariableCount);
    matrix.setFromTriplets(triplets.begin(), triplets.end());
    Eigen::SparseQR<
        Eigen::SparseMatrix<double>,
        Eigen::COLAMDOrdering<int>> solver;
    solver.setPivotThreshold(std::sqrt(std::numeric_limits<double>::epsilon()));
    solver.compute(matrix);
    if (solver.info() != Eigen::Success) return result;
    result.estimatedRank = static_cast<std::size_t>(solver.rank());
    const Eigen::MatrixXd solution = solver.solve(targets);
    if (solver.info() != Eigen::Success || !solution.allFinite()) return result;
    result.solution = solution;
    const Eigen::MatrixXd residuals = matrix * solution - targets;
    result.minimumMaximumResidual = residuals.cwiseAbs().maxCoeff();
    result.residuals.reserve(rows.size());
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (rows[row].sourcePatch < 0) continue;
        result.residuals.push_back({
            rows[row].sourcePatch,
            rows[row].sourceUv[0],
            rows[row].sourceUv[1],
            residuals.row(static_cast<int>(row)).cwiseAbs().maxCoeff()});
    }
    return result;
}

std::optional<Eigen::MatrixXd> projectOntoConstraints(
    const std::vector<LinearRow>& rows,
    const int controlVariableCount,
    const Eigen::MatrixXd& reference) {
    if (rows.empty()) return reference;
    if (reference.rows() != controlVariableCount || reference.cols() != 3 ||
        !reference.allFinite()) {
        return std::nullopt;
    }
    const auto rowCount = static_cast<int>(rows.size());
    const auto systemSize = controlVariableCount + rowCount;
    std::vector<Eigen::Triplet<double>> triplets;
    std::size_t nonZeros = static_cast<std::size_t>(controlVariableCount);
    for (const auto& row : rows) nonZeros += row.coefficients.size() * 2;
    triplets.reserve(nonZeros);
    for (int variable = 0; variable < controlVariableCount; ++variable) {
        triplets.emplace_back(variable, variable, 1.0);
    }
    for (int row = 0; row < rowCount; ++row) {
        for (const auto& [column, value] :
             rows[static_cast<std::size_t>(row)].coefficients) {
            triplets.emplace_back(
                column, controlVariableCount + row, value);
            triplets.emplace_back(
                controlVariableCount + row, column, value);
        }
    }
    Eigen::SparseMatrix<double> system(systemSize, systemSize);
    system.setFromTriplets(triplets.begin(), triplets.end());
    system.makeCompressed();
    Eigen::SparseLU<Eigen::SparseMatrix<double>> solver;
    solver.analyzePattern(system);
    solver.factorize(system);
    if (solver.info() != Eigen::Success) return std::nullopt;
    Eigen::MatrixXd targets = Eigen::MatrixXd::Zero(systemSize, 3);
    targets.topRows(controlVariableCount) = reference;
    for (int row = 0; row < rowCount; ++row) {
        const auto& target = rows[static_cast<std::size_t>(row)].target;
        targets(controlVariableCount + row, 0) = target.X();
        targets(controlVariableCount + row, 1) = target.Y();
        targets(controlVariableCount + row, 2) = target.Z();
    }
    const auto solution = solver.solve(targets);
    if (solver.info() != Eigen::Success || !solution.allFinite()) {
        return std::nullopt;
    }
    return solution.topRows(controlVariableCount);
}

Handle(Geom_BSplineSurface) makeSurface(
    const KnotConfiguration& knots,
    const Eigen::MatrixXd& solution,
    const int variableOffset) {
    const auto uCount = knots.uControlCount();
    const auto vCount = knots.vControlCount();
    TColgp_Array2OfPnt poles(1, uCount, 1, vCount);
    for (int u = 0; u < uCount; ++u) {
        for (int v = 0; v < vCount; ++v) {
            const auto variable = variableOffset + u * vCount + v;
            poles.SetValue(u + 1, v + 1, gp_Pnt(
                solution(variable, 0), solution(variable, 1), solution(variable, 2)));
        }
    }
    TColStd_Array1OfReal uKnots(1, static_cast<int>(knots.u.size()));
    TColStd_Array1OfInteger uMultiplicities(1, static_cast<int>(knots.u.size()));
    for (int index = 1; index <= uKnots.Length(); ++index) {
        uKnots.SetValue(index, knots.u[static_cast<std::size_t>(index - 1)]);
        uMultiplicities.SetValue(index, index == 1 || index == uKnots.Length() ? kDegree + 1 : 1);
    }
    TColStd_Array1OfReal vKnots(1, static_cast<int>(knots.v.size()));
    TColStd_Array1OfInteger vMultiplicities(1, static_cast<int>(knots.v.size()));
    for (int index = 1; index <= vKnots.Length(); ++index) {
        vKnots.SetValue(index, knots.v[static_cast<std::size_t>(index - 1)]);
        vMultiplicities.SetValue(index, index == 1 || index == vKnots.Length() ? kDegree + 1 : 1);
    }
    return new Geom_BSplineSurface(
        poles,
        uKnots,
        vKnots,
        uMultiplicities,
        vMultiplicities,
        kDegree,
        kDegree,
        false,
        false);
}

bool evaluateLocalSupportDomain(
    const Handle(Geom_BSplineSurface)& surface,
    const RegionReferenceMesh& carrier,
    const RegionPatchParameterization& parameterization,
    const RegionRefitOptions& options,
    std::string& reason) {
    if (surface.IsNull() ||
        parameterization.vertexIndices.empty() ||
        parameterization.vertexIndices.size() !=
            surfaceParameters(parameterization).size()) {
        reason = "A local support domain has incomplete surface or parameterization data.";
        return false;
    }
    std::array<double, 3> minimum {
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    std::array<double, 3> maximum {
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    std::unordered_map<std::size_t, std::size_t> localByGlobal;
    localByGlobal.reserve(parameterization.vertexIndices.size());
    for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
        const auto vertexIndex = parameterization.vertexIndices[local];
        if (vertexIndex >= carrier.vertices.size() ||
            !std::isfinite(surfaceParameters(parameterization)[local][0]) ||
            !std::isfinite(surfaceParameters(parameterization)[local][1])) {
            reason = "A local support domain contains non-finite or invalid parameterization data.";
            return false;
        }
        localByGlobal.emplace(vertexIndex, local);
        const auto& vertex = carrier.vertices[vertexIndex];
        const std::array<double, 3> coordinate {vertex.point.X(), vertex.point.Y(), vertex.point.Z()};
        if (!std::all_of(coordinate.begin(), coordinate.end(), [](const auto value) {
                return std::isfinite(value);
            })) {
            reason = "A local support domain contains a non-finite reference point.";
            return false;
        }
        for (std::size_t axis = 0; axis < coordinate.size(); ++axis) {
            minimum[axis] = std::min(minimum[axis], coordinate[axis]);
            maximum[axis] = std::max(maximum[axis], coordinate[axis]);
        }
    }
    const auto diagonal = std::sqrt(
        std::pow(maximum[0] - minimum[0], 2.0) +
        std::pow(maximum[1] - minimum[1], 2.0) +
        std::pow(maximum[2] - minimum[2], 2.0));
    const auto margin = std::max(options.tolerance3d * 5.0, diagonal * 0.1);
    for (int u = 1; u <= surface->NbUPoles(); ++u) {
        for (int v = 1; v <= surface->NbVPoles(); ++v) {
            const auto pole = surface->Pole(u, v);
            if (!std::isfinite(pole.X()) ||
                !std::isfinite(pole.Y()) ||
                !std::isfinite(pole.Z())) {
                reason = "A fitted control net contains a non-finite pole.";
                return false;
            }
            if (parameterization.edgePcurves.empty() &&
                (pole.X() < minimum[0] - margin || pole.X() > maximum[0] + margin ||
                    pole.Y() < minimum[1] - margin || pole.Y() > maximum[1] + margin ||
                    pole.Z() < minimum[2] - margin || pole.Z() > maximum[2] + margin)) {
                reason = "A fitted control net expands beyond its local support-domain envelope.";
                return false;
            }
        }
    }

    using ParameterPoint = std::array<double, 2>;
    struct TrimTriangle {
        std::array<ParameterPoint, 3> parameters;
        gp_Vec referenceNormal;
        std::array<std::size_t, 3> vertices;
    };
    std::vector<TrimTriangle> trimTriangles;
    trimTriangles.reserve(parameterization.triangleIndices.size());
    gp_Vec patchReferenceNormal;
    for (const auto triangleIndex : parameterization.triangleIndices) {
        if (triangleIndex >= carrier.triangles.size()) {
            reason = "A local support domain references an invalid trim triangle.";
            return false;
        }
        const auto& triangle = carrier.triangles[triangleIndex];
        TrimTriangle trimTriangle;
        trimTriangle.vertices = triangle.vertices;
        for (std::size_t corner = 0; corner < triangle.vertices.size(); ++corner) {
            const auto found = localByGlobal.find(triangle.vertices[corner]);
            if (found == localByGlobal.end()) {
                reason = "A trim triangle references a vertex outside its patch parameterization.";
                return false;
            }
            trimTriangle.parameters[corner] =
                surfaceParameters(parameterization)[found->second];
        }
        const auto signedUvArea =
            (trimTriangle.parameters[1][0] - trimTriangle.parameters[0][0]) *
                (trimTriangle.parameters[2][1] - trimTriangle.parameters[0][1]) -
            (trimTriangle.parameters[1][1] - trimTriangle.parameters[0][1]) *
                (trimTriangle.parameters[2][0] - trimTriangle.parameters[0][0]);
        if (!std::isfinite(signedUvArea) || std::abs(signedUvArea) <= 1.0e-14) {
            reason = "A local support domain contains a degenerate trim triangle.";
            return false;
        }
        gp_Vec first(
            carrier.vertices[triangle.vertices[0]].point,
            carrier.vertices[triangle.vertices[1]].point);
        gp_Vec second(
            carrier.vertices[triangle.vertices[0]].point,
            carrier.vertices[triangle.vertices[2]].point);
        trimTriangle.referenceNormal = first.Crossed(second);
        if (signedUvArea < 0.0) trimTriangle.referenceNormal.Reverse();
        if (trimTriangle.referenceNormal.SquareMagnitude() <= 1.0e-24) {
            reason = "A local support domain has no stable trim-triangle reference normal.";
            return false;
        }
        patchReferenceNormal += trimTriangle.referenceNormal;
        trimTriangles.push_back(std::move(trimTriangle));
    }
    if (trimTriangles.empty()) {
        reason = "A local support domain has no trim triangles for its Jacobian gate.";
        return false;
    }
    if (patchReferenceNormal.SquareMagnitude() <= 1.0e-24) {
        reason = "A local support domain has no stable patch reference normal.";
        return false;
    }
    for (auto& triangle : trimTriangles) {
        triangle.referenceNormal = patchReferenceNormal;
    }

    constexpr double minimumOrientationCosine =
        1.4901161193847656e-8; // sqrt(machine epsilon)
    const auto evaluateJacobian = [&](const ParameterPoint& parameter,
                                      const gp_Vec& referenceNormal) {
        gp_Pnt point;
        gp_Vec derivativeU;
        gp_Vec derivativeV;
        surface->D1(parameter[0], parameter[1], point, derivativeU, derivativeV);
        const std::array<double, 9> values {
            point.X(), point.Y(), point.Z(),
            derivativeU.X(), derivativeU.Y(), derivativeU.Z(),
            derivativeV.X(), derivativeV.Y(), derivativeV.Z()};
        if (!std::all_of(values.begin(), values.end(), [](const auto value) {
                return std::isfinite(value);
            })) {
            reason = "A fitted local support domain has non-finite trim-domain derivatives.";
            return false;
        }
        const auto jacobian = derivativeU.Crossed(derivativeV);
        if (jacobian.SquareMagnitude() <= 1.0e-20) {
            reason = "A fitted local support domain has a collapsed Jacobian inside its trim domain.";
            return false;
        }
        const auto orientationScale = std::sqrt(
            jacobian.SquareMagnitude() * referenceNormal.SquareMagnitude());
        const auto orientationCosine = jacobian.Dot(referenceNormal) / orientationScale;
        if (!std::isfinite(orientationCosine) ||
            std::abs(orientationCosine) <= minimumOrientationCosine) {
            reason = "A fitted local support domain has an unstable Jacobian orientation inside its trim domain.";
            return false;
        }
        if (orientationCosine < 0.0) {
            reason = "A fitted local support domain contains a Jacobian flip inside its trim domain.";
            return false;
        }
        return true;
    };

    const auto barycentricResolution = std::max(
        surface->UDegree() + surface->VDegree() + 1,
        surface->NbUKnots() + surface->NbVKnots() - 1);
    for (const auto& triangle : trimTriangles) {
        for (int firstWeight = 0; firstWeight <= barycentricResolution; ++firstWeight) {
            for (int secondWeight = 0;
                 secondWeight <= barycentricResolution - firstWeight;
                 ++secondWeight) {
                const auto first =
                    static_cast<double>(firstWeight) / barycentricResolution;
                const auto second =
                    static_cast<double>(secondWeight) / barycentricResolution;
                const auto third = 1.0 - first - second;
                const ParameterPoint parameter {
                    first * triangle.parameters[0][0] +
                        second * triangle.parameters[1][0] +
                        third * triangle.parameters[2][0],
                    first * triangle.parameters[0][1] +
                        second * triangle.parameters[1][1] +
                        third * triangle.parameters[2][1]};
                if (!evaluateJacobian(parameter, triangle.referenceNormal)) return false;
            }
        }
    }

    std::map<std::array<std::size_t, 2>, const TrimTriangle*> boundaryTriangles;
    for (const auto& triangle : trimTriangles) {
        for (std::size_t edge = 0; edge < triangle.vertices.size(); ++edge) {
            const std::array<std::size_t, 2> key {
                std::min(
                    triangle.vertices[edge],
                    triangle.vertices[(edge + 1) % triangle.vertices.size()]),
                std::max(
                    triangle.vertices[edge],
                    triangle.vertices[(edge + 1) % triangle.vertices.size()])};
            boundaryTriangles.emplace(key, &triangle);
        }
    }
    constexpr double safetyBandFractionOfSampleSpacing = 0.25;
    const auto safetyBandBarycentricOffset =
        safetyBandFractionOfSampleSpacing / barycentricResolution;
    for (std::size_t boundary = 0;
         boundary < parameterization.boundaryLoop.size();
         ++boundary) {
        const auto firstVertex = parameterization.boundaryLoop[boundary];
        const auto secondVertex = parameterization.boundaryLoop[
            (boundary + 1) % parameterization.boundaryLoop.size()];
        const std::array<std::size_t, 2> key {
            std::min(firstVertex, secondVertex),
            std::max(firstVertex, secondVertex)};
        const auto found = boundaryTriangles.find(key);
        if (found == boundaryTriangles.end()) {
            reason = "A trim boundary edge has no incident parameterization triangle.";
            return false;
        }
        const auto& triangle = *found->second;
        std::size_t firstCorner = 0;
        std::size_t secondCorner = 0;
        std::size_t interiorCorner = 0;
        for (std::size_t corner = 0; corner < triangle.vertices.size(); ++corner) {
            if (triangle.vertices[corner] == firstVertex) firstCorner = corner;
            else if (triangle.vertices[corner] == secondVertex) secondCorner = corner;
            else interiorCorner = corner;
        }
        for (int sample = 0; sample <= barycentricResolution; ++sample) {
            const auto edgeWeight =
                static_cast<double>(sample) / barycentricResolution;
            const auto expandedWeight = 1.0 + safetyBandBarycentricOffset;
            const ParameterPoint parameter {
                expandedWeight * (
                    (1.0 - edgeWeight) * triangle.parameters[firstCorner][0] +
                    edgeWeight * triangle.parameters[secondCorner][0]) -
                    safetyBandBarycentricOffset *
                        triangle.parameters[interiorCorner][0],
                expandedWeight * (
                    (1.0 - edgeWeight) * triangle.parameters[firstCorner][1] +
                    edgeWeight * triangle.parameters[secondCorner][1]) -
                    safetyBandBarycentricOffset *
                        triangle.parameters[interiorCorner][1]};
            if (!evaluateJacobian(parameter, triangle.referenceNormal)) return false;
        }
    }
    return true;
}

struct ConstructibleBlendResult {
    bool available = false;
    double alpha = 0.0;
    Eigen::MatrixXd solution;
    std::vector<Handle(Geom_BSplineSurface)> surfaces;
    std::string failure;
};

ConstructibleBlendResult largestConstructibleBlend(
    const Eigen::MatrixXd& baseline,
    const Eigen::MatrixXd& target,
    const std::vector<KnotConfiguration>& knots,
    const std::vector<int>& variableOffsets,
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    const RegionRefitOptions& options) {
    ConstructibleBlendResult result;
    if (baseline.rows() != target.rows() ||
        baseline.cols() != 3 ||
        target.cols() != 3 ||
        !baseline.allFinite() ||
        !target.allFinite()) {
        result.failure = "Constructibility line search received an invalid control network.";
        return result;
    }
    const auto evaluate = [&](const Eigen::MatrixXd& solution,
                              std::vector<Handle(Geom_BSplineSurface)>& surfaces,
                              std::string& failure) {
        surfaces.clear();
        surfaces.reserve(static_cast<std::size_t>(layout.patchCount));
        try {
            for (int patch = 0; patch < layout.patchCount; ++patch) {
                surfaces.push_back(makeSurface(
                    knots[static_cast<std::size_t>(patch)],
                    solution,
                    variableOffsets[static_cast<std::size_t>(patch)]));
            }
            for (std::size_t patch = 0; patch < surfaces.size(); ++patch) {
                if (!evaluateLocalSupportDomain(
                        surfaces[patch],
                        layoutCarrier,
                        layout.patches[patch],
                        options,
                        failure)) {
                    return false;
                }
            }
        } catch (const Standard_Failure&) {
            failure = "Open CASCADE rejected a constructibility line-search surface.";
            return false;
        }
        return true;
    };

    if (!evaluate(baseline, result.surfaces, result.failure)) return result;
    result.available = true;
    result.solution = baseline;
    std::vector<Handle(Geom_BSplineSurface)> targetSurfaces;
    std::string targetFailure;
    if (evaluate(target, targetSurfaces, targetFailure)) {
        result.alpha = 1.0;
        result.solution = target;
        result.surfaces = std::move(targetSurfaces);
        return result;
    }
    result.failure = targetFailure;
    auto lower = 0.0;
    auto upper = 1.0;
    for (int iteration = 0;
         iteration < std::numeric_limits<float>::digits;
         ++iteration) {
        const auto alpha = (lower + upper) * 0.5;
        const Eigen::MatrixXd candidate =
            baseline + alpha * (target - baseline);
        std::vector<Handle(Geom_BSplineSurface)> candidateSurfaces;
        std::string candidateFailure;
        if (evaluate(candidate, candidateSurfaces, candidateFailure)) {
            lower = alpha;
            result.alpha = alpha;
            result.solution = candidate;
            result.surfaces = std::move(candidateSurfaces);
        } else {
            upper = alpha;
            result.failure = candidateFailure;
        }
    }
    return result;
}

double normalAngleDegrees(const gp_Vec& first, const gp_Dir& second) {
    if (first.SquareMagnitude() <= 1.0e-24) return 180.0;
    const auto dot = std::clamp(std::abs(gp_Dir(first).Dot(second)), 0.0, 1.0);
    return std::acos(dot) * 180.0 / kPi;
}

struct SurfaceProjection {
    gp_Pnt point;
    gp_Vec du;
    gp_Vec dv;
    double u = 0.0;
    double v = 0.0;
    double distance = std::numeric_limits<double>::infinity();
    std::size_t iterationCount = 0;
    std::size_t fallbackCount = 0;
};

SurfaceProjection projectToSurface(
    const Handle(Geom_BSplineSurface)& surface,
    const gp_Pnt& target,
    const std::array<double, 2>& initial) {
    SurfaceProjection best;
    best.u = initial[0];
    best.v = initial[1];
    surface->D1(best.u, best.v, best.point, best.du, best.dv);
    best.distance = best.point.Distance(target);
    for (int iteration = 0; iteration < 12; ++iteration) {
        ++best.iterationCount;
        const gp_Vec residual(target, best.point);
        const auto first = best.du.SquareMagnitude();
        const auto mixed = best.du.Dot(best.dv);
        const auto second = best.dv.SquareMagnitude();
        const auto determinant = first * second - mixed * mixed;
        if (determinant <= 1.0e-24) break;
        auto deltaU = (-second * residual.Dot(best.du) + mixed * residual.Dot(best.dv)) / determinant;
        auto deltaV = (mixed * residual.Dot(best.du) - first * residual.Dot(best.dv)) / determinant;
        const auto maximumStep = std::max(std::abs(deltaU), std::abs(deltaV));
        if (maximumStep > 0.25) {
            deltaU *= 0.25 / maximumStep;
            deltaV *= 0.25 / maximumStep;
        }
        bool accepted = false;
        for (int lineSearch = 0; lineSearch < 8; ++lineSearch) {
            if (lineSearch > 0) ++best.fallbackCount;
            const auto candidateU = std::clamp(best.u + deltaU, 0.0, 1.0);
            const auto candidateV = std::clamp(best.v + deltaV, 0.0, 1.0);
            gp_Pnt candidatePoint;
            gp_Vec candidateDu;
            gp_Vec candidateDv;
            surface->D1(candidateU, candidateV, candidatePoint, candidateDu, candidateDv);
            const auto candidateDistance = candidatePoint.Distance(target);
            if (candidateDistance < best.distance - 1.0e-12) {
                best.point = candidatePoint;
                best.du = candidateDu;
                best.dv = candidateDv;
                best.u = candidateU;
                best.v = candidateV;
                best.distance = candidateDistance;
                accepted = true;
                break;
            }
            deltaU *= 0.5;
            deltaV *= 0.5;
        }
        if (!accepted || std::max(std::abs(deltaU), std::abs(deltaV)) < 1.0e-10) break;
    }
    return best;
}

ValidationMetrics validateNetwork(
    const RegionReferenceMesh& fittingReference,
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    const std::vector<PatchLookup>& lookups,
    const std::vector<Handle(Geom_BSplineSurface)>& surfaces,
    const RegionRefitOptions& options,
    const std::vector<MappedReferenceSample>& mappedSamples) {
    ValidationMetrics metrics;
    for (const auto& sample : mappedSamples) {
        const auto patch = static_cast<std::size_t>(sample.patch);
        const auto vertexIndex = sample.referenceVertex;
        const auto& vertex = fittingReference.vertices[vertexIndex];
        const auto uv = sample.uv;
        const auto bucket = static_cast<int>((vertexIndex * 2654435761ull + patch * 40503ull) % 10ull);
        if (!vertex.boundary && bucket < 7) {
            metrics.trainingMaximumDistance = std::max(
                metrics.trainingMaximumDistance,
                surfaces[patch]->Value(uv[0], uv[1]).Distance(vertex.point));
            continue;
        }
        const auto projection = projectToSurface(surfaces[patch], vertex.point, uv);
        ++metrics.projectionCount;
        metrics.uvIterationCount += projection.iterationCount;
        metrics.projectionFallbackCount += projection.fallbackCount;
        if (vertex.boundary) continue;
        const auto distance = projection.distance;
        if (bucket < 9) {
            const auto fittedNormal = projection.du.Crossed(projection.dv);
            const auto normalDegrees = normalAngleDegrees(fittedNormal, vertex.normal);
            const auto score = std::max(
                distance,
                options.tolerance3d * normalDegrees /
                    std::max(options.normalToleranceDegrees, 1.0e-9));
            metrics.selectionMaximumDistance = std::max(metrics.selectionMaximumDistance, distance);
            metrics.selectionMaximumScore = std::max(metrics.selectionMaximumScore, score);
            metrics.selectionResiduals.push_back({static_cast<int>(patch), uv[0], uv[1], score});
        } else {
            metrics.holdoutDistances.push_back(distance);
            if (distance > metrics.holdoutMaximumDistance) {
                metrics.holdoutMaximumDistance = distance;
                metrics.holdoutWorstDistanceVertex = vertexIndex;
                metrics.holdoutWorstDistancePatch = static_cast<int>(patch);
                metrics.holdoutWorstDistanceInternalSeam = vertex.internalSeam;
            }
            if (!vertex.internalSeam) {
                const auto normalDegrees = normalAngleDegrees(
                    projection.du.Crossed(projection.dv), vertex.normal);
                metrics.holdoutNormalDegrees.push_back(normalDegrees);
                if (normalDegrees > metrics.holdoutMaximumNormalDegrees) {
                    metrics.holdoutMaximumNormalDegrees = normalDegrees;
                    metrics.holdoutWorstNormalVertex = vertexIndex;
                    metrics.holdoutWorstNormalPatch = static_cast<int>(patch);
                    metrics.holdoutWorstNormalInternalSeam = vertex.internalSeam;
                }
            }
        }
    }
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        const auto& parameterization = layout.patches[patch];
        const auto& surfaceParameters =
            parameterization.surfaceParameters.size() ==
                    parameterization.vertexIndices.size()
                ? parameterization.surfaceParameters
                : parameterization.parameters;
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            const auto vertexIndex = parameterization.vertexIndices[local];
            const auto& vertex = layoutCarrier.vertices[vertexIndex];
            const auto uv = normalizedParameter(surfaceParameters[local]);
            if (!vertex.boundary) continue;
            const auto projection = projectToSurface(surfaces[patch], vertex.point, uv);
            ++metrics.projectionCount;
            metrics.uvIterationCount += projection.iterationCount;
            metrics.projectionFallbackCount += projection.fallbackCount;
            metrics.boundaryMaximumDistance = std::max(
                metrics.boundaryMaximumDistance, projection.distance);
        }
    }
    for (const auto& seam : layout.seams) {
        std::set<std::size_t> vertices;
        for (const auto& edge : seam.meshEdges) {
            vertices.insert(edge[0]);
            vertices.insert(edge[1]);
        }
        for (const auto vertex : vertices) {
            const auto firstLocal = lookups[static_cast<std::size_t>(seam.firstPatch)].localByGlobal.at(vertex);
            const auto secondLocal = lookups[static_cast<std::size_t>(seam.secondPatch)].localByGlobal.at(vertex);
            const auto& firstParameterization =
                layout.patches[static_cast<std::size_t>(seam.firstPatch)];
            const auto& secondParameterization =
                layout.patches[static_cast<std::size_t>(seam.secondPatch)];
            const auto& firstParameters =
                firstParameterization.surfaceParameters.size() ==
                        firstParameterization.vertexIndices.size()
                    ? firstParameterization.surfaceParameters
                    : firstParameterization.parameters;
            const auto& secondParameters =
                secondParameterization.surfaceParameters.size() ==
                        secondParameterization.vertexIndices.size()
                    ? secondParameterization.surfaceParameters
                    : secondParameterization.parameters;
            const auto firstUv = normalizedParameter(
                firstParameters[firstLocal]);
            const auto secondUv = normalizedParameter(
                secondParameters[secondLocal]);
            metrics.seamMaximumDistance = std::max(
                metrics.seamMaximumDistance,
                surfaces[static_cast<std::size_t>(seam.firstPatch)]->Value(firstUv[0], firstUv[1]).Distance(
                    surfaces[static_cast<std::size_t>(seam.secondPatch)]->Value(secondUv[0], secondUv[1])));
        }
    }
    const auto percentile95 = [](std::vector<double> values) {
        if (values.empty()) return 0.0;
        const auto index = std::min(
            values.size() - 1,
            static_cast<std::size_t>(std::ceil(static_cast<double>(values.size()) * 0.95)) - 1);
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
        return values[index];
    };
    metrics.holdout95Distance = percentile95(metrics.holdoutDistances);
    metrics.holdout95NormalDegrees = percentile95(metrics.holdoutNormalDegrees);
    return metrics;
}

void validateFrozenCurveContinuity(
    ValidationMetrics& metrics,
    const RegionPatchLayout& layout,
    const RegionCurveNetwork& network,
    const std::vector<Handle(Geom_BSplineSurface)>& surfaces,
    const RegionRefitOptions& options) {
    metrics.seamContinuityGatePassed = true;
    const auto sampleCount = std::max(97, options.curveValidationSampleCount);
    for (const auto& edge : network.edges) {
        if (edge.originalBoundary || edge.curve.IsNull() ||
            edge.firstPatch < 0 || edge.secondPatch < 0 ||
            edge.firstPatch >= static_cast<int>(surfaces.size()) ||
            edge.secondPatch >= static_cast<int>(surfaces.size())) {
            continue;
        }
        for (int sample = 0; sample < sampleCount; ++sample) {
            const auto alpha = static_cast<double>(sample) /
                static_cast<double>(sampleCount - 1);
            const auto parameter = edge.firstParameter +
                alpha * (edge.lastParameter - edge.firstParameter);
            const auto curvePoint = edge.curve->Value(parameter);
            const auto firstUv = curveParameterOnPatch(
                layout, edge, edge.firstPatch, parameter);
            const auto secondUv = curveParameterOnPatch(
                layout, edge, edge.secondPatch, parameter);
            if (!firstUv.has_value() || !secondUv.has_value()) {
                metrics.seamContinuityGatePassed = false;
                metrics.seamMaximumCurveDeviation =
                    std::numeric_limits<double>::infinity();
                continue;
            }
            gp_Pnt firstPoint;
            gp_Pnt secondPoint;
            gp_Vec firstDu;
            gp_Vec firstDv;
            gp_Vec secondDu;
            gp_Vec secondDv;
            surfaces[static_cast<std::size_t>(edge.firstPatch)]->D1(
                (*firstUv)[0], (*firstUv)[1],
                firstPoint, firstDu, firstDv);
            surfaces[static_cast<std::size_t>(edge.secondPatch)]->D1(
                (*secondUv)[0], (*secondUv)[1],
                secondPoint, secondDu, secondDv);
            metrics.seamMaximumCurveDeviation = std::max({
                metrics.seamMaximumCurveDeviation,
                firstPoint.Distance(curvePoint),
                secondPoint.Distance(curvePoint)});
            metrics.seamMaximumDistance = std::max(
                metrics.seamMaximumDistance,
                firstPoint.Distance(secondPoint));
            const auto firstNormal = firstDu.Crossed(firstDv);
            const auto secondNormal = secondDu.Crossed(secondDv);
            const auto normalDegrees = normalAngleDegrees(
                firstNormal,
                secondNormal.SquareMagnitude() > 1.0e-24
                    ? gp_Dir(secondNormal)
                    : gp_Dir(0.0, 0.0, 1.0));
            metrics.seamMaximumNormalDegrees = std::max(
                metrics.seamMaximumNormalDegrees, normalDegrees);
            if (edge.continuity == RegionCurveContinuity::G2) {
                try {
                    GeomLProp_SLProps firstProperties(
                        surfaces[static_cast<std::size_t>(edge.firstPatch)],
                        (*firstUv)[0],
                        (*firstUv)[1],
                        2,
                        1.0e-9);
                    GeomLProp_SLProps secondProperties(
                        surfaces[static_cast<std::size_t>(edge.secondPatch)],
                        (*secondUv)[0],
                        (*secondUv)[1],
                        2,
                        1.0e-9);
                    if (!firstProperties.IsCurvatureDefined() ||
                        !secondProperties.IsCurvatureDefined()) {
                        metrics.seamMaximumCurvatureDifference =
                            std::numeric_limits<double>::infinity();
                    } else {
                        metrics.seamMaximumCurvatureDifference = std::max(
                            metrics.seamMaximumCurvatureDifference,
                            std::abs(
                                firstProperties.MeanCurvature() -
                                secondProperties.MeanCurvature()));
                    }
                } catch (const Standard_Failure&) {
                    metrics.seamMaximumCurvatureDifference =
                        std::numeric_limits<double>::infinity();
                }
            }
        }
        const auto positionPassed =
            metrics.seamMaximumCurveDeviation <=
                options.curveOnSurfaceTolerance &&
            metrics.seamMaximumDistance <= options.curveOnSurfaceTolerance;
        const auto g1Passed =
            edge.continuity == RegionCurveContinuity::G0 ||
            metrics.seamMaximumNormalDegrees <= options.g1ToleranceDegrees;
        const auto g2Passed =
            edge.continuity != RegionCurveContinuity::G2 ||
            metrics.seamMaximumCurvatureDifference <=
                options.g2CurvatureTolerance;
        metrics.seamContinuityGatePassed =
            metrics.seamContinuityGatePassed &&
            positionPassed && g1Passed && g2Passed;
    }
}

bool insertResidualKnot(
    const std::vector<ResidualSample>& residuals,
    std::vector<KnotConfiguration>& knots,
    const double minimumError,
    const int maximumControlPointsPerSide) {
    if (residuals.empty()) return false;
    const auto median = [](std::vector<double> values) {
        const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
        std::nth_element(values.begin(), middle, values.end());
        return *middle;
    };
    const auto insert = [maximumControlPointsPerSide](std::vector<double>& target, double value) {
        if (static_cast<int>(target.size()) + kDegree - 1 >= maximumControlPointsPerSide) {
            return false;
        }
        value = std::clamp(value, 0.02, 0.98);
        const auto duplicate = std::any_of(target.begin(), target.end(), [&](const auto knot) {
            return std::abs(knot - value) < 0.01;
        });
        if (duplicate) {
            auto largest = 0.0;
            for (std::size_t index = 0; index + 1 < target.size(); ++index) {
                const auto span = target[index + 1] - target[index];
                if (span > largest) {
                    largest = span;
                    value = (target[index + 1] + target[index]) * 0.5;
                }
            }
            if (largest < 0.02) return false;
        }
        target.push_back(value);
        std::sort(target.begin(), target.end());
        return true;
    };
    std::map<int, std::vector<ResidualSample>> byPatch;
    for (const auto& sample : residuals) byPatch[sample.patch].push_back(sample);
    std::vector<std::pair<double, int>> rankedPatches;
    for (const auto& [patch, samples] : byPatch) {
        const auto maximum = std::max_element(samples.begin(), samples.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.error < rhs.error;
        })->error;
        if (maximum > minimumError) rankedPatches.emplace_back(maximum, patch);
    }
    std::sort(rankedPatches.begin(), rankedPatches.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first != rhs.first ? lhs.first > rhs.first : lhs.second < rhs.second;
    });
    if (rankedPatches.size() > 3) rankedPatches.resize(3);

    std::vector<double> requestedKnots;
    for (const auto& [error, targetPatch] : rankedPatches) {
        (void)error;
        auto sorted = byPatch[targetPatch];
        std::sort(sorted.begin(), sorted.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.error > rhs.error;
        });
        sorted.resize(std::max<std::size_t>(1, sorted.size() / 10));
        std::vector<double> u;
        std::vector<double> v;
        u.reserve(sorted.size());
        v.reserve(sorted.size());
        for (const auto& sample : sorted) {
            u.push_back(sample.u);
            v.push_back(sample.v);
        }
        const auto uMedian = median(u);
        const auto vMedian = median(v);
        requestedKnots.push_back(uMedian);
        requestedKnots.push_back(vMedian);
    }
    bool insertedAny = false;
    for (const auto value : requestedKnots) {
        auto familyInserted = false;
        for (auto& patchKnots : knots) {
            const auto insertedU = insert(patchKnots.u, value);
            const auto insertedV = insert(patchKnots.v, value);
            familyInserted = familyInserted || insertedU || insertedV;
        }
        insertedAny = insertedAny || familyInserted;
    }
    return insertedAny;
}

bool insertBoundaryFeasibilityKnots(
    const std::vector<ResidualSample>& residuals,
    std::vector<KnotConfiguration>& knots,
    const int maximumControlPointsPerSide,
    const double minimumError) {
    std::map<int, ResidualSample> worstByPatch;
    for (const auto& sample : residuals) {
        if (sample.patch < 0 ||
            sample.patch >= static_cast<int>(knots.size()) ||
            sample.error <= minimumError) {
            continue;
        }
        const auto found = worstByPatch.find(sample.patch);
        if (found == worstByPatch.end() ||
            sample.error > found->second.error) {
            worstByPatch[sample.patch] = sample;
        }
    }
    const auto insert = [maximumControlPointsPerSide](
                            std::vector<double>& target,
                            double value) {
        if (static_cast<int>(target.size()) + kDegree - 1 >=
            maximumControlPointsPerSide) {
            return false;
        }
        value = std::clamp(value, 0.02, 0.98);
        if (std::any_of(
                target.begin(), target.end(),
                [&](const auto knot) {
                    return std::abs(knot - value) < 0.005;
                })) {
            auto largest = 0.0;
            for (std::size_t index = 0; index + 1 < target.size(); ++index) {
                const auto span = target[index + 1] - target[index];
                if (span > largest) {
                    largest = span;
                    value = (target[index + 1] + target[index]) * 0.5;
                }
            }
            if (largest < 0.01) return false;
        }
        target.push_back(value);
        std::sort(target.begin(), target.end());
        return true;
    };
    auto insertedAny = false;
    for (const auto& [patch, worst] : worstByPatch) {
        auto& patchKnots = knots[static_cast<std::size_t>(patch)];
        const auto insertedU = insert(patchKnots.u, worst.u);
        const auto insertedV = insert(patchKnots.v, worst.v);
        insertedAny = insertedAny || insertedU || insertedV;
    }
    return insertedAny;
}

std::vector<double> uniformUniqueKnots(const int controlCount) {
    const auto uniqueCount = controlCount - kDegree + 1;
    std::vector<double> knots(static_cast<std::size_t>(uniqueCount), 0.0);
    for (int index = 0; index < uniqueCount; ++index) {
        knots[static_cast<std::size_t>(index)] =
            static_cast<double>(index) /
            static_cast<double>(uniqueCount - 1);
    }
    return knots;
}

struct LogicalSideTargetSegment {
    const RegionCurveNetworkEdge* edge = nullptr;
    double sideFirst = 0.0;
    double sideLast = 1.0;
    bool forward = true;
};

struct LogicalSideTarget {
    std::string key;
    std::vector<LogicalSideTargetSegment> segments;
    bool originalBoundary = false;
    bool reverseForPatch = false;

    gp_Pnt value(const double sideParameter) const {
        const auto clamped = std::clamp(sideParameter, 0.0, 1.0);
        const auto found = std::find_if(
            segments.begin(), segments.end(),
            [&](const auto& segment) {
                return clamped <= segment.sideLast + 1.0e-14;
            });
        const auto& segment =
            found == segments.end() ? segments.back() : *found;
        const auto alpha = segment.sideLast > segment.sideFirst
            ? std::clamp(
                (clamped - segment.sideFirst) /
                    (segment.sideLast - segment.sideFirst),
                0.0,
                1.0)
            : 0.0;
        const auto curveAlpha = segment.forward ? alpha : 1.0 - alpha;
        const auto parameter =
            segment.edge->firstParameter +
            curveAlpha *
                (segment.edge->lastParameter -
                 segment.edge->firstParameter);
        return segment.edge->curve->Value(parameter);
    }
};

std::optional<LogicalSideTarget> logicalSideTarget(
    const RegionPatchLayout& layout,
    const RegionCurveNetwork& network,
    const int patch,
    const std::size_t sideIndex) {
    if (patch < 0 || patch >= layout.patchCount ||
        sideIndex >= 4) {
        return std::nullopt;
    }
    const auto loop = std::find_if(
        network.patchLoops.begin(), network.patchLoops.end(),
        [&](const auto& candidate) { return candidate.patch == patch; });
    if (loop == network.patchLoops.end()) return std::nullopt;
    const auto& parameterization =
        layout.patches[static_cast<std::size_t>(patch)];
    const auto& logicalSide = parameterization.logicalSides[sideIndex];
    if (logicalSide.edgeUseOrders.empty()) return std::nullopt;

    LogicalSideTarget target;
    const auto singleOrder =
        logicalSide.edgeUseOrders.size() == 1
        ? logicalSide.edgeUseOrders.front()
        : loop->edges.size();
    if (singleOrder < loop->edges.size()) {
        const auto& use = loop->edges[singleOrder];
        if (use.edgeIndex < network.edges.size() &&
            !network.edges[use.edgeIndex].originalBoundary) {
            const auto& edge = network.edges[use.edgeIndex];
            target.key = "internal-" + std::to_string(edge.id);
            target.originalBoundary = false;
            target.reverseForPatch = !use.forward;
            target.segments.push_back({
                &edge, 0.0, 1.0, true});
            return target;
        }
    }

    target.key =
        "patch-" + std::to_string(patch) +
        "-side-" + std::to_string(sideIndex);
    target.originalBoundary = true;
    for (const auto order : logicalSide.edgeUseOrders) {
        if (order >= loop->edges.size()) return std::nullopt;
        const auto& use = loop->edges[order];
        if (use.edgeIndex >= network.edges.size()) return std::nullopt;
        const auto pcurve = std::find_if(
            parameterization.edgePcurves.begin(),
            parameterization.edgePcurves.end(),
            [&](const auto& candidate) {
                return candidate.loopOrder == order &&
                    candidate.edgeIndex == use.edgeIndex;
            });
        if (pcurve == parameterization.edgePcurves.end() ||
            !(pcurve->sideLastParameter >
                pcurve->sideFirstParameter)) {
            return std::nullopt;
        }
        target.segments.push_back({
            &network.edges[use.edgeIndex],
            pcurve->sideFirstParameter,
            pcurve->sideLastParameter,
            use.forward});
        target.originalBoundary =
            target.originalBoundary &&
            network.edges[use.edgeIndex].originalBoundary;
    }
    return target.segments.empty()
        ? std::nullopt
        : std::optional<LogicalSideTarget> {std::move(target)};
}

std::optional<std::vector<gp_Pnt>> fitLogicalSidePoles(
    const LogicalSideTarget& target,
    const std::vector<double>& knots,
    const int validationSampleCount,
    double& maximumDeviation) {
    const auto controlCount =
        static_cast<int>(knots.size()) + kDegree - 1;
    if (controlCount < kDegree + 1 ||
        target.segments.empty()) {
        return std::nullopt;
    }
    const auto firstPoint = target.value(0.0);
    const auto lastPoint = target.value(1.0);
    const auto interiorCount = controlCount - 2;
    const auto fitSampleCount = std::max(
        validationSampleCount,
        controlCount * (kDegree + 1));
    Eigen::MatrixXd matrix(fitSampleCount, interiorCount);
    Eigen::MatrixXd values(fitSampleCount, 3);
    for (int sample = 0; sample < fitSampleCount; ++sample) {
        const auto parameter =
            (static_cast<double>(sample) + 0.5) /
            static_cast<double>(fitSampleCount);
        const auto basis = basisValues(knots, parameter);
        const auto targetPoint = target.value(parameter);
        for (int control = 1; control + 1 < controlCount; ++control) {
            matrix(sample, control - 1) =
                basis[static_cast<std::size_t>(control)];
        }
        const std::array<double, 3> coordinates {
            targetPoint.X(), targetPoint.Y(), targetPoint.Z()};
        const std::array<double, 3> first {
            firstPoint.X(), firstPoint.Y(), firstPoint.Z()};
        const std::array<double, 3> last {
            lastPoint.X(), lastPoint.Y(), lastPoint.Z()};
        for (int axis = 0; axis < 3; ++axis) {
            values(sample, axis) =
                coordinates[static_cast<std::size_t>(axis)] -
                basis.front() * first[static_cast<std::size_t>(axis)] -
                basis.back() * last[static_cast<std::size_t>(axis)];
        }
    }
    const auto decomposition = matrix.colPivHouseholderQr();
    if (decomposition.rank() < interiorCount) return std::nullopt;
    const Eigen::MatrixXd interior = decomposition.solve(values);
    if (!interior.allFinite()) return std::nullopt;
    std::vector<gp_Pnt> poles(
        static_cast<std::size_t>(controlCount));
    poles.front() = firstPoint;
    poles.back() = lastPoint;
    for (int control = 1; control + 1 < controlCount; ++control) {
        poles[static_cast<std::size_t>(control)] = gp_Pnt(
            interior(control - 1, 0),
            interior(control - 1, 1),
            interior(control - 1, 2));
    }
    maximumDeviation = 0.0;
    const auto denseCount = std::max(validationSampleCount, 257);
    for (int sample = 0; sample < denseCount; ++sample) {
        const auto parameter =
            static_cast<double>(sample) /
            static_cast<double>(denseCount - 1);
        const auto basis = basisValues(knots, parameter);
        gp_XYZ fitted {};
        for (int control = 0; control < controlCount; ++control) {
            fitted += poles[static_cast<std::size_t>(control)].XYZ() *
                basis[static_cast<std::size_t>(control)];
        }
        maximumDeviation = std::max(
            maximumDeviation,
            gp_Pnt(fitted).Distance(target.value(parameter)));
    }
    return poles;
}

std::optional<BoundarySupportNetwork> buildBoundarySupportNetwork(
    const RegionPatchLayout& layout,
    const RegionCurveNetwork& network,
    const RegionRefitOptions& options) {
    BoundarySupportNetwork result;
    if (!network.valid() ||
        layout.patchCount != network.patchCount ||
        layout.patches.size() !=
            static_cast<std::size_t>(layout.patchCount)) {
        result.message =
            "Boundary-support construction received mismatched topology.";
        return result;
    }
    std::vector<std::array<LogicalSideTarget, 4>> targets(
        static_cast<std::size_t>(layout.patchCount));
    std::vector<std::array<int, 4>> requiredCounts(
        static_cast<std::size_t>(layout.patchCount));
    const auto maximumSupportControlCount =
        std::min(options.maximumControlPointsPerSide, 25);
    for (int patch = 0; patch < layout.patchCount; ++patch) {
        for (std::size_t side = 0; side < 4; ++side) {
            const auto target =
                logicalSideTarget(layout, network, patch, side);
            if (!target.has_value()) {
                result.message =
                    "A logical side has no valid frozen support target.";
                return result;
            }
            targets[static_cast<std::size_t>(patch)][side] = *target;
            const auto tolerance = target->originalBoundary
                ? options.curveOnSurfaceTolerance
                : options.curveProjectionTolerance;
            auto qualifiedCount = 0;
            for (int controlCount = kDegree + 1;
                 controlCount <= maximumSupportControlCount;
                 ++controlCount) {
                auto deviation = 0.0;
                const auto poles = fitLogicalSidePoles(
                    *target,
                    uniformUniqueKnots(controlCount),
                    options.curveValidationSampleCount,
                    deviation);
                if (poles.has_value() && std::isfinite(deviation) &&
                    deviation <= tolerance) {
                    qualifiedCount = controlCount;
                    break;
                }
            }
            if (qualifiedCount == 0) {
                std::ostringstream stream;
                stream
                    << "A logical side could not meet its frozen-curve tolerance with at most "
                    << maximumSupportControlCount
                    << " cubic support control points.";
                result.message = stream.str();
                result.productQualified = false;
                return result;
            }
            requiredCounts[static_cast<std::size_t>(patch)][side] =
                qualifiedCount;
        }
    }

    auto changed = true;
    while (changed) {
        changed = false;
        for (auto& counts : requiredCounts) {
            const auto uCount = std::max(counts[0], counts[2]);
            const auto vCount = std::max(counts[1], counts[3]);
            for (const auto side : {std::size_t {0}, std::size_t {2}}) {
                if (counts[side] != uCount) {
                    counts[side] = uCount;
                    changed = true;
                }
            }
            for (const auto side : {std::size_t {1}, std::size_t {3}}) {
                if (counts[side] != vCount) {
                    counts[side] = vCount;
                    changed = true;
                }
            }
        }
        std::map<std::string, int> sharedCounts;
        for (std::size_t patch = 0; patch < targets.size(); ++patch) {
            for (std::size_t side = 0; side < 4; ++side) {
                if (targets[patch][side].originalBoundary) continue;
                sharedCounts[targets[patch][side].key] = std::max(
                    sharedCounts[targets[patch][side].key],
                    requiredCounts[patch][side]);
            }
        }
        for (std::size_t patch = 0; patch < targets.size(); ++patch) {
            for (std::size_t side = 0; side < 4; ++side) {
                if (targets[patch][side].originalBoundary) continue;
                const auto shared =
                    sharedCounts.at(targets[patch][side].key);
                if (requiredCounts[patch][side] != shared) {
                    requiredCounts[patch][side] = shared;
                    changed = true;
                }
            }
        }
    }

    result.knots.resize(static_cast<std::size_t>(layout.patchCount));
    std::vector<int> offsets(static_cast<std::size_t>(layout.patchCount));
    auto fullControlCount = 0;
    for (int patch = 0; patch < layout.patchCount; ++patch) {
        auto& knots = result.knots[static_cast<std::size_t>(patch)];
        knots.u = uniformUniqueKnots(
            requiredCounts[static_cast<std::size_t>(patch)][0]);
        knots.v = uniformUniqueKnots(
            requiredCounts[static_cast<std::size_t>(patch)][1]);
        offsets[static_cast<std::size_t>(patch)] = fullControlCount;
        fullControlCount +=
            knots.uControlCount() * knots.vControlCount();
    }
    result.fixedControls.resize(
        static_cast<std::size_t>(fullControlCount));
    std::map<std::pair<std::string, int>, std::vector<gp_Pnt>>
        sharedPoles;
    for (int patch = 0; patch < layout.patchCount; ++patch) {
        const auto& patchKnots =
            result.knots[static_cast<std::size_t>(patch)];
        const auto uCount = patchKnots.uControlCount();
        const auto vCount = patchKnots.vControlCount();
        const auto offset = offsets[static_cast<std::size_t>(patch)];
        for (std::size_t side = 0; side < 4; ++side) {
            const auto& target =
                targets[static_cast<std::size_t>(patch)][side];
            const auto& sideKnots =
                side == 0 || side == 2
                ? patchKnots.u
                : patchKnots.v;
            const auto controlCount =
                static_cast<int>(sideKnots.size()) + kDegree - 1;
            auto& poles = sharedPoles[{target.key, controlCount}];
            auto deviation = 0.0;
            if (poles.empty()) {
                const auto fitted = fitLogicalSidePoles(
                    target,
                    sideKnots,
                    options.curveValidationSampleCount,
                    deviation);
                if (!fitted.has_value()) {
                    result.message =
                        "A compatible logical-side support fit failed.";
                    return result;
                }
                poles = *fitted;
            } else {
                const auto fitted = fitLogicalSidePoles(
                    target,
                    sideKnots,
                    options.curveValidationSampleCount,
                    deviation);
                if (!fitted.has_value()) return std::nullopt;
            }
            if (target.originalBoundary) {
                result.maximumOuterBoundaryDeviation = std::max(
                    result.maximumOuterBoundaryDeviation,
                    deviation);
            } else {
                result.maximumInternalBoundaryDeviation = std::max(
                    result.maximumInternalBoundaryDeviation,
                    deviation);
            }
            auto orientedPoles = poles;
            if (target.reverseForPatch) {
                std::reverse(orientedPoles.begin(), orientedPoles.end());
            }
            for (int control = 0; control < controlCount; ++control) {
                int u = 0;
                int v = 0;
                switch (side) {
                case 0:
                    u = control;
                    v = 0;
                    break;
                case 1:
                    u = uCount - 1;
                    v = control;
                    break;
                case 2:
                    u = uCount - 1 - control;
                    v = vCount - 1;
                    break;
                case 3:
                    u = 0;
                    v = vCount - 1 - control;
                    break;
                }
                const auto index =
                    offset + u * vCount + v;
                auto& fixed =
                    result.fixedControls[static_cast<std::size_t>(index)];
                const auto& pole =
                    orientedPoles[static_cast<std::size_t>(control)];
                if (fixed.has_value() &&
                    fixed->Distance(pole) >
                        options.curveOnSurfaceTolerance) {
                    result.message =
                        "Compatible logical sides disagree at a corner control point.";
                    return result;
                }
                fixed = pole;
            }
        }
    }
    result.fixedControlCount = static_cast<int>(std::count_if(
        result.fixedControls.begin(),
        result.fixedControls.end(),
        [](const auto& point) { return point.has_value(); }));
    result.productQualified =
        result.maximumOuterBoundaryDeviation <=
            options.curveOnSurfaceTolerance &&
        result.maximumInternalBoundaryDeviation <=
            options.curveProjectionTolerance;
    result.message = result.productQualified
        ? "Built compatible bounded cubic logical-side supports."
        : "Bounded logical-side supports exceeded a frozen-curve tolerance.";
    return result;
}

std::optional<Eigen::MatrixXd> solveReducedBoundaryLockedSystem(
    const std::vector<LinearRow>& rows,
    const std::vector<NormalRow>& normalRows,
    const BoundarySupportNetwork& support,
    RegionFitIterationReport& report,
    const RegionRefitOptions& options) {
    const auto fullControlCount =
        static_cast<int>(support.fixedControls.size());
    std::vector<int> freeIndex(
        static_cast<std::size_t>(fullControlCount), -1);
    auto freeControlCount = 0;
    for (int control = 0; control < fullControlCount; ++control) {
        if (!support.fixedControls[static_cast<std::size_t>(control)]
                 .has_value()) {
            freeIndex[static_cast<std::size_t>(control)] =
                freeControlCount++;
        }
    }
    report.fixedBoundaryControlPointCount =
        support.fixedControlCount;
    report.interiorControlPointCount = freeControlCount;
    report.reducedSpdSystemUsed = true;
    Eigen::MatrixXd solution(fullControlCount, 3);
    for (int control = 0; control < fullControlCount; ++control) {
        const auto& fixed =
            support.fixedControls[static_cast<std::size_t>(control)];
        if (!fixed.has_value()) continue;
        solution(control, 0) = fixed->X();
        solution(control, 1) = fixed->Y();
        solution(control, 2) = fixed->Z();
    }
    if (freeControlCount == 0) return solution;

    const auto reducedVariableCount = freeControlCount * 3;
    std::vector<Eigen::Triplet<double>> triplets;
    Eigen::VectorXd targets = Eigen::VectorXd::Zero(
        static_cast<Eigen::Index>(rows.size() * 3 +
                                  normalRows.size()));
    auto scalarRow = 0;
    for (const auto& row : rows) {
        const std::array<double, 3> target {
            row.target.X(), row.target.Y(), row.target.Z()};
        for (int axis = 0; axis < 3; ++axis) {
            auto reducedTarget = target[static_cast<std::size_t>(axis)];
            for (const auto& [control, coefficient] :
                 row.coefficients) {
                const auto& fixed = support.fixedControls[
                    static_cast<std::size_t>(control)];
                if (fixed.has_value()) {
                    const std::array<double, 3> coordinates {
                        fixed->X(), fixed->Y(), fixed->Z()};
                    reducedTarget -= coefficient *
                        coordinates[static_cast<std::size_t>(axis)];
                } else {
                    triplets.emplace_back(
                        scalarRow,
                        freeIndex[static_cast<std::size_t>(control)] *
                                3 +
                            axis,
                        coefficient);
                }
            }
            targets[scalarRow++] = reducedTarget;
        }
    }
    constexpr double normalWeight = 0.25;
    for (const auto& row : normalRows) {
        const std::array<double, 3> normal {
            row.normal.X(), row.normal.Y(), row.normal.Z()};
        auto reducedTarget = 0.0;
        for (const auto& [control, coefficient] : row.coefficients) {
            const auto& fixed = support.fixedControls[
                static_cast<std::size_t>(control)];
            if (fixed.has_value()) {
                reducedTarget -= coefficient * normalWeight *
                    (fixed->X() * normal[0] +
                     fixed->Y() * normal[1] +
                     fixed->Z() * normal[2]);
            } else {
                for (int axis = 0; axis < 3; ++axis) {
                    triplets.emplace_back(
                        scalarRow,
                        freeIndex[static_cast<std::size_t>(control)] *
                                3 +
                            axis,
                        coefficient *
                            normal[static_cast<std::size_t>(axis)] *
                            normalWeight);
                }
            }
        }
        targets[scalarRow++] = reducedTarget;
    }
    Eigen::SparseMatrix<double> matrix(
        scalarRow, reducedVariableCount);
    matrix.setFromTriplets(triplets.begin(), triplets.end());
    matrix.makeCompressed();
    report.matrixRows = static_cast<std::size_t>(scalarRow);
    report.matrixColumns =
        static_cast<std::size_t>(reducedVariableCount);
    report.matrixNonZeros =
        static_cast<std::size_t>(matrix.nonZeros());
    Eigen::VectorXd columnScales =
        Eigen::VectorXd::Zero(reducedVariableCount);
    for (int column = 0; column < matrix.outerSize(); ++column) {
        for (Eigen::SparseMatrix<double>::InnerIterator value(
                 matrix, column);
             value;
             ++value) {
            columnScales[column] +=
                value.value() * value.value();
        }
    }
    Eigen::SparseMatrix<double> scaled = matrix;
    for (int column = 0; column < scaled.outerSize(); ++column) {
        columnScales[column] = std::sqrt(
            std::max(columnScales[column], 1.0e-24));
        for (Eigen::SparseMatrix<double>::InnerIterator value(
                 scaled, column);
             value;
             ++value) {
            value.valueRef() /= columnScales[column];
        }
    }
    Eigen::SparseMatrix<double> normal =
        scaled.transpose() * scaled;
    for (int diagonal = 0; diagonal < reducedVariableCount;
         ++diagonal) {
        normal.coeffRef(diagonal, diagonal) += 1.0e-10;
    }
    normal.makeCompressed();
    report.estimatedSolverBytes =
        static_cast<std::uint64_t>(
            matrix.nonZeros() + normal.nonZeros()) *
            16ull * 8ull +
        static_cast<std::uint64_t>(reducedVariableCount) * 256ull;
    const auto preflightLimit =
        static_cast<std::uint64_t>(
            options.solverMemoryPreflightMiB) *
        1024ull * 1024ull;
    if (report.estimatedSolverBytes > preflightLimit) {
        return std::nullopt;
    }
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
    solver.compute(normal);
    if (solver.info() != Eigen::Success) return std::nullopt;
    Eigen::VectorXd reduced =
        solver.solve(scaled.transpose() * targets);
    if (solver.info() != Eigen::Success || !reduced.allFinite()) {
        return std::nullopt;
    }
    for (int variable = 0; variable < reducedVariableCount;
         ++variable) {
        reduced[variable] /= columnScales[variable];
    }
    for (int control = 0; control < fullControlCount; ++control) {
        const auto free =
            freeIndex[static_cast<std::size_t>(control)];
        if (free < 0) continue;
        for (int axis = 0; axis < 3; ++axis) {
            solution(control, axis) =
                reduced[free * 3 + axis];
        }
    }
    return solution.allFinite()
        ? std::optional<Eigen::MatrixXd> {std::move(solution)}
        : std::nullopt;
}

TopoDS_Shape inspectionShape(const std::vector<Handle(Geom_BSplineSurface)>& surfaces) {
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& surface : surfaces) {
        const auto face = BRepBuilderAPI_MakeFace(surface, 1.0e-7);
        if (face.IsDone()) builder.Add(compound, face.Face());
    }
    return compound;
}

std::vector<Handle(Geom_BSplineSurface)> makeBoundaryLockedCoonsNetwork(
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    const RegionCurveNetwork& network,
    const RegionRefitOptions& options,
    std::string& failure) {
    std::vector<Handle(Geom_BSplineSurface)> surfaces;
    surfaces.reserve(layout.patches.size());
    constexpr double joinTolerance = 1.0e-7;
    try {
        for (std::size_t patchIndex = 0;
             patchIndex < layout.patches.size();
             ++patchIndex) {
            const auto loop = std::find_if(
                network.patchLoops.begin(),
                network.patchLoops.end(),
                [&](const auto& candidate) {
                    return candidate.patch ==
                        static_cast<int>(patchIndex);
                });
            if (loop == network.patchLoops.end()) {
                failure = "A Coons fallback patch has no frozen curve loop.";
                return {};
            }
            const auto& parameterization = layout.patches[patchIndex];
            if (!parameterization.rectangularIsoBoundary) {
                failure =
                    "A Coons fallback patch has no rectangular iso boundary.";
                return {};
            }
            std::array<Handle(Geom_BSplineCurve), 4> sides;
            for (std::size_t sideIndex = 0;
                 sideIndex < parameterization.logicalSides.size();
                 ++sideIndex) {
                const auto& logicalSide =
                    parameterization.logicalSides[sideIndex];
                std::unique_ptr<GeomConvert_CompCurveToBSplineCurve> composer;
                for (const auto order : logicalSide.edgeUseOrders) {
                    if (order >= loop->edges.size()) {
                        failure =
                            "A Coons fallback logical side references a missing edge use.";
                        return {};
                    }
                    const auto& use = loop->edges[order];
                    if (use.edgeIndex >= network.edges.size()) {
                        failure =
                            "A Coons fallback logical side references a missing frozen edge.";
                        return {};
                    }
                    const auto& edge = network.edges[use.edgeIndex];
                    if (edge.curve.IsNull() ||
                        !(edge.lastParameter > edge.firstParameter)) {
                        failure =
                            "A Coons fallback logical side contains an invalid frozen curve.";
                        return {};
                    }
                    Handle(Geom_TrimmedCurve) segment =
                        new Geom_TrimmedCurve(
                            edge.curve,
                            edge.firstParameter,
                            edge.lastParameter);
                    if (!use.forward) segment->Reverse();
                    auto segmentSpline =
                        GeomConvert::CurveToBSplineCurve(segment);
                    if (segmentSpline.IsNull()) {
                        failure =
                            "A Coons fallback edge could not be converted to B-spline form.";
                        return {};
                    }
                    const auto pcurve = std::find_if(
                        parameterization.edgePcurves.begin(),
                        parameterization.edgePcurves.end(),
                        [&](const auto& candidate) {
                            return candidate.edgeIndex == use.edgeIndex;
                        });
                    if (pcurve == parameterization.edgePcurves.end() ||
                        !(pcurve->sideLastParameter >
                            pcurve->sideFirstParameter)) {
                        failure =
                            "A Coons fallback edge has no logical-side parameter interval.";
                        return {};
                    }
                    const auto firstKnot = segmentSpline->Knot(1);
                    const auto lastKnot =
                        segmentSpline->Knot(segmentSpline->NbKnots());
                    if (!(lastKnot > firstKnot)) {
                        failure =
                            "A Coons fallback edge has a degenerate B-spline parameter range.";
                        return {};
                    }
                    TColStd_Array1OfReal segmentKnots(
                        1, segmentSpline->NbKnots());
                    for (int knot = 1;
                         knot <= segmentSpline->NbKnots();
                         ++knot) {
                        const auto alpha =
                            (segmentSpline->Knot(knot) - firstKnot) /
                            (lastKnot - firstKnot);
                        segmentKnots.SetValue(
                            knot,
                            pcurve->sideFirstParameter +
                                alpha *
                                    (pcurve->sideLastParameter -
                                     pcurve->sideFirstParameter));
                    }
                    segmentSpline->SetKnots(segmentKnots);
                    Handle(Geom_BoundedCurve) bounded = segmentSpline;
                    if (!composer) {
                        composer =
                            std::make_unique<
                                GeomConvert_CompCurveToBSplineCurve>(
                                bounded);
                    } else if (!composer->Add(
                                   bounded,
                                   joinTolerance,
                                   Standard_True,
                                   Standard_False,
                                   0)) {
                        failure =
                            "A Coons fallback logical side is not G0-contiguous.";
                        return {};
                    }
                }
                if (!composer) {
                    failure = "A Coons fallback logical side is empty.";
                    return {};
                }
                sides[sideIndex] = composer->BSplineCurve();
                if (sides[sideIndex].IsNull()) {
                    failure =
                        "A Coons fallback logical side could not be converted to B-spline form.";
                    return {};
                }
            }
            const auto normalizeKnots = [](const auto count,
                                           const auto knot,
                                           const auto setKnots) {
                const auto first = knot(1);
                const auto last = knot(count);
                if (!(last > first)) return false;
                TColStd_Array1OfReal normalized(1, count);
                for (int index = 1; index <= count; ++index) {
                    normalized.SetValue(
                        index,
                        (knot(index) - first) / (last - first));
                }
                setKnots(normalized);
                return true;
            };
            Handle(Geom_BSplineSurface) surface;
            std::string lastSupportFailure;
            for (const auto style : {
                     GeomFill_CoonsStyle,
                     GeomFill_StretchStyle,
                     GeomFill_CurvedStyle}) {
                try {
                    GeomFill_BSplineCurves filling(
                        sides[0],
                        sides[1],
                        sides[2],
                        sides[3],
                        style);
                    auto candidate = filling.Surface();
                    if (candidate.IsNull()) {
                        lastSupportFailure =
                            "Open CASCADE did not construct a boundary-locked fallback surface.";
                        continue;
                    }
                    if (!normalizeKnots(
                            candidate->NbUKnots(),
                            [&](const int index) {
                                return candidate->UKnot(index);
                            },
                            [&](const TColStd_Array1OfReal& knots) {
                                candidate->SetUKnots(knots);
                            }) ||
                        !normalizeKnots(
                            candidate->NbVKnots(),
                            [&](const int index) {
                                return candidate->VKnot(index);
                            },
                            [&](const TColStd_Array1OfReal& knots) {
                                candidate->SetVKnots(knots);
                            })) {
                        lastSupportFailure =
                            "A boundary-locked fallback surface has a degenerate parameter range.";
                        continue;
                    }
                    auto boundaryDeviation = 0.0;
                    constexpr int boundarySamples = 33;
                    for (const auto& use : loop->edges) {
                        const auto& edge = network.edges[use.edgeIndex];
                        for (int sample = 0;
                             sample < boundarySamples;
                             ++sample) {
                            const auto parameter =
                                edge.firstParameter +
                                static_cast<double>(sample) /
                                    static_cast<double>(boundarySamples - 1) *
                                    (edge.lastParameter - edge.firstParameter);
                            const auto uv = curveParameterOnPatch(
                                layout,
                                edge,
                                static_cast<int>(patchIndex),
                                parameter);
                            if (!uv.has_value()) {
                                boundaryDeviation =
                                    std::numeric_limits<double>::infinity();
                                break;
                            }
                            boundaryDeviation = std::max(
                                boundaryDeviation,
                                candidate->Value((*uv)[0], (*uv)[1])
                                    .Distance(edge.curve->Value(parameter)));
                        }
                    }
                    if (boundaryDeviation >
                            options.curveOnSurfaceTolerance) {
                        std::ostringstream stream;
                        stream
                            << "A boundary-locked fallback surface boundary deviates by "
                            << boundaryDeviation
                            << " mm from its exact iso pcurve.";
                        lastSupportFailure = stream.str();
                        continue;
                    }
                    surface = std::move(candidate);
                    break;
                } catch (const Standard_Failure& exception) {
                    lastSupportFailure =
                        "Open CASCADE rejected a boundary-locked filling style";
                    if (const auto* text = exception.GetMessageString();
                        text != nullptr && *text != '\0') {
                        lastSupportFailure += ": ";
                        lastSupportFailure += text;
                    }
                    lastSupportFailure += ".";
                    continue;
                }
            }
            if (surface.IsNull()) {
                failure = lastSupportFailure;
                return {};
            }
            surfaces.push_back(std::move(surface));
        }
        constexpr int validationSamples = 257;
        auto maximumDeviation = 0.0;
        for (const auto& edge : network.edges) {
            const std::array<int, 2> patches {
                edge.firstPatch, edge.secondPatch};
            const auto patchCount = edge.originalBoundary ? 1 : 2;
            for (int side = 0; side < patchCount; ++side) {
                const auto patch = patches[static_cast<std::size_t>(side)];
                if (patch < 0 ||
                    patch >= static_cast<int>(surfaces.size())) {
                    failure =
                        "A Coons fallback edge references an invalid patch.";
                    return {};
                }
                for (int sample = 0; sample < validationSamples; ++sample) {
                    const auto parameter =
                        edge.firstParameter +
                        static_cast<double>(sample) /
                            static_cast<double>(validationSamples - 1) *
                            (edge.lastParameter - edge.firstParameter);
                    const auto uv = curveParameterOnPatch(
                        layout, edge, patch, parameter);
                    if (!uv.has_value()) {
                        failure =
                            "A Coons fallback edge has no exact iso pcurve.";
                        return {};
                    }
                    maximumDeviation = std::max(
                        maximumDeviation,
                        surfaces[static_cast<std::size_t>(patch)]
                            ->Value((*uv)[0], (*uv)[1])
                            .Distance(edge.curve->Value(parameter)));
                }
            }
        }
        if (maximumDeviation > options.curveOnSurfaceTolerance) {
            std::ostringstream stream;
            stream << "A Coons fallback boundary deviates by "
                   << maximumDeviation << " mm from its frozen curve.";
            failure = stream.str();
            return {};
        }
    } catch (const Standard_Failure&) {
        failure = "Open CASCADE rejected a boundary-locked Coons fallback.";
        return {};
    }
    return surfaces;
}

}

RegionSurfaceParameterMappingResult mapRegionReferenceSamplesToSurfaceParameters(
    const RegionReferenceMesh& fittingReference,
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout) {
    RegionSurfaceParameterMappingResult result;
    if (layout.trianglePatches.size() != layoutCarrier.triangles.size() ||
        layout.patches.size() != static_cast<std::size_t>(layout.patchCount)) {
        result.crossFaceFailureCount = fittingReference.vertices.size();
        return result;
    }

    const auto lookups = buildLookups(layout);
    const auto sourceForFace = [](const RegionReferenceVertex& vertex,
                                  const FaceId faceId)
        -> const RegionReferenceSourceSample* {
        const auto found = std::find_if(
            vertex.sources.begin(), vertex.sources.end(),
            [&](const auto& source) { return source.faceId == faceId; });
        return found == vertex.sources.end() ? nullptr : &*found;
    };
    const auto parameterAt = [&](const int patch,
                                 const std::size_t globalVertex)
        -> std::optional<std::array<double, 2>> {
        if (patch < 0 || patch >= layout.patchCount) return std::nullopt;
        const auto& parameterization =
            layout.patches[static_cast<std::size_t>(patch)];
        const auto found =
            lookups[static_cast<std::size_t>(patch)].localByGlobal.find(
                globalVertex);
        if (found ==
            lookups[static_cast<std::size_t>(patch)].localByGlobal.end()) {
            return std::nullopt;
        }
        const auto local = found->second;
        const auto& parameters =
            parameterization.surfaceParameters.size() ==
                parameterization.vertexIndices.size()
            ? parameterization.surfaceParameters
            : parameterization.parameters;
        if (local >= parameters.size()) return std::nullopt;
        return normalizedParameter(parameters[local]);
    };
    const auto barycentric = [](const gp_Pnt2d& point,
                                const gp_Pnt2d& first,
                                const gp_Pnt2d& second,
                                const gp_Pnt2d& third)
        -> std::optional<std::array<double, 3>> {
        const auto denominator =
            (second.Y() - third.Y()) * (first.X() - third.X()) +
            (third.X() - second.X()) * (first.Y() - third.Y());
        if (std::abs(denominator) <= 1.0e-20) return std::nullopt;
        const auto firstWeight =
            ((second.Y() - third.Y()) * (point.X() - third.X()) +
             (third.X() - second.X()) * (point.Y() - third.Y())) /
            denominator;
        const auto secondWeight =
            ((third.Y() - first.Y()) * (point.X() - third.X()) +
             (first.X() - third.X()) * (point.Y() - third.Y())) /
            denominator;
        return std::array<double, 3> {
            firstWeight, secondWeight, 1.0 - firstWeight - secondWeight};
    };

    result.samples.reserve(fittingReference.vertices.size());
    for (std::size_t referenceVertex = 0;
         referenceVertex < fittingReference.vertices.size();
         ++referenceVertex) {
        const auto& reference = fittingReference.vertices[referenceVertex];
        std::vector<const RegionReferenceSourceSample*> sources;
        sources.reserve(reference.sources.size());
        for (const auto& source : reference.sources) sources.push_back(&source);
        std::sort(
            sources.begin(), sources.end(),
            [](const auto* first, const auto* second) {
                return std::tuple {
                           first->faceId,
                           first->uv.X(),
                           first->uv.Y()} <
                    std::tuple {
                           second->faceId,
                           second->uv.X(),
                           second->uv.Y()};
            });

        std::optional<RegionSurfaceParameterMappingSample> exact;
        for (const auto* source : sources) {
            for (std::size_t triangleIndex = 0;
                 triangleIndex < layoutCarrier.triangles.size();
                 ++triangleIndex) {
                const auto& triangle = layoutCarrier.triangles[triangleIndex];
                if (triangle.faceId != source->faceId) continue;
                const auto patch = layout.trianglePatches[triangleIndex];
                if (patch < 0 || patch >= layout.patchCount) continue;
                std::array<const RegionReferenceSourceSample*, 3>
                    triangleSources {};
                std::array<std::array<double, 2>, 3> surfaceParameters {};
                auto usable = true;
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    if (triangle.vertices[corner] >=
                        layoutCarrier.vertices.size()) {
                        usable = false;
                        break;
                    }
                    triangleSources[corner] = sourceForFace(
                        layoutCarrier.vertices[triangle.vertices[corner]],
                        source->faceId);
                    const auto surfaceParameter =
                        parameterAt(patch, triangle.vertices[corner]);
                    if (triangleSources[corner] == nullptr ||
                        !surfaceParameter.has_value()) {
                        usable = false;
                        break;
                    }
                    surfaceParameters[corner] = *surfaceParameter;
                }
                if (!usable) continue;
                const auto weights = barycentric(
                    source->uv,
                    triangleSources[0]->uv,
                    triangleSources[1]->uv,
                    triangleSources[2]->uv);
                if (!weights.has_value()) continue;
                constexpr double insideTolerance = 1.0e-10;
                if (std::any_of(
                        weights->begin(), weights->end(),
                        [](const auto weight) {
                            return weight < -insideTolerance ||
                                weight > 1.0 + insideTolerance;
                        })) {
                    continue;
                }
                std::array<double, 2> parameter {};
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    parameter[0] += (*weights)[corner] *
                        surfaceParameters[corner][0];
                    parameter[1] += (*weights)[corner] *
                        surfaceParameters[corner][1];
                }
                exact = RegionSurfaceParameterMappingSample {
                    referenceVertex,
                    triangleIndex,
                    patch,
                    normalizedParameter(parameter)};
                break;
            }
            if (exact.has_value()) break;
        }
        if (exact.has_value()) {
            result.samples.push_back(*exact);
            ++result.sameFaceBarycentricCount;
            continue;
        }

        auto bestDistance = std::numeric_limits<double>::infinity();
        std::optional<RegionSurfaceParameterMappingSample> fallback;
        for (const auto* source : sources) {
            for (std::size_t carrierVertex = 0;
                 carrierVertex < layoutCarrier.vertices.size();
                 ++carrierVertex) {
                const auto* carrierSource = sourceForFace(
                    layoutCarrier.vertices[carrierVertex], source->faceId);
                if (carrierSource == nullptr) continue;
                for (int patch = 0; patch < layout.patchCount; ++patch) {
                    const auto surfaceParameter =
                        parameterAt(patch, carrierVertex);
                    if (!surfaceParameter.has_value()) continue;
                    const auto dx =
                        carrierSource->uv.X() - source->uv.X();
                    const auto dy =
                        carrierSource->uv.Y() - source->uv.Y();
                    const auto distance = dx * dx + dy * dy;
                    if (distance < bestDistance ||
                        (distance == bestDistance &&
                         (!fallback.has_value() ||
                          std::tuple {carrierVertex, patch} <
                              std::tuple {
                                  fallback->carrierTriangle,
                                  fallback->patch}))) {
                        bestDistance = distance;
                        fallback = RegionSurfaceParameterMappingSample {
                            referenceVertex,
                            carrierVertex,
                            patch,
                            *surfaceParameter};
                    }
                }
            }
        }
        if (fallback.has_value()) {
            result.samples.push_back(*fallback);
            ++result.sameFaceFallbackCount;
        } else {
            ++result.crossFaceFailureCount;
        }
    }
    return result;
}

bool regionLocalSupportDomainPassed(
    const Handle(Geom_BSplineSurface)& surface,
    const RegionReferenceMesh& carrier,
    const RegionPatchParameterization& parameterization,
    const RegionRefitOptions& options,
    std::string& reason) {
    return evaluateLocalSupportDomain(
        surface, carrier, parameterization, options, reason);
}

bool JointBSplineNetworkFitResult::success() const {
    return status == RegionRefitStatus::Success && network != nullptr;
}

double normalizedViolationScore(
    const ValidationMetrics& metrics,
    const RegionRefitOptions& options) {
    return std::max({
        metrics.holdoutMaximumDistance / std::max(options.tolerance3d, 1.0e-12),
        metrics.holdoutMaximumNormalDegrees / std::max(options.normalToleranceDegrees, 1.0e-12),
        metrics.boundaryMaximumDistance / std::max(options.curveOnSurfaceTolerance, 1.0e-12),
        metrics.seamMaximumDistance / std::max(options.curveOnSurfaceTolerance, 1.0e-12),
        metrics.seamMaximumCurveDeviation / std::max(options.curveOnSurfaceTolerance, 1.0e-12),
        metrics.seamMaximumNormalDegrees / std::max(options.g1ToleranceDegrees, 1.0e-12),
        metrics.seamMaximumCurvatureDifference / std::max(options.g2CurvatureTolerance, 1.0e-12)});
}

double percentile(
    std::vector<double> values,
    const double probability) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(
        std::clamp(probability, 0.0, 1.0) *
        static_cast<double>(values.size() - 1)));
    return values[index];
}

RegionSurfaceQualityMetrics measureSurfaceQuality(
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    const std::vector<Handle(Geom_BSplineSurface)>& surfaces) {
    RegionSurfaceQualityMetrics quality;
    quality.minimumNormalizedJacobian =
        std::numeric_limits<double>::infinity();
    std::vector<double> conditionNumbers;
    std::size_t fairnessTermCount = 0;
    std::size_t curvatureTermCount = 0;
    constexpr int sampleCount = 17;
    constexpr double derivativeTolerance = 1.0e-14;
    for (std::size_t patch = 0; patch < surfaces.size(); ++patch) {
        const auto& surface = surfaces[patch];
        if (surface.IsNull()) {
            quality.hardFailure = true;
            continue;
        }
        quality.surfaceComplexities.push_back({
            static_cast<int>(patch),
            surface->NbUPoles(),
            surface->NbVPoles(),
            surface->NbUKnots(),
            surface->NbVKnots()});
        quality.totalPoleCount +=
            surface->NbUPoles() * surface->NbVPoles();
        quality.maximumPoleCountPerSide = std::max({
            quality.maximumPoleCountPerSide,
            surface->NbUPoles(),
            surface->NbVPoles()});
        quality.totalKnotCount +=
            surface->NbUKnots() + surface->NbVKnots();
        const auto addFairnessTerm = [&](const gp_Pnt& previous,
                                         const gp_Pnt& current,
                                         const gp_Pnt& next) {
            const gp_XYZ second =
                previous.XYZ() - current.XYZ() * 2.0 + next.XYZ();
            const auto value = second.SquareModulus();
            if (!std::isfinite(value)) {
                quality.hardFailure = true;
                return;
            }
            quality.fairnessEnergy += value;
            ++fairnessTermCount;
        };
        for (int u = 2; u < surface->NbUPoles(); ++u) {
            for (int v = 1; v <= surface->NbVPoles(); ++v) {
                addFairnessTerm(
                    surface->Pole(u - 1, v),
                    surface->Pole(u, v),
                    surface->Pole(u + 1, v));
            }
        }
        for (int u = 1; u <= surface->NbUPoles(); ++u) {
            for (int v = 2; v < surface->NbVPoles(); ++v) {
                addFairnessTerm(
                    surface->Pole(u, v - 1),
                    surface->Pole(u, v),
                    surface->Pole(u, v + 1));
            }
        }

        std::array<std::array<gp_Dir, sampleCount>, sampleCount> normals;
        std::array<std::array<double, sampleCount>, sampleCount> curvatures {};
        std::array<std::array<bool, sampleCount>, sampleCount> valid {};
        for (int uIndex = 0; uIndex < sampleCount; ++uIndex) {
            const auto u = static_cast<double>(uIndex) /
                static_cast<double>(sampleCount - 1);
            for (int vIndex = 0; vIndex < sampleCount; ++vIndex) {
                const auto v = static_cast<double>(vIndex) /
                    static_cast<double>(sampleCount - 1);
                gp_Pnt point;
                gp_Vec du;
                gp_Vec dv;
                gp_Vec duu;
                gp_Vec duv;
                gp_Vec dvv;
                try {
                    surface->D2(u, v, point, du, dv, duu, duv, dvv);
                } catch (const Standard_Failure&) {
                    quality.hardFailure = true;
                    continue;
                }
                const auto du2 = du.SquareMagnitude();
                const auto dv2 = dv.SquareMagnitude();
                const auto dot = du.Dot(dv);
                const auto determinant = du2 * dv2 - dot * dot;
                if (!std::isfinite(du2) || !std::isfinite(dv2) ||
                    !std::isfinite(determinant) ||
                    du2 <= derivativeTolerance ||
                    dv2 <= derivativeTolerance ||
                    determinant <= derivativeTolerance *
                        std::max(du2 * dv2, 1.0)) {
                    quality.hardFailure = true;
                    continue;
                }
                const auto trace = du2 + dv2;
                const auto discriminant = std::sqrt(std::max(
                    0.0,
                    trace * trace - 4.0 * determinant));
                const auto largest = 0.5 * (trace + discriminant);
                const auto smallest = 0.5 * (trace - discriminant);
                if (!(smallest > derivativeTolerance) ||
                    !std::isfinite(largest)) {
                    quality.hardFailure = true;
                    continue;
                }
                const auto condition =
                    std::sqrt(largest / smallest);
                conditionNumbers.push_back(condition);
                const auto normalizedJacobian =
                    std::sqrt(determinant / (du2 * dv2));
                quality.minimumNormalizedJacobian = std::min(
                    quality.minimumNormalizedJacobian,
                    normalizedJacobian);
                gp_Vec normal = du.Crossed(dv);
                if (normal.SquareMagnitude() <= derivativeTolerance) {
                    quality.hardFailure = true;
                    continue;
                }
                normals[uIndex][vIndex] = gp_Dir(normal);
                const auto secondFundamentalU =
                    duu.Dot(gp_Vec(normals[uIndex][vIndex]));
                const auto secondFundamentalUv =
                    duv.Dot(gp_Vec(normals[uIndex][vIndex]));
                const auto secondFundamentalV =
                    dvv.Dot(gp_Vec(normals[uIndex][vIndex]));
                const auto meanCurvature =
                    (dv2 * secondFundamentalU -
                     2.0 * dot * secondFundamentalUv +
                     du2 * secondFundamentalV) /
                    (2.0 * determinant);
                if (!std::isfinite(meanCurvature) ||
                    !std::isfinite(condition) ||
                    !std::isfinite(normalizedJacobian)) {
                    quality.hardFailure = true;
                    continue;
                }
                curvatures[uIndex][vIndex] = meanCurvature;
                valid[uIndex][vIndex] = true;
                if (uIndex > 0 && valid[uIndex - 1][vIndex]) {
                    quality.maximumNormalVariationDegrees = std::max(
                        quality.maximumNormalVariationDegrees,
                        std::acos(std::clamp(
                            normals[uIndex][vIndex].Dot(
                                normals[uIndex - 1][vIndex]),
                            -1.0,
                            1.0)) * 180.0 / kPi);
                }
                if (vIndex > 0 && valid[uIndex][vIndex - 1]) {
                    quality.maximumNormalVariationDegrees = std::max(
                        quality.maximumNormalVariationDegrees,
                        std::acos(std::clamp(
                            normals[uIndex][vIndex].Dot(
                                normals[uIndex][vIndex - 1]),
                            -1.0,
                            1.0)) * 180.0 / kPi);
                }
            }
        }
        for (int uIndex = 1; uIndex + 1 < sampleCount; ++uIndex) {
            for (int vIndex = 1; vIndex + 1 < sampleCount; ++vIndex) {
                if (valid[uIndex - 1][vIndex] &&
                    valid[uIndex][vIndex] &&
                    valid[uIndex + 1][vIndex]) {
                    quality.curvatureOscillation += std::abs(
                        curvatures[uIndex - 1][vIndex] -
                        2.0 * curvatures[uIndex][vIndex] +
                        curvatures[uIndex + 1][vIndex]);
                    ++curvatureTermCount;
                }
                if (valid[uIndex][vIndex - 1] &&
                    valid[uIndex][vIndex] &&
                    valid[uIndex][vIndex + 1]) {
                    quality.curvatureOscillation += std::abs(
                        curvatures[uIndex][vIndex - 1] -
                        2.0 * curvatures[uIndex][vIndex] +
                        curvatures[uIndex][vIndex + 1]);
                    ++curvatureTermCount;
                }
            }
        }

        if (patch >= layout.patches.size()) {
            quality.hardFailure = true;
            continue;
        }
        const auto& parameterization = layout.patches[patch];
        std::unordered_map<std::size_t, std::size_t> localByGlobal;
        for (std::size_t local = 0;
             local < parameterization.vertexIndices.size();
             ++local) {
            localByGlobal.emplace(
                parameterization.vertexIndices[local], local);
        }
        const auto& parameters =
            parameterization.surfaceParameters.size() ==
                parameterization.vertexIndices.size()
            ? parameterization.surfaceParameters
            : parameterization.parameters;
        for (const auto triangleIndex : parameterization.triangleIndices) {
            if (triangleIndex >= layoutCarrier.triangles.size()) {
                quality.hardFailure = true;
                continue;
            }
            const auto& triangle = layoutCarrier.triangles[triangleIndex];
            std::array<std::size_t, 3> local {};
            auto mapped = true;
            for (std::size_t corner = 0; corner < local.size(); ++corner) {
                const auto found =
                    localByGlobal.find(triangle.vertices[corner]);
                if (found == localByGlobal.end() ||
                    found->second >= parameters.size() ||
                    triangle.vertices[corner] >=
                        layoutCarrier.vertices.size()) {
                    mapped = false;
                    break;
                }
                local[corner] = found->second;
            }
            if (!mapped) {
                quality.hardFailure = true;
                continue;
            }
            const auto uv = std::array<double, 2> {
                (parameters[local[0]][0] +
                 parameters[local[1]][0] +
                 parameters[local[2]][0]) / 3.0,
                (parameters[local[0]][1] +
                 parameters[local[1]][1] +
                 parameters[local[2]][1]) / 3.0};
            gp_Pnt point;
            gp_Vec du;
            gp_Vec dv;
            surface->D1(uv[0], uv[1], point, du, dv);
            const auto surfaceNormal = du.Crossed(dv);
            const auto first = layoutCarrier.vertices[
                triangle.vertices[0]].point;
            const auto referenceNormal = gp_Vec(
                first,
                layoutCarrier.vertices[triangle.vertices[1]].point)
                .Crossed(gp_Vec(
                    first,
                    layoutCarrier.vertices[triangle.vertices[2]].point));
            if (surfaceNormal.SquareMagnitude() <= derivativeTolerance ||
                referenceNormal.SquareMagnitude() <= derivativeTolerance ||
                surfaceNormal.Dot(referenceNormal) <= 0.0) {
                quality.hardFailure = true;
            }
        }
    }
    if (fairnessTermCount > 0) {
        quality.fairnessEnergy /=
            static_cast<double>(fairnessTermCount);
    }
    if (curvatureTermCount > 0) {
        quality.curvatureOscillation /=
            static_cast<double>(curvatureTermCount);
    }
    if (!std::isfinite(quality.minimumNormalizedJacobian)) {
        quality.minimumNormalizedJacobian = 0.0;
        quality.hardFailure = true;
    }
    quality.conditionNumberP50 = percentile(conditionNumbers, 0.50);
    quality.conditionNumberP95 = percentile(conditionNumbers, 0.95);
    quality.conditionNumberP99 = percentile(conditionNumbers, 0.99);
    return quality;
}

void recordSurfaceQuality(
    RegionFitIterationReport& report,
    const RegionSurfaceQualityMetrics& quality) {
    report.actualTotalPoleCount = quality.totalPoleCount;
    report.actualMaximumPoleCountPerSide =
        quality.maximumPoleCountPerSide;
    report.actualTotalKnotCount = quality.totalKnotCount;
    report.fairnessEnergy = quality.fairnessEnergy;
    report.maximumNormalVariationDegrees =
        quality.maximumNormalVariationDegrees;
    report.curvatureOscillation = quality.curvatureOscillation;
    report.minimumNormalizedJacobian =
        quality.minimumNormalizedJacobian;
    report.conditionNumberP50 = quality.conditionNumberP50;
    report.conditionNumberP95 = quality.conditionNumberP95;
    report.conditionNumberP99 = quality.conditionNumberP99;
    report.surfaceQualityHardFailure = quality.hardFailure;
}

bool fitGatesPassed(
    const ValidationMetrics& metrics,
    const RegionRefitOptions& options) {
    return metrics.holdoutMaximumDistance <= options.tolerance3d &&
        metrics.holdoutMaximumNormalDegrees <=
            options.normalToleranceDegrees &&
        metrics.boundaryMaximumDistance <=
            options.curveOnSurfaceTolerance &&
        metrics.seamMaximumDistance <=
            options.curveOnSurfaceTolerance &&
        metrics.seamContinuityGatePassed;
}

std::shared_ptr<RegionFittedPatchNetwork> fittedNetwork(
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    std::vector<Handle(Geom_BSplineSurface)> surfaces,
    const ValidationMetrics& metrics,
    const RegionRefitOptions& options,
    const bool gatePassed,
    const bool boundaryLocked,
    std::string fitOutcome) {
    auto network = std::make_shared<RegionFittedPatchNetwork>();
    network->layout = layout;
    network->surfaces = std::move(surfaces);
    network->quality = measureSurfaceQuality(
        layoutCarrier, layout, network->surfaces);
    network->inspectionShape = inspectionShape(network->surfaces);
    network->holdoutMaximumDistance = metrics.holdoutMaximumDistance;
    network->holdoutMaximumNormalDegrees = metrics.holdoutMaximumNormalDegrees;
    network->boundaryMaximumDistance = metrics.boundaryMaximumDistance;
    network->seamMaximumDistance = metrics.seamMaximumDistance;
    network->seamMaximumCurveDeviation = metrics.seamMaximumCurveDeviation;
    network->seamMaximumNormalDegrees = metrics.seamMaximumNormalDegrees;
    network->seamMaximumCurvatureDifference =
        metrics.seamMaximumCurvatureDifference;
    network->seamContinuityGatePassed = metrics.seamContinuityGatePassed;
    network->normalizedViolationScore = normalizedViolationScore(metrics, options);
    network->fitGatePassed = gatePassed;
    network->boundaryLocked = boundaryLocked;
    network->constructibleForBRep =
        boundaryLocked &&
        std::all_of(
            layout.patches.begin(),
            layout.patches.end(),
            [](const auto& patch) {
                return patch.rectangularIsoBoundary;
            });
    network->fitOutcome = std::move(fitOutcome);
    return network;
}

JointBSplineNetworkFitResult JointBSplineNetworkFitter::fit(
    const RegionReferenceMesh& fittingReference,
    const RegionReferenceMesh& layoutCarrier,
    const RegionTopologyConstraints& constraints,
    const std::vector<RegionPatchLayout>& candidates,
    const std::vector<RegionCurveNetwork>& curveNetworks,
    const RegionRefitOptions& options,
    const std::chrono::steady_clock::time_point deadline) const {
    JointBSplineNetworkFitResult result;
    const auto diagnosticRank = [&](const RegionFittedPatchNetwork& network) {
        auto layoutHardPassed = true;
        for (const auto& patch : network.layout.patches) {
            layoutHardPassed = layoutHardPassed &&
                patch.flippedTriangleCount == 0 &&
                patch.pcurveSelfIntersectionCount == 0 &&
                patch.pcurveNonAdjacentIntersectionCount == 0;
        }
        const auto hardPassed =
            static_cast<int>(network.boundaryLocked) +
            static_cast<int>(network.constructibleForBRep) +
            static_cast<int>(!network.quality.hardFailure) +
            static_cast<int>(layoutHardPassed);
        const auto productPassed =
            static_cast<int>(
                network.holdoutMaximumDistance <= options.tolerance3d) +
            static_cast<int>(
                network.holdoutMaximumNormalDegrees <=
                    options.normalToleranceDegrees) +
            static_cast<int>(
                network.boundaryMaximumDistance <=
                    options.curveOnSurfaceTolerance) +
            static_cast<int>(
                network.seamMaximumDistance <=
                    options.curveOnSurfaceTolerance) +
            static_cast<int>(
                network.seamMaximumCurveDeviation <=
                    options.curveOnSurfaceTolerance) +
            static_cast<int>(
                network.seamMaximumNormalDegrees <=
                    options.g1ToleranceDegrees) +
            static_cast<int>(
                !options.enableInternalSeamG2 ||
                network.seamMaximumCurvatureDifference <=
                    options.g2CurvatureTolerance) +
            static_cast<int>(
                network.quality.maximumPoleCountPerSide <=
                    std::min(
                        options.maximumControlPointsPerSide,
                        25));
        return std::tuple {
            -hardPassed,
            -productPassed,
            network.normalizedViolationScore,
            network.seamMaximumNormalDegrees,
            network.holdoutMaximumDistance,
            network.holdoutMaximumNormalDegrees,
            network.quality.fairnessEnergy,
            network.quality.curvatureOscillation,
            network.quality.totalPoleCount,
            network.layout.patchCount,
            network.layout.hash};
    };
    const auto maximumAttempts = std::min<std::size_t>(
        candidates.size(), static_cast<std::size_t>(options.maximumFullFitCount));
    for (std::size_t candidateIndex = 0; candidateIndex < maximumAttempts; ++candidateIndex) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.status = RegionRefitStatus::TimeBudgetExceeded;
            result.message = "Joint fitting reached the cumulative fit deadline.";
            return result;
        }
        const auto attemptStarted = std::chrono::steady_clock::now();
        const auto remainingAttempts = maximumAttempts - candidateIndex;
        const auto remainingFitMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - attemptStarted).count();
        const auto fairAttemptBudgetMilliseconds = std::max<std::int64_t>(
            1,
            remainingFitMilliseconds /
                static_cast<std::int64_t>(remainingAttempts));
        const auto attemptDeadline = std::min(
            deadline,
            attemptStarted + std::chrono::milliseconds(std::max(
                options.singleFitBudgetMilliseconds,
                fairAttemptBudgetMilliseconds)));
        const auto& layout = candidates[candidateIndex];
        const auto* curveNetwork = curveNetworkFor(layout, curveNetworks);
        RegionFitAttemptReport attempt;
        attempt.patchCount = layout.patchCount;
        attempt.layoutHash = layout.hash;
        if (!layout.curveNetworkHash.empty() &&
            (curveNetwork == nullptr || !curveNetwork->valid())) {
            attempt.stopReason =
                "The layout has no matching validated frozen curve network.";
            result.attempts.push_back(std::move(attempt));
            continue;
        }
        const auto lookups = buildLookups(layout);
        std::size_t sameFaceBarycentricMappingCount = 0;
        std::size_t sameFaceMappingFallbackCount = 0;
        std::size_t crossFaceMappingFailureCount = 0;
        const auto mappedSamples = mapReferenceSamples(
            fittingReference,
            layoutCarrier,
            layout,
            lookups,
            sameFaceBarycentricMappingCount,
            sameFaceMappingFallbackCount,
            crossFaceMappingFailureCount);
        attempt.sameFaceBarycentricMappingCount =
            sameFaceBarycentricMappingCount;
        attempt.sameFaceMappingFallbackCount =
            sameFaceMappingFallbackCount;
        attempt.crossFaceMappingFailureCount =
            crossFaceMappingFailureCount;
        if (mappedSamples.size() != fittingReference.vertices.size()) {
            std::ostringstream stream;
            stream
                << "Not every fitting-reference sample could be mapped through a same-FaceId carrier triangle"
                << " (same_face_fallbacks="
                << sameFaceMappingFallbackCount
                << ", cross_face_failures="
                << crossFaceMappingFailureCount << ").";
            attempt.stopReason = stream.str();
            result.attempts.push_back(std::move(attempt));
            continue;
        }
        std::vector<KnotConfiguration> knots(static_cast<std::size_t>(layout.patchCount));
        double previousSelection = std::numeric_limits<double>::infinity();
        int lowImprovementRounds = 0;
        int boundaryFeasibilityRefinementRounds = 0;
        std::vector<Handle(Geom_BSplineSurface)> bestSurfaces;
        ValidationMetrics bestMetrics;
        RegionSurfaceQualityMetrics bestQuality;
        auto bestBoundaryLocked = false;
        std::string boundaryFallbackFailure;
        if (curveNetwork != nullptr) {
            auto coonsSurfaces = makeBoundaryLockedCoonsNetwork(
                layoutCarrier,
                layout,
                *curveNetwork,
                options,
                boundaryFallbackFailure);
            if (!coonsSurfaces.empty()) {
                bestMetrics = validateNetwork(
                    fittingReference,
                    layoutCarrier,
                    layout,
                    lookups,
                    coonsSurfaces,
                    options,
                    mappedSamples);
                validateFrozenCurveContinuity(
                    bestMetrics,
                    layout,
                    *curveNetwork,
                    coonsSurfaces,
                    options);
                bestQuality = measureSurfaceQuality(
                    layoutCarrier, layout, coonsSurfaces);
                bestSurfaces = std::move(coonsSurfaces);
                bestBoundaryLocked = true;
            }
        }

        std::optional<BoundarySupportNetwork> boundedSupport;
        if (curveNetwork != nullptr) {
            boundedSupport = buildBoundarySupportNetwork(
                layout, *curveNetwork, options);
            if (boundedSupport.has_value() &&
                boundedSupport->productQualified) {
                knots = boundedSupport->knots;
            } else {
                attempt.stopReason = boundedSupport.has_value()
                    ? boundedSupport->message
                    : "Bounded cubic logical-side support construction failed.";
            }
        }
        const auto maximumIterations = curveNetwork != nullptr
            ? (boundedSupport.has_value() &&
                    boundedSupport->productQualified
                ? 1 : 0)
            : std::min(options.maximumFitIterations, 20);
        for (int iteration = 0; iteration < maximumIterations; ++iteration) {
            const auto iterationStarted = std::chrono::steady_clock::now();
            RegionFitIterationReport iterationReport;
            iterationReport.iteration = iteration + 1;
            iterationReport.sameFaceBarycentricMappingCount =
                sameFaceBarycentricMappingCount;
            iterationReport.sameFaceMappingFallbackCount =
                sameFaceMappingFallbackCount;
            iterationReport.crossFaceMappingFailureCount =
                crossFaceMappingFailureCount;
            auto finalIterationForBudget = false;
            for (const auto& patchKnots : knots) {
                iterationReport.uControlCount = std::max(
                    iterationReport.uControlCount, patchKnots.uControlCount());
                iterationReport.vControlCount = std::max(
                    iterationReport.vControlCount, patchKnots.vControlCount());
            }
            if (!attempt.iterations.empty()) {
                const auto forecastMilliseconds = static_cast<long long>(std::ceil(
                    std::max(1.0, attempt.iterations.back().wallMilliseconds) * 2.5));
                if (std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(forecastMilliseconds) >= attemptDeadline) {
                    finalIterationForBudget = true;
                }
            }
            if (std::chrono::steady_clock::now() >= attemptDeadline) {
                iterationReport.stopReason = "Single-fit deadline reached before starting the iteration.";
                attempt.iterations.push_back(iterationReport);
                attempt.stopReason = iterationReport.stopReason;
                break;
            }
            std::vector<int> variableOffsets(static_cast<std::size_t>(layout.patchCount));
            int controlVariableCount = 0;
            for (int patch = 0; patch < layout.patchCount; ++patch) {
                variableOffsets[static_cast<std::size_t>(patch)] = controlVariableCount;
                const auto& patchKnots = knots[static_cast<std::size_t>(patch)];
                controlVariableCount += patchKnots.uControlCount() * patchKnots.vControlCount();
            }
            const auto seamSamples = buildSeamSamples(layout);
            const auto seamConstraintSamples = selectSeamConstraintSamples(
                layout,
                knots,
                lookups,
                seamSamples);
            const auto representativeFitSamples = selectRepresentativeFitSamples(
                fittingReference,
                mappedSamples,
                knots);
            std::size_t seamG1RowCount = 0;
            std::size_t seamG2RowCount = 0;
            const auto rows = buildSystemRows(
                fittingReference,
                layoutCarrier,
                layout,
                knots,
                variableOffsets,
                lookups,
                seamConstraintSamples,
                representativeFitSamples,
                curveNetwork,
                options,
                seamG1RowCount,
                seamG2RowCount);
            std::size_t adjacentNormalRowCount = 0;
            const auto normalRows = buildNormalRows(
                fittingReference,
                layoutCarrier,
                constraints,
                layout,
                knots,
                variableOffsets,
                representativeFitSamples,
                adjacentNormalRowCount);
            iterationReport.adjacentNormalSoftConstraintCount = adjacentNormalRowCount;
            if (curveNetwork != nullptr &&
                boundedSupport.has_value() &&
                boundedSupport->productQualified) {
                iterationReport.boundaryHardConstraintCount =
                    static_cast<std::size_t>(
                        boundedSupport->fixedControlCount);
                iterationReport.seamG0ConstraintCount =
                    curveNetwork->report.internalEdgeCount;
                iterationReport.seamG1SoftConstraintCount =
                    seamG1RowCount;
                iterationReport.seamG2SoftConstraintCount =
                    seamG2RowCount;
                iterationReport.constraintRowsBefore =
                    static_cast<std::size_t>(
                        boundedSupport->fixedControlCount);
                iterationReport.constraintRowsAfter =
                    iterationReport.constraintRowsBefore;
                iterationReport.constraintEstimatedRank =
                    iterationReport.constraintRowsBefore;
                iterationReport.minimumFullHardConstraintResidual =
                    std::max(
                        boundedSupport->maximumOuterBoundaryDeviation,
                        boundedSupport->maximumInternalBoundaryDeviation);
                iterationReport.boundaryFeasibilityResidual =
                    iterationReport.minimumFullHardConstraintResidual;
                iterationReport.maximumHardConstraintResidual =
                    iterationReport.minimumFullHardConstraintResidual;
                iterationReport.hardConstraintFeasible = true;
                iterationReport.hardConstraintStatus =
                    "BoundaryControlElimination";
                iterationReport.boundaryTargetMinimum = {
                    std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity()};
                iterationReport.boundaryTargetMaximum = {
                    -std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity()};
                for (const auto& fixed :
                     boundedSupport->fixedControls) {
                    if (!fixed.has_value()) continue;
                    const std::array<double, 3> coordinates {
                        fixed->X(), fixed->Y(), fixed->Z()};
                    for (std::size_t axis = 0;
                         axis < coordinates.size();
                         ++axis) {
                        iterationReport.boundaryTargetMinimum[axis] =
                            std::min(
                                iterationReport
                                    .boundaryTargetMinimum[axis],
                                coordinates[axis]);
                        iterationReport.boundaryTargetMaximum[axis] =
                            std::max(
                                iterationReport
                                    .boundaryTargetMaximum[axis],
                                coordinates[axis]);
                        if (std::abs(coordinates[axis]) > 1.0e-12) {
                            ++iterationReport.nonZeroBoundaryRhsCount;
                        }
                    }
                }
                const auto solution =
                    solveReducedBoundaryLockedSystem(
                        rows,
                        normalRows,
                        *boundedSupport,
                        iterationReport,
                        options);
                if (!solution.has_value()) {
                    iterationReport.stopReason =
                        iterationReport.estimatedSolverBytes >
                            static_cast<std::uint64_t>(
                                options.solverMemoryPreflightMiB) *
                                1024ull * 1024ull
                        ? "Reduced SPD joint-fit matrix exceeds the memory preflight limit."
                        : "Reduced SPD joint-fit factorization or solve failed.";
                    iterationReport.wallMilliseconds =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            iterationStarted).count();
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason =
                        iterationReport.stopReason;
                    break;
                }
                std::vector<Handle(Geom_BSplineSurface)> surfaces;
                std::string supportFailure;
                try {
                    surfaces.reserve(
                        static_cast<std::size_t>(layout.patchCount));
                    for (int patch = 0;
                         patch < layout.patchCount;
                         ++patch) {
                        surfaces.push_back(makeSurface(
                            knots[static_cast<std::size_t>(patch)],
                            *solution,
                            variableOffsets[
                                static_cast<std::size_t>(patch)]));
                    }
                    for (std::size_t patch = 0;
                         patch < surfaces.size();
                         ++patch) {
                        if (!regionLocalSupportDomainPassed(
                                surfaces[patch],
                                layoutCarrier,
                                layout.patches[patch],
                                options,
                                supportFailure)) {
                            surfaces.clear();
                            break;
                        }
                    }
                } catch (const Standard_Failure&) {
                    surfaces.clear();
                    supportFailure =
                        "Open CASCADE rejected the reduced SPD fitted network.";
                }
                if (surfaces.empty()) {
                    iterationReport.stopReason =
                        supportFailure.empty()
                        ? "Reduced SPD fitting produced no constructible support surface."
                        : supportFailure;
                    iterationReport.wallMilliseconds =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            iterationStarted).count();
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason =
                        iterationReport.stopReason;
                    break;
                }
                auto metrics = validateNetwork(
                    fittingReference,
                    layoutCarrier,
                    layout,
                    lookups,
                    surfaces,
                    options,
                    mappedSamples);
                validateFrozenCurveContinuity(
                    metrics,
                    layout,
                    *curveNetwork,
                    surfaces,
                    options);
                iterationReport.trainingMaximumDistance =
                    metrics.trainingMaximumDistance;
                iterationReport.projectionCount =
                    metrics.projectionCount;
                iterationReport.uvIterationCount =
                    metrics.uvIterationCount;
                iterationReport.projectionFallbackCount =
                    metrics.projectionFallbackCount;
                iterationReport.selectionMaximumDistance =
                    metrics.selectionMaximumDistance;
                iterationReport.holdoutMaximumDistance =
                    metrics.holdoutMaximumDistance;
                iterationReport.holdoutMaximumNormalDegrees =
                    metrics.holdoutMaximumNormalDegrees;
                iterationReport.holdout95Distance =
                    metrics.holdout95Distance;
                iterationReport.holdout95NormalDegrees =
                    metrics.holdout95NormalDegrees;
                iterationReport.holdoutWorstDistanceVertex =
                    metrics.holdoutWorstDistanceVertex;
                iterationReport.holdoutWorstNormalVertex =
                    metrics.holdoutWorstNormalVertex;
                iterationReport.holdoutWorstDistancePatch =
                    metrics.holdoutWorstDistancePatch;
                iterationReport.holdoutWorstNormalPatch =
                    metrics.holdoutWorstNormalPatch;
                iterationReport.holdoutWorstDistanceInternalSeam =
                    metrics.holdoutWorstDistanceInternalSeam;
                iterationReport.holdoutWorstNormalInternalSeam =
                    metrics.holdoutWorstNormalInternalSeam;
                iterationReport.boundaryMaximumDistance =
                    metrics.boundaryMaximumDistance;
                iterationReport.seamMaximumDistance =
                    metrics.seamMaximumDistance;
                iterationReport.seamMaximumCurveDeviation =
                    metrics.seamMaximumCurveDeviation;
                iterationReport.seamMaximumNormalDegrees =
                    metrics.seamMaximumNormalDegrees;
                iterationReport.seamMaximumCurvatureDifference =
                    metrics.seamMaximumCurvatureDifference;
                iterationReport.seamContinuityGatePassed =
                    metrics.seamContinuityGatePassed;
                const auto surfaceQuality = measureSurfaceQuality(
                    layoutCarrier, layout, surfaces);
                recordSurfaceQuality(iterationReport, surfaceQuality);
                const auto accepted =
                    fitGatesPassed(metrics, options) &&
                    !surfaceQuality.hardFailure &&
                    surfaceQuality.maximumPoleCountPerSide <=
                        std::min(
                            options.maximumControlPointsPerSide,
                            25);
                const auto diagnosticSurfaceRank =
                    [&](const ValidationMetrics& candidateMetrics,
                        const RegionSurfaceQualityMetrics&
                            candidateQuality) {
                        const auto productPassed =
                            static_cast<int>(
                                candidateMetrics.holdoutMaximumDistance <=
                                    options.tolerance3d) +
                            static_cast<int>(
                                candidateMetrics
                                    .holdoutMaximumNormalDegrees <=
                                    options.normalToleranceDegrees) +
                            static_cast<int>(
                                candidateMetrics.boundaryMaximumDistance <=
                                    options.curveOnSurfaceTolerance) +
                            static_cast<int>(
                                candidateMetrics.seamMaximumDistance <=
                                    options.curveOnSurfaceTolerance) +
                            static_cast<int>(
                                candidateMetrics
                                    .seamMaximumCurveDeviation <=
                                    options.curveOnSurfaceTolerance) +
                            static_cast<int>(
                                candidateMetrics
                                    .seamMaximumNormalDegrees <=
                                    options.g1ToleranceDegrees) +
                            static_cast<int>(
                                candidateQuality
                                    .maximumPoleCountPerSide <=
                                    std::min(
                                        options
                                            .maximumControlPointsPerSide,
                                        25));
                        return std::tuple {
                            candidateQuality.hardFailure ? 1 : 0,
                            -productPassed,
                            normalizedViolationScore(
                                candidateMetrics, options),
                            candidateMetrics
                                .seamMaximumNormalDegrees,
                            candidateMetrics.holdoutMaximumDistance,
                            candidateMetrics
                                .holdoutMaximumNormalDegrees,
                            candidateQuality.fairnessEnergy,
                            candidateQuality.curvatureOscillation,
                            candidateQuality.totalPoleCount};
                    };
                if (bestSurfaces.empty() ||
                    diagnosticSurfaceRank(metrics, surfaceQuality) <
                        diagnosticSurfaceRank(
                            bestMetrics, bestQuality)) {
                    bestSurfaces = surfaces;
                    bestMetrics = metrics;
                    bestQuality = surfaceQuality;
                    bestBoundaryLocked = true;
                }
                iterationReport.stopReason = accepted
                    ? "Reduced SPD fairness fit passed all geometry gates."
                    : "Reduced SPD fairness fit completed but did not pass every geometry gate.";
                iterationReport.wallMilliseconds =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() -
                        iterationStarted).count();
                attempt.iterations.push_back(iterationReport);
                attempt.stopReason = iterationReport.stopReason;
                if (accepted) {
                    attempt.success = true;
                    attempt.wallMilliseconds =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            attemptStarted).count();
                    result.attempts.push_back(attempt);
                    result.network = fittedNetwork(
                        layoutCarrier,
                        layout,
                        std::move(surfaces),
                        metrics,
                        options,
                        true,
                        true,
                        "ProductFit");
                    result.status = RegionRefitStatus::Success;
                    result.message =
                        "The reduced SPD boundary-locked fairness fit passed all fitting gates.";
                    return result;
                }
                break;
            }
            auto equalityRows = curveNetwork != nullptr
                ? buildFrozenCurveEqualityRows(
                    layout,
                    *curveNetwork,
                    knots,
                    variableOffsets)
                : buildBoundaryEqualityRows(
                    layoutCarrier,
                    constraints,
                    layout,
                    knots,
                    variableOffsets);
            const auto boundaryEqualityRowCount = curveNetwork != nullptr
                ? static_cast<std::size_t>(std::count_if(
                    equalityRows.begin(), equalityRows.end(),
                    [](const auto& row) { return row.originalBoundary; }))
                : equalityRows.size();
            auto seamEqualityRows = curveNetwork != nullptr
                ? std::vector<LinearRow> {}
                : buildSeamEqualityRows(
                    layout,
                    knots,
                    variableOffsets,
                    lookups,
                    seamConstraintSamples);
            iterationReport.boundaryHardConstraintCount = boundaryEqualityRowCount;
            iterationReport.seamG0ConstraintCount = curveNetwork != nullptr
                ? equalityRows.size() - boundaryEqualityRowCount
                : seamEqualityRows.size();
            iterationReport.seamG1SoftConstraintCount = seamG1RowCount;
            iterationReport.seamG2SoftConstraintCount = seamG2RowCount;
            if (boundaryEqualityRowCount > 0) {
                iterationReport.boundaryTargetMinimum = {
                    std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity()};
                iterationReport.boundaryTargetMaximum = {
                    -std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity()};
                for (std::size_t row = 0; row < equalityRows.size(); ++row) {
                    if (curveNetwork != nullptr &&
                        !equalityRows[row].originalBoundary) {
                        continue;
                    }
                    const std::array<double, 3> target {
                        equalityRows[row].target.X(),
                        equalityRows[row].target.Y(),
                        equalityRows[row].target.Z()};
                    for (std::size_t axis = 0; axis < target.size(); ++axis) {
                        iterationReport.boundaryTargetMinimum[axis] = std::min(
                            iterationReport.boundaryTargetMinimum[axis], target[axis]);
                        iterationReport.boundaryTargetMaximum[axis] = std::max(
                            iterationReport.boundaryTargetMaximum[axis], target[axis]);
                        if (std::abs(target[axis]) > 1.0e-12) {
                            ++iterationReport.nonZeroBoundaryRhsCount;
                        }
                    }
                }
            }
            equalityRows.insert(
                equalityRows.end(),
                std::make_move_iterator(seamEqualityRows.begin()),
                std::make_move_iterator(seamEqualityRows.end()));
            iterationReport.constraintRowsBefore = equalityRows.size();
            const auto fullConstraintFeasibility = assessConstraintFeasibility(
                equalityRows, controlVariableCount);
            iterationReport.minimumFullHardConstraintResidual =
                fullConstraintFeasibility.minimumMaximumResidual;
            iterationReport.boundaryFeasibilityRefinementRounds =
                boundaryFeasibilityRefinementRounds;
            iterationReport.boundaryFeasibilityResidual =
                fullConstraintFeasibility.minimumMaximumResidual;
            iterationReport.hardConstraintFeasible =
                equalityRows.empty() ||
                (fullConstraintFeasibility.estimatedRank > 0 &&
                    fullConstraintFeasibility.minimumMaximumResidual <=
                        options.curveOnSurfaceTolerance);
            iterationReport.hardConstraintStatus =
                equalityRows.empty()
                    ? "NoHardConstraints"
                    : iterationReport.hardConstraintFeasible
                    ? "Feasible"
                    : "InfeasibleHardBoundaryConstraints";
            if (!iterationReport.hardConstraintFeasible &&
                !bestSurfaces.empty() &&
                !finalIterationForBudget &&
                iteration + 1 < maximumIterations &&
                insertBoundaryFeasibilityKnots(
                    fullConstraintFeasibility.residuals,
                    knots,
                    options.maximumControlPointsPerSide,
                    options.curveOnSurfaceTolerance)) {
                ++boundaryFeasibilityRefinementRounds;
                iterationReport.boundaryFeasibilityRefinementRounds =
                    boundaryFeasibilityRefinementRounds;
                iterationReport.stopReason =
                    "Refined U and V knots while retaining the previously constructible diagnostic network.";
                iterationReport.wallMilliseconds =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() -
                        iterationStarted).count();
                attempt.iterations.push_back(iterationReport);
                continue;
            }
            if (!iterationReport.hardConstraintFeasible) {
                iterationReport.hardConstraintStatus =
                    "BoundaryInfeasibleDiagnostic";
            }
            auto samplesPerConstraintCurve = static_cast<std::size_t>(kDegree + 1);
            for (const auto& patchKnots : knots) {
                samplesPerConstraintCurve = std::max({
                    samplesPerConstraintCurve,
                    patchKnots.u.size(),
                    patchKnots.v.size()});
            }
            auto solveEqualityRows = compressConstraintRows(
                equalityRows, samplesPerConstraintCurve);
            if (!solveEqualityRows.empty()) {
                std::vector<Eigen::Triplet<double>> transposeTriplets;
                std::size_t equalityNonZeros = 0;
                for (const auto& row : solveEqualityRows) {
                    equalityNonZeros += row.coefficients.size();
                }
                transposeTriplets.reserve(equalityNonZeros);
                for (std::size_t row = 0; row < solveEqualityRows.size(); ++row) {
                    for (const auto& [column, value] : solveEqualityRows[row].coefficients) {
                        transposeTriplets.emplace_back(
                            column, static_cast<int>(row), value);
                    }
                }
                Eigen::SparseMatrix<double> equalityTranspose(
                    controlVariableCount, static_cast<int>(solveEqualityRows.size()));
                equalityTranspose.setFromTriplets(
                    transposeTriplets.begin(), transposeTriplets.end());
                Eigen::SparseQR<
                    Eigen::SparseMatrix<double>,
                    Eigen::COLAMDOrdering<int>> rankRevealingQr;
                auto independentRowTolerance =
                    std::sqrt(std::numeric_limits<double>::epsilon());
                if (boundaryEqualityRowCount > 0) {
                    const auto boundaryTargetDiagonal = std::sqrt(
                        std::pow(
                            iterationReport.boundaryTargetMaximum[0] -
                                iterationReport.boundaryTargetMinimum[0],
                            2.0) +
                        std::pow(
                            iterationReport.boundaryTargetMaximum[1] -
                                iterationReport.boundaryTargetMinimum[1],
                            2.0) +
                        std::pow(
                            iterationReport.boundaryTargetMaximum[2] -
                                iterationReport.boundaryTargetMinimum[2],
                            2.0));
                    independentRowTolerance = std::max(
                        independentRowTolerance,
                        options.curveOnSurfaceTolerance /
                            std::max(
                                boundaryTargetDiagonal,
                                options.curveOnSurfaceTolerance));
                }
                rankRevealingQr.setPivotThreshold(independentRowTolerance);
                rankRevealingQr.compute(equalityTranspose);
                iterationReport.constraintEstimatedRank =
                    rankRevealingQr.info() == Eigen::Success
                        ? static_cast<std::size_t>(rankRevealingQr.rank())
                        : 0;
                if (rankRevealingQr.info() == Eigen::Success &&
                    rankRevealingQr.rank() < static_cast<int>(solveEqualityRows.size())) {
                    auto independentRows = std::vector<LinearRow> {};
                    independentRows.reserve(
                        static_cast<std::size_t>(rankRevealingQr.rank()));
                    const auto& permutation = rankRevealingQr.colsPermutation().indices();
                    for (int column = 0; column < rankRevealingQr.rank(); ++column) {
                        independentRows.push_back(
                            solveEqualityRows[static_cast<std::size_t>(permutation[column])]);
                    }
                    solveEqualityRows = std::move(independentRows);
                }
            }
            iterationReport.constraintRowsAfter = solveEqualityRows.size();
            std::vector<Eigen::Triplet<double>> triplets;
            std::size_t nonZeros = 0;
            for (const auto& row : rows) nonZeros += row.coefficients.size() * 3;
            for (const auto& row : normalRows) nonZeros += row.coefficients.size() * 3;
            triplets.reserve(nonZeros);
            const auto scalarRowCount = rows.size() * 3 + normalRows.size();
            const auto coupledVariableCount = controlVariableCount * 3;
            Eigen::VectorXd targets(static_cast<int>(scalarRowCount));
            int scalarRow = 0;
            for (const auto& row : rows) {
                const std::array<double, 3> target {
                    row.target.X(), row.target.Y(), row.target.Z()};
                for (int axis = 0; axis < 3; ++axis) {
                    for (const auto& [column, value] : row.coefficients) {
                        triplets.emplace_back(scalarRow, column * 3 + axis, value);
                    }
                    targets[scalarRow] = target[static_cast<std::size_t>(axis)];
                    ++scalarRow;
                }
            }
            constexpr double normalWeight = 0.25;
            for (const auto& row : normalRows) {
                const std::array<double, 3> normal {
                    row.normal.X(), row.normal.Y(), row.normal.Z()};
                for (const auto& [column, value] : row.coefficients) {
                    for (int axis = 0; axis < 3; ++axis) {
                        triplets.emplace_back(
                            scalarRow,
                            column * 3 + axis,
                            value * normal[static_cast<std::size_t>(axis)] * normalWeight);
                    }
                }
                targets[scalarRow] = 0.0;
                ++scalarRow;
            }
            Eigen::SparseMatrix<double> matrix(
                static_cast<int>(scalarRowCount), coupledVariableCount);
            matrix.setFromTriplets(triplets.begin(), triplets.end());
            Eigen::VectorXd columnScales = Eigen::VectorXd::Zero(coupledVariableCount);
            for (int column = 0; column < matrix.outerSize(); ++column) {
                for (Eigen::SparseMatrix<double>::InnerIterator value(matrix, column); value; ++value) {
                    columnScales[column] += value.value() * value.value();
                }
            }
            for (const auto& row : equalityRows) {
                if (curveNetwork != nullptr && !row.originalBoundary) {
                    continue;
                }
                for (const auto& [column, value] : row.coefficients) {
                    for (int axis = 0; axis < 3; ++axis) {
                        columnScales[column * 3 + axis] += value * value;
                    }
                }
            }
            Eigen::SparseMatrix<double> scaledMatrix = matrix;
            for (int column = 0; column < scaledMatrix.outerSize(); ++column) {
                columnScales[column] = std::sqrt(std::max(columnScales[column], 1.0e-24));
                for (Eigen::SparseMatrix<double>::InnerIterator value(scaledMatrix, column); value; ++value) {
                    value.valueRef() /= columnScales[column];
                }
            }
            Eigen::SparseMatrix<double> normalMatrix = scaledMatrix.transpose() * scaledMatrix;
            for (int diagonal = 0; diagonal < coupledVariableCount; ++diagonal) {
                normalMatrix.coeffRef(diagonal, diagonal) += 1.0e-8;
            }
            normalMatrix.makeCompressed();
            Eigen::VectorXd normalTargets = scaledMatrix.transpose() * targets;
            std::vector<Handle(Geom_BSplineSurface)> initialDiagnosticSurfaces;
            ValidationMetrics initialDiagnosticMetrics;
            std::string initialDiagnosticFailure;
            std::optional<Eigen::MatrixXd> initialConstructibleSolution;
            auto boundaryRefinementNeeded = false;
            if (!equalityRows.empty()) {
                auto initialNormalMatrix = normalMatrix;
                auto initialNormalTargets = normalTargets;
                auto initialDiagonalScale = 0.0;
                for (int diagonal = 0; diagonal < coupledVariableCount; ++diagonal) {
                    initialDiagonalScale += std::max(
                        0.0, normalMatrix.coeff(diagonal, diagonal) - 1.0e-8);
                }
                initialDiagonalScale /=
                    static_cast<double>(std::max(1, coupledVariableCount));
                iterationReport.initialControlPriorScale =
                    iterationReport.hardConstraintFeasible
                        ? 0.0
                        : options.initialControlPriorRelativeWeight *
                            initialDiagonalScale;
                const auto initialControlPrior = buildInitialControlPrior(
                    fittingReference,
                    representativeFitSamples,
                    knots,
                    variableOffsets,
                    columnScales);
                for (int diagonal = 0; diagonal < coupledVariableCount; ++diagonal) {
                    initialNormalMatrix.coeffRef(diagonal, diagonal) +=
                        iterationReport.initialControlPriorScale;
                    initialNormalTargets[diagonal] +=
                        iterationReport.initialControlPriorScale *
                        initialControlPrior[diagonal];
                }
                initialNormalMatrix.makeCompressed();
                Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> initialSolver;
                initialSolver.compute(initialNormalMatrix);
                if (initialSolver.info() != Eigen::Success) {
                    iterationReport.stopReason =
                        "Initial local support fit factorization failed.";
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason = iterationReport.stopReason;
                    break;
                }
                const Eigen::VectorXd initialScaledSolution =
                    initialSolver.solve(initialNormalTargets);
                if (initialSolver.info() != Eigen::Success ||
                    !initialScaledSolution.allFinite()) {
                    iterationReport.stopReason = "Initial local support fit solve failed.";
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason = iterationReport.stopReason;
                    break;
                }
                Eigen::MatrixXd initialFitSolution(controlVariableCount, 3);
                for (int variable = 0; variable < controlVariableCount; ++variable) {
                    for (int axis = 0; axis < 3; ++axis) {
                        const auto coupled = variable * 3 + axis;
                        initialFitSolution(variable, axis) =
                            initialScaledSolution[coupled] / columnScales[coupled];
                    }
                }
                const auto affineBaseline = buildAffineControlBaseline(
                    fittingReference,
                    layoutCarrier,
                    layout,
                    representativeFitSamples,
                    knots,
                    variableOffsets,
                    controlVariableCount);
                if (iterationReport.hardConstraintFeasible) {
                    const auto boundarySolution = projectOntoConstraints(
                        solveEqualityRows,
                        controlVariableCount,
                        initialFitSolution);
                    if (boundarySolution.has_value()) {
                        auto boundaryBaseline = largestConstructibleBlend(
                            *boundarySolution,
                            *boundarySolution,
                            knots,
                            variableOffsets,
                            layoutCarrier,
                            layout,
                            options);
                        if (boundaryBaseline.available) {
                            iterationReport.initialSupportBlendAlpha = 1.0;
                            initialDiagnosticMetrics = validateNetwork(
                                fittingReference,
                                layoutCarrier,
                                layout,
                                lookups,
                                boundaryBaseline.surfaces,
                                options,
                                mappedSamples);
                            if (curveNetwork != nullptr) {
                                validateFrozenCurveContinuity(
                                    initialDiagnosticMetrics,
                                    layout,
                                    *curveNetwork,
                                    boundaryBaseline.surfaces,
                                    options);
                            }
                            initialConstructibleSolution =
                                std::move(boundaryBaseline.solution);
                            initialDiagnosticSurfaces =
                                std::move(boundaryBaseline.surfaces);
                            initialDiagnosticFailure =
                                boundaryBaseline.failure;
                        } else {
                            initialDiagnosticFailure =
                                boundaryBaseline.failure;
                        }
                    } else {
                        initialDiagnosticFailure =
                            "Could not project the data-fit control network onto all hard curve constraints.";
                    }
                    boundaryRefinementNeeded = !initialConstructibleSolution;
                }
                boundaryRefinementNeeded =
                    boundaryRefinementNeeded ||
                    !iterationReport.hardConstraintFeasible;
                if (boundaryRefinementNeeded &&
                    affineBaseline &&
                    curveNetwork == nullptr) {
                    const auto baseline = largestConstructibleBlend(
                        *affineBaseline,
                        *affineBaseline,
                        knots,
                        variableOffsets,
                        layoutCarrier,
                        layout,
                        options);
                    if (baseline.available) {
                        auto baselineMetrics = validateNetwork(
                            fittingReference,
                            layoutCarrier,
                            layout,
                            lookups,
                            baseline.surfaces,
                            options,
                            mappedSamples);
                        if (curveNetwork != nullptr) {
                            validateFrozenCurveContinuity(
                                baselineMetrics,
                                layout,
                                *curveNetwork,
                                baseline.surfaces,
                                options);
                        }
                        iterationReport.affineBaselineTrainingMaximumDistance =
                            baselineMetrics.trainingMaximumDistance;
                        iterationReport.affineBaselineBoundaryMaximumDistance =
                            baselineMetrics.boundaryMaximumDistance;
                    }
                    auto initialBlend = largestConstructibleBlend(
                        *affineBaseline,
                        initialFitSolution,
                        knots,
                        variableOffsets,
                        layoutCarrier,
                        layout,
                        options);
                    if (initialBlend.available) {
                        iterationReport.initialSupportBlendAlpha = initialBlend.alpha;
                        initialDiagnosticMetrics = validateNetwork(
                            fittingReference,
                            layoutCarrier,
                            layout,
                            lookups,
                            initialBlend.surfaces,
                            options,
                            mappedSamples);
                        if (curveNetwork != nullptr) {
                            validateFrozenCurveContinuity(
                                initialDiagnosticMetrics,
                                layout,
                                *curveNetwork,
                                initialBlend.surfaces,
                                options);
                        }
                        initialConstructibleSolution = std::move(initialBlend.solution);
                        initialDiagnosticSurfaces = std::move(initialBlend.surfaces);
                        initialDiagnosticFailure = initialBlend.failure;
                    } else {
                        initialDiagnosticFailure = initialBlend.failure;
                    }
                } else if (boundaryRefinementNeeded) {
                    initialDiagnosticFailure =
                        "Could not construct a non-degenerate affine local support baseline.";
                }
                if (initialConstructibleSolution) {
                    iterationReport.initialTrainingMaximumDistance =
                        initialDiagnosticMetrics.trainingMaximumDistance;
                    iterationReport.initialBoundaryMaximumDistance =
                        initialDiagnosticMetrics.boundaryMaximumDistance;
                }
                if (boundaryRefinementNeeded) {
                    auto refinementSamples =
                        fullConstraintFeasibility.residuals;
                    if (refinementSamples.empty() ||
                        std::all_of(
                            refinementSamples.begin(),
                            refinementSamples.end(),
                            [](const auto& sample) {
                                return sample.error <= 1.0e-12;
                            })) {
                        for (const auto& row : equalityRows) {
                            if (row.sourcePatch >= 0) {
                                refinementSamples.push_back({
                                    row.sourcePatch,
                                    row.sourceUv[0],
                                    row.sourceUv[1],
                                    1.0});
                            }
                        }
                    }
                    if (!finalIterationForBudget &&
                        iteration + 1 < maximumIterations &&
                        insertBoundaryFeasibilityKnots(
                            refinementSamples,
                            knots,
                            options.maximumControlPointsPerSide,
                            options.curveOnSurfaceTolerance)) {
                        if (!initialDiagnosticSurfaces.empty()) {
                            bestSurfaces = initialDiagnosticSurfaces;
                            bestMetrics = initialDiagnosticMetrics;
                            bestBoundaryLocked =
                                iterationReport.hardConstraintFeasible;
                        }
                        ++boundaryFeasibilityRefinementRounds;
                        iterationReport.boundaryFeasibilityRefinementRounds =
                            boundaryFeasibilityRefinementRounds;
                        iterationReport.stopReason =
                            "Refined U and V knots because the boundary-feasible control network was not constructible: " +
                            initialDiagnosticFailure;
                        iterationReport.wallMilliseconds =
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                iterationStarted).count();
                        attempt.iterations.push_back(iterationReport);
                        continue;
                    }
                    iterationReport.hardConstraintFeasible = false;
                    iterationReport.hardConstraintStatus =
                        "BoundaryInfeasibleDiagnostic";
                }

                auto diagonalScale = 0.0;
                for (int diagonal = 0; diagonal < coupledVariableCount; ++diagonal) {
                    diagonalScale += std::max(
                        0.0, normalMatrix.coeff(diagonal, diagonal) - 1.0e-8);
                }
                diagonalScale /= static_cast<double>(std::max(1, coupledVariableCount));
                iterationReport.constraintCorrectionAnchorScale =
                    options.constraintCorrectionAnchorRelativeWeight * diagonalScale;
                Eigen::VectorXd correctionAnchor = initialScaledSolution;
                if (initialConstructibleSolution) {
                    for (int variable = 0; variable < controlVariableCount; ++variable) {
                        for (int axis = 0; axis < 3; ++axis) {
                            const auto coupled = variable * 3 + axis;
                            correctionAnchor[coupled] =
                                (*initialConstructibleSolution)(variable, axis) *
                                columnScales[coupled];
                        }
                    }
                }
                for (int diagonal = 0; diagonal < coupledVariableCount; ++diagonal) {
                    normalMatrix.coeffRef(diagonal, diagonal) +=
                        iterationReport.constraintCorrectionAnchorScale;
                    normalTargets[diagonal] +=
                        iterationReport.constraintCorrectionAnchorScale *
                        correctionAnchor[diagonal];
                }
                normalMatrix.makeCompressed();
            }
            const auto equalityScalarRowCount = solveEqualityRows.size() * 3;
            iterationReport.matrixRows = scalarRowCount + equalityScalarRowCount;
            iterationReport.matrixColumns = static_cast<std::size_t>(coupledVariableCount);
            Eigen::SparseMatrix<double> equalityMatrix;
            Eigen::SparseMatrix<double> augmentedMatrix;
            std::vector<double> equalityRowScales;
            auto augmentedSize = coupledVariableCount;
            if (!solveEqualityRows.empty()) {
                std::vector<Eigen::Triplet<double>> equalityTriplets;
                std::size_t equalityNonZeros = 0;
                for (const auto& row : solveEqualityRows) {
                    equalityNonZeros += row.coefficients.size() * 3;
                }
                equalityTriplets.reserve(equalityNonZeros);
                equalityRowScales.reserve(equalityScalarRowCount);
                auto equalityRow = 0;
                for (const auto& row : solveEqualityRows) {
                    for (int axis = 0; axis < 3; ++axis) {
                        auto squaredRowNorm = 0.0;
                        for (const auto& [column, value] : row.coefficients) {
                            const auto scaledValue =
                                value / columnScales[column * 3 + axis];
                            squaredRowNorm += scaledValue * scaledValue;
                        }
                        const auto rowScale = std::sqrt(std::max(
                            squaredRowNorm, std::numeric_limits<double>::min()));
                        equalityRowScales.push_back(rowScale);
                        for (const auto& [column, value] : row.coefficients) {
                            const auto coupledColumn = column * 3 + axis;
                            equalityTriplets.emplace_back(
                                equalityRow,
                                coupledColumn,
                                value / columnScales[coupledColumn] / rowScale);
                        }
                        ++equalityRow;
                    }
                }
                equalityMatrix.resize(static_cast<int>(equalityScalarRowCount), coupledVariableCount);
                equalityMatrix.setFromTriplets(equalityTriplets.begin(), equalityTriplets.end());
                equalityMatrix.makeCompressed();

                std::vector<Eigen::Triplet<double>> augmentedTriplets;
                augmentedTriplets.reserve(
                    static_cast<std::size_t>(normalMatrix.nonZeros()) +
                    static_cast<std::size_t>(equalityMatrix.nonZeros()) * 2);
                for (int column = 0; column < normalMatrix.outerSize(); ++column) {
                    for (Eigen::SparseMatrix<double>::InnerIterator value(normalMatrix, column); value; ++value) {
                        augmentedTriplets.emplace_back(
                            static_cast<int>(value.row()),
                            static_cast<int>(value.col()),
                            value.value());
                    }
                }
                for (int column = 0; column < equalityMatrix.outerSize(); ++column) {
                    for (Eigen::SparseMatrix<double>::InnerIterator value(equalityMatrix, column); value; ++value) {
                        augmentedTriplets.emplace_back(
                            static_cast<int>(value.col()),
                            coupledVariableCount + static_cast<int>(value.row()),
                            value.value());
                        augmentedTriplets.emplace_back(
                            coupledVariableCount + static_cast<int>(value.row()),
                            static_cast<int>(value.col()),
                            value.value());
                    }
                }
                augmentedSize += static_cast<int>(equalityScalarRowCount);
                augmentedMatrix.resize(augmentedSize, augmentedSize);
                augmentedMatrix.setFromTriplets(augmentedTriplets.begin(), augmentedTriplets.end());
                augmentedMatrix.makeCompressed();
            }
            iterationReport.matrixNonZeros = static_cast<std::size_t>(
                matrix.nonZeros() + equalityMatrix.nonZeros());
            iterationReport.estimatedSolverBytes =
                static_cast<std::uint64_t>(matrix.nonZeros() + equalityMatrix.nonZeros()) * 16ull +
                static_cast<std::uint64_t>(
                    solveEqualityRows.empty() ? normalMatrix.nonZeros() : augmentedMatrix.nonZeros()) * 16ull *
                    (solveEqualityRows.empty() ? 8ull : 12ull) +
                static_cast<std::uint64_t>(augmentedSize) * 256ull;
            const auto preflightLimit = static_cast<std::uint64_t>(options.solverMemoryPreflightMiB) * 1024ull * 1024ull;
            if (iterationReport.estimatedSolverBytes > preflightLimit) {
                iterationReport.stopReason = "Joint-fit matrix exceeds the memory preflight limit.";
                attempt.iterations.push_back(iterationReport);
                attempt.stopReason = iterationReport.stopReason;
                result.attempts.push_back(std::move(attempt));
                result.status = RegionRefitStatus::MemoryBudgetExceeded;
                result.message = "A selected layout exceeded the joint-fit memory preflight limit.";
                return result;
            }
            if (std::chrono::steady_clock::now() >= attemptDeadline) {
                if (!iterationReport.hardConstraintFeasible &&
                    !initialDiagnosticSurfaces.empty()) {
                    bestSurfaces = initialDiagnosticSurfaces;
                    bestMetrics = initialDiagnosticMetrics;
                    iterationReport.constraintCorrectionBlendAlpha = 0.0;
                    iterationReport.stopReason =
                        iterationReport.hardConstraintStatus +
                        "; single-fit deadline reached before constrained factorization, so the constructible initial network was retained for diagnostic sewing.";
                } else {
                    iterationReport.stopReason =
                        "Single-fit deadline reached before sparse factorization.";
                }
                attempt.iterations.push_back(iterationReport);
                attempt.stopReason = iterationReport.stopReason;
                break;
            }
            Eigen::VectorXd coupledSolution(coupledVariableCount);
            iterationReport.symbolicFactorizationReuseCount = 0;
            if (solveEqualityRows.empty()) {
                Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
                solver.compute(normalMatrix);
                if (solver.info() != Eigen::Success) {
                    iterationReport.stopReason = "Joint-fit symbolic or numeric factorization failed.";
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason = iterationReport.stopReason;
                    break;
                }
                coupledSolution = solver.solve(normalTargets);
                if (solver.info() != Eigen::Success || !coupledSolution.allFinite()) {
                    iterationReport.stopReason = "Joint-fit solve failed.";
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason = iterationReport.stopReason;
                    break;
                }
            } else {
                Eigen::SparseLU<Eigen::SparseMatrix<double>> solver;
                solver.analyzePattern(augmentedMatrix);
                solver.factorize(augmentedMatrix);
                if (solver.info() != Eigen::Success) {
                    if (!iterationReport.hardConstraintFeasible &&
                        !initialDiagnosticSurfaces.empty()) {
                        bestSurfaces = initialDiagnosticSurfaces;
                        bestMetrics = initialDiagnosticMetrics;
                        iterationReport.constraintCorrectionBlendAlpha = 0.0;
                        iterationReport.stopReason =
                            iterationReport.hardConstraintStatus +
                            "; constrained factorization failed, so the constructible initial network was retained for diagnostic sewing.";
                    } else {
                        iterationReport.stopReason =
                            "Constrained joint-fit factorization failed.";
                    }
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason = iterationReport.stopReason;
                    break;
                }
                Eigen::VectorXd augmentedTargets = Eigen::VectorXd::Zero(augmentedSize);
                augmentedTargets.head(coupledVariableCount) = normalTargets;
                auto equalityScalarRow = 0;
                for (const auto& row : solveEqualityRows) {
                    const std::array<double, 3> target {
                        row.target.X(), row.target.Y(), row.target.Z()};
                    for (int axis = 0; axis < 3; ++axis) {
                        augmentedTargets[coupledVariableCount + equalityScalarRow] =
                            target[static_cast<std::size_t>(axis)] /
                            equalityRowScales[static_cast<std::size_t>(equalityScalarRow)];
                        ++equalityScalarRow;
                    }
                }
                Eigen::VectorXd augmentedSolution = solver.solve(augmentedTargets);
                if (solver.info() != Eigen::Success || !augmentedSolution.allFinite()) {
                    if (!iterationReport.hardConstraintFeasible &&
                        !initialDiagnosticSurfaces.empty()) {
                        bestSurfaces = initialDiagnosticSurfaces;
                        bestMetrics = initialDiagnosticMetrics;
                        iterationReport.constraintCorrectionBlendAlpha = 0.0;
                        iterationReport.stopReason =
                            iterationReport.hardConstraintStatus +
                            "; constrained solve failed, so the constructible initial network was retained for diagnostic sewing.";
                    } else {
                        iterationReport.stopReason =
                            "Constrained joint-fit solve failed.";
                    }
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason = iterationReport.stopReason;
                    break;
                }
                coupledSolution = augmentedSolution.head(coupledVariableCount);
            }
            for (int variable = 0; variable < coupledVariableCount; ++variable) {
                coupledSolution[variable] /= columnScales[variable];
            }
            Eigen::MatrixXd solution(controlVariableCount, 3);
            for (int variable = 0; variable < controlVariableCount; ++variable) {
                for (int axis = 0; axis < 3; ++axis) {
                    solution(variable, axis) = coupledSolution[variable * 3 + axis];
                }
            }
            std::vector<Handle(Geom_BSplineSurface)> surfaces;
            std::string supportFailure;
            if (initialConstructibleSolution) {
                auto correctedBlend = largestConstructibleBlend(
                    *initialConstructibleSolution,
                    solution,
                    knots,
                    variableOffsets,
                    layoutCarrier,
                    layout,
                    options);
                if (correctedBlend.available) {
                    iterationReport.constraintCorrectionBlendAlpha =
                        correctedBlend.alpha;
                    solution = std::move(correctedBlend.solution);
                    surfaces = std::move(correctedBlend.surfaces);
                    supportFailure = correctedBlend.failure;
                } else {
                    supportFailure = correctedBlend.failure;
                }
            }
            for (const auto& row : equalityRows) {
                const std::array<double, 3> target {
                    row.target.X(), row.target.Y(), row.target.Z()};
                for (int axis = 0; axis < 3; ++axis) {
                    auto predicted = 0.0;
                    for (const auto& [column, value] : row.coefficients) {
                        predicted += value * solution(column, axis);
                    }
                    iterationReport.maximumHardConstraintResidual = std::max(
                        iterationReport.maximumHardConstraintResidual,
                        std::abs(predicted - target[static_cast<std::size_t>(axis)]));
                }
            }
            if (iterationReport.maximumHardConstraintResidual >
                options.curveOnSurfaceTolerance) {
                iterationReport.hardConstraintFeasible = false;
                if (iterationReport.hardConstraintStatus !=
                    "BoundaryInfeasibleDiagnostic") {
                    iterationReport.hardConstraintStatus =
                        "InfeasibleHardBoundaryConstraints";
                }
            }
            try {
                if (surfaces.empty()) {
                    surfaces.reserve(static_cast<std::size_t>(layout.patchCount));
                    for (int patch = 0; patch < layout.patchCount; ++patch) {
                        surfaces.push_back(makeSurface(
                            knots[static_cast<std::size_t>(patch)],
                            solution,
                            variableOffsets[static_cast<std::size_t>(patch)]));
                    }
                    for (std::size_t patch = 0; patch < surfaces.size(); ++patch) {
                        if (!regionLocalSupportDomainPassed(
                                surfaces[patch],
                                layoutCarrier,
                                layout.patches[patch],
                                options,
                                supportFailure)) {
                            surfaces.clear();
                            break;
                        }
                    }
                }
                if (surfaces.empty()) {
                    if (!iterationReport.hardConstraintFeasible &&
                        !initialDiagnosticSurfaces.empty()) {
                        bestSurfaces = initialDiagnosticSurfaces;
                        bestMetrics = initialDiagnosticMetrics;
                        iterationReport.stopReason =
                            iterationReport.hardConstraintStatus +
                            "; retained the constructible initial support network for diagnostic sewing.";
                    } else {
                        iterationReport.stopReason = supportFailure;
                        if (!initialDiagnosticFailure.empty()) {
                            iterationReport.stopReason +=
                                " Initial support: " +
                                initialDiagnosticFailure;
                        }
                    }
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason = iterationReport.stopReason;
                    break;
                }
                auto metrics = validateNetwork(
                    fittingReference,
                    layoutCarrier,
                    layout,
                    lookups,
                    surfaces,
                    options,
                    mappedSamples);
                if (curveNetwork != nullptr) {
                    validateFrozenCurveContinuity(
                        metrics,
                        layout,
                        *curveNetwork,
                        surfaces,
                        options);
                }
                iterationReport.trainingMaximumDistance = metrics.trainingMaximumDistance;
                iterationReport.projectionCount = metrics.projectionCount;
                iterationReport.uvIterationCount = metrics.uvIterationCount;
                iterationReport.projectionFallbackCount = metrics.projectionFallbackCount;
                iterationReport.selectionMaximumDistance = metrics.selectionMaximumDistance;
                iterationReport.holdoutMaximumDistance = metrics.holdoutMaximumDistance;
                iterationReport.holdoutMaximumNormalDegrees = metrics.holdoutMaximumNormalDegrees;
                iterationReport.holdout95Distance = metrics.holdout95Distance;
                iterationReport.holdout95NormalDegrees = metrics.holdout95NormalDegrees;
                iterationReport.holdoutWorstDistanceVertex = metrics.holdoutWorstDistanceVertex;
                iterationReport.holdoutWorstNormalVertex = metrics.holdoutWorstNormalVertex;
                iterationReport.holdoutWorstDistancePatch = metrics.holdoutWorstDistancePatch;
                iterationReport.holdoutWorstNormalPatch = metrics.holdoutWorstNormalPatch;
                iterationReport.holdoutWorstDistanceInternalSeam = metrics.holdoutWorstDistanceInternalSeam;
                iterationReport.holdoutWorstNormalInternalSeam = metrics.holdoutWorstNormalInternalSeam;
                iterationReport.boundaryMaximumDistance = metrics.boundaryMaximumDistance;
                iterationReport.seamMaximumDistance = metrics.seamMaximumDistance;
                iterationReport.seamMaximumCurveDeviation =
                    metrics.seamMaximumCurveDeviation;
                iterationReport.seamMaximumNormalDegrees =
                    metrics.seamMaximumNormalDegrees;
                iterationReport.seamMaximumCurvatureDifference =
                    metrics.seamMaximumCurvatureDifference;
                iterationReport.seamContinuityGatePassed =
                    metrics.seamContinuityGatePassed;
                if (equalityRows.empty()) {
                    iterationReport.initialTrainingMaximumDistance = metrics.trainingMaximumDistance;
                    iterationReport.initialBoundaryMaximumDistance = metrics.boundaryMaximumDistance;
                }
                iterationReport.correctedTrainingMaximumDistance = metrics.trainingMaximumDistance;
                iterationReport.correctedBoundaryMaximumDistance = metrics.boundaryMaximumDistance;
                const auto surfaceQuality = measureSurfaceQuality(
                    layoutCarrier, layout, surfaces);
                recordSurfaceQuality(iterationReport, surfaceQuality);
                const auto accepted =
                    fitGatesPassed(metrics, options) &&
                    iterationReport.hardConstraintFeasible &&
                    !surfaceQuality.hardFailure &&
                    surfaceQuality.maximumPoleCountPerSide <=
                        std::min(
                            options.maximumControlPointsPerSide,
                            25);
                if (bestSurfaces.empty() || normalizedViolationScore(metrics, options) <
                        normalizedViolationScore(bestMetrics, options)) {
                    bestSurfaces = surfaces;
                    bestMetrics = metrics;
                    bestBoundaryLocked =
                        iterationReport.hardConstraintFeasible;
                }
                if (accepted) {
                    iterationReport.stopReason = "All fit, boundary, seam, and holdout gates passed.";
                    iterationReport.wallMilliseconds = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - iterationStarted).count();
                    attempt.iterations.push_back(iterationReport);
                    attempt.success = true;
                    attempt.stopReason = iterationReport.stopReason;
                    attempt.wallMilliseconds = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - attemptStarted).count();
                    result.attempts.push_back(attempt);
                    result.network = fittedNetwork(
                        layoutCarrier,
                        layout,
                        std::move(surfaces),
                        metrics,
                        options,
                        true,
                        true,
                        "ProductFit");
                    result.status = RegionRefitStatus::Success;
                    result.message = "The smallest fully fitted layout passed all fitting gates.";
                    return result;
                }
                if (!iterationReport.hardConstraintFeasible) {
                    iterationReport.stopReason =
                        iterationReport.hardConstraintStatus +
                        "; retained the maximum constructible correction blend for diagnostic sewing.";
                    attempt.stopReason = iterationReport.stopReason;
                } else {
                    const auto improvement = std::isfinite(previousSelection) && previousSelection > 0.0
                        ? (previousSelection - metrics.selectionMaximumScore) / previousSelection
                        : 1.0;
                    lowImprovementRounds = iteration >= maximumIterations - 3 && improvement < 0.01
                        ? lowImprovementRounds + 1 : 0;
                    previousSelection = metrics.selectionMaximumScore;
                    if (lowImprovementRounds >= 2) {
                        iterationReport.stopReason = "Two consecutive mature-grid rounds improved less than 1%.";
                        attempt.stopReason = iterationReport.stopReason;
                    } else if (!insertResidualKnot(
                            metrics.selectionResiduals,
                            knots,
                            options.tolerance3d * 0.5,
                            options.maximumControlPointsPerSide)) {
                        iterationReport.stopReason = "No additional residual-driven knot could be inserted.";
                        attempt.stopReason = iterationReport.stopReason;
                    } else {
                        iterationReport.stopReason = "Inserted one knot at the concentrated selection residual.";
                    }
                }
            } catch (const Standard_Failure&) {
                iterationReport.stopReason = "Open CASCADE rejected the fitted B-spline network.";
                attempt.stopReason = iterationReport.stopReason;
            }
            iterationReport.wallMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - iterationStarted).count();
            attempt.iterations.push_back(iterationReport);
            if (lowImprovementRounds >= 2 ||
                iterationReport.stopReason != "Inserted one knot at the concentrated selection residual.") {
                break;
            }
        }
        attempt.wallMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - attemptStarted).count();
        if (attempt.stopReason.empty()) {
            attempt.stopReason = "Fit iteration budget exhausted without passing all gates.";
        }
        if (!boundaryFallbackFailure.empty() &&
            attempt.stopReason.find(boundaryFallbackFailure) ==
                std::string::npos) {
            attempt.stopReason +=
                " Boundary-locked Coons fallback unavailable: " +
                boundaryFallbackFailure;
        }
        if (!bestSurfaces.empty()) {
            auto candidate = fittedNetwork(
                layoutCarrier,
                layout,
                std::move(bestSurfaces),
                bestMetrics,
                options,
                false,
                bestBoundaryLocked,
                bestBoundaryLocked
                    ? "BoundaryLockedDiagnostic"
                    : "UnconstrainedInspectionOnly");
            if (result.bestEffortNetwork == nullptr ||
                diagnosticRank(*candidate) <
                    diagnosticRank(*result.bestEffortNetwork)) {
                result.bestEffortNetwork = std::move(candidate);
            }
        }
        result.attempts.push_back(std::move(attempt));
    }
    result.status = std::chrono::steady_clock::now() >= deadline
        ? RegionRefitStatus::TimeBudgetExceeded
        : RegionRefitStatus::NoValidatedLayoutWithinBudget;
    result.message = result.status == RegionRefitStatus::TimeBudgetExceeded
        ? "Joint fitting reached its cumulative deadline."
        : "No coarse-screened layout passed the full joint fitting gates.";
    return result;
}

JointBSplineNetworkFitResult JointBSplineNetworkFitter::fit(
    const RegionReferenceMesh& fittingReference,
    const RegionReferenceMesh& layoutCarrier,
    const RegionTopologyConstraints& constraints,
    const std::vector<RegionPatchLayout>& candidates,
    const RegionRefitOptions& options,
    const std::chrono::steady_clock::time_point deadline) const {
    return fit(
        fittingReference,
        layoutCarrier,
        constraints,
        candidates,
        {},
        options,
        deadline);
}

JointBSplineNetworkFitResult JointBSplineNetworkFitter::fit(
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const std::vector<RegionPatchLayout>& candidates,
    const RegionRefitOptions& options,
    const std::chrono::steady_clock::time_point deadline) const {
    return fit(
        mesh,
        mesh,
        constraints,
        candidates,
        {},
        options,
        deadline);
}

}
