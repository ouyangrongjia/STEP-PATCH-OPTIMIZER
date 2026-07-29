#include "reduction/region_refit/SurfaceDifferentialQuality.h"

#include <Standard_Failure.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <unordered_map>

namespace spo {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDerivativeTolerance = 1.0e-14;

double percentile(std::vector<double> values, const double probability) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(
        std::clamp(probability, 0.0, 1.0) *
        static_cast<double>(values.size() - 1)));
    return values[index];
}

void reject(
    SurfaceLegalityReport& report,
    const std::string& reason) {
    report.passed = false;
    if (std::find(
            report.rejectionReasons.begin(),
            report.rejectionReasons.end(),
            reason) == report.rejectionReasons.end()) {
        report.rejectionReasons.push_back(reason);
    }
}

bool finitePoint(const gp_Pnt& point) {
    return std::isfinite(point.X()) &&
        std::isfinite(point.Y()) &&
        std::isfinite(point.Z());
}

void checkKnots(
    const Handle(Geom_BSplineSurface)& surface,
    SurfaceLegalityReport& legality) {
    TColStd_Array1OfReal uKnots(1, surface->NbUKnots());
    TColStd_Array1OfReal vKnots(1, surface->NbVKnots());
    surface->UKnots(uKnots);
    surface->VKnots(vKnots);
    for (int index = 2; index <= uKnots.Upper(); ++index) {
        if (!(uKnots.Value(index) > uKnots.Value(index - 1))) {
            legality.strictlyIncreasingKnots = false;
        }
    }
    for (int index = 2; index <= vKnots.Upper(); ++index) {
        if (!(vKnots.Value(index) > vKnots.Value(index - 1))) {
            legality.strictlyIncreasingKnots = false;
        }
    }
    if (!legality.strictlyIncreasingKnots) {
        reject(legality, "NonIncreasingKnotVector");
    }
}

}

bool orientPatchTriangles(
    const RegionReferenceMesh& mesh,
    const RegionPatchParameterization& patch,
    std::vector<int>& orientations,
    std::string& message) {
    if (patch.triangleIndices.empty()) {
        message = "Parameter domain contains no triangles.";
        return false;
    }
    struct DirectedUse {
        std::size_t triangle = 0;
        int direction = 0;
    };
    std::map<std::array<std::size_t, 2>,
             std::vector<DirectedUse>> uses;
    for (const auto triangleIndex : patch.triangleIndices) {
        if (triangleIndex >= mesh.triangles.size()) {
            message =
                "Parameter domain references an invalid triangle.";
            return false;
        }
        const auto& triangle = mesh.triangles[triangleIndex];
        for (std::size_t edge = 0;
             edge < triangle.vertices.size();
             ++edge) {
            const auto first = triangle.vertices[edge];
            const auto second = triangle.vertices[
                (edge + 1) % triangle.vertices.size()];
            const std::array<std::size_t, 2> key {
                std::min(first, second),
                std::max(first, second)};
            uses[key].push_back({
                triangleIndex,
                first < second ? 1 : -1});
        }
    }
    orientations.assign(mesh.triangles.size(), 0);
    const auto seed = patch.triangleIndices.front();
    orientations[seed] = 1;
    std::queue<std::size_t> pending;
    pending.push(seed);
    while (!pending.empty()) {
        const auto currentTriangle = pending.front();
        pending.pop();
        const auto& triangle = mesh.triangles[currentTriangle];
        for (std::size_t edge = 0;
             edge < triangle.vertices.size();
             ++edge) {
            const auto first = triangle.vertices[edge];
            const auto second = triangle.vertices[
                (edge + 1) % triangle.vertices.size()];
            const std::array<std::size_t, 2> key {
                std::min(first, second),
                std::max(first, second)};
            const auto& edgeUses = uses.at(key);
            if (edgeUses.size() != 2) continue;
            const auto& current =
                edgeUses[0].triangle == currentTriangle
                ? edgeUses[0]
                : edgeUses[1];
            const auto& other =
                edgeUses[0].triangle == currentTriangle
                ? edgeUses[1]
                : edgeUses[0];
            const auto expected =
                -current.direction *
                orientations[currentTriangle] *
                other.direction;
            if (orientations[other.triangle] == 0) {
                orientations[other.triangle] = expected;
                pending.push(other.triangle);
            } else if (
                orientations[other.triangle] != expected) {
                message =
                    "Parameter-domain triangles are not orientable.";
                return false;
            }
        }
    }
    if (std::any_of(
            patch.triangleIndices.begin(),
            patch.triangleIndices.end(),
            [&](const auto triangle) {
                return orientations[triangle] == 0;
            })) {
        message =
            "Parameter-domain triangles are disconnected.";
        return false;
    }
    return true;
}

std::string classifySurfaceUvTopology(
    const SurfaceLegalityReport& report) {
    if (report.uvFlippedTriangleCount > 0 &&
        report.uvDegenerateTriangleCount > 0) {
        return "FlippedAndDegenerateUvTriangle";
    }
    if (report.uvFlippedTriangleCount > 0) {
        return "FlippedUvTriangle";
    }
    if (report.uvDegenerateTriangleCount > 0) {
        return "DegenerateUvTriangle";
    }
    return "Passed";
}

std::string classifySurfaceJacobianLegality(
    const SurfaceLegalityReport& report) {
    if (report.jacobianOrientationFlipCount > 0 &&
        report.jacobianDegenerateSampleCount > 0) {
        return "SurfaceOrientationFlipAndDegenerateJacobian";
    }
    if (report.jacobianOrientationFlipCount > 0) {
        return "SurfaceOrientationFlip";
    }
    if (report.jacobianDegenerateSampleCount > 0) {
        return "DegenerateSurfaceJacobian";
    }
    return "Passed";
}

SurfaceDifferentialQualityReport evaluateSurfaceDifferentialQuality(
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    const std::vector<Handle(Geom_BSplineSurface)>& surfaces,
    const int samplesPerDirection) {
    SurfaceDifferentialQualityReport result;
    auto& legality = result.legality;
    const auto sampleCount = std::max(samplesPerDirection, 5);
    std::vector<double> conditionNumbers;
    double bendingIntegral = 0.0;
    double normalGradientIntegral = 0.0;
    double curvatureVariationIntegral = 0.0;
    double controlNetEnergy = 0.0;
    std::size_t controlNetTermCount = 0;

    for (std::size_t patchIndex = 0;
         patchIndex < surfaces.size();
         ++patchIndex) {
        const auto& surface = surfaces[patchIndex];
        if (surface.IsNull()) {
            reject(legality, "NullSurface");
            continue;
        }
        if (surface->UDegree() < 1 || surface->VDegree() < 1 ||
            surface->NbUPoles() <= surface->UDegree() ||
            surface->NbVPoles() <= surface->VDegree()) {
            legality.validDegreesAndPoles = false;
            reject(legality, "InvalidDegreeOrPoleCount");
        }
        checkKnots(surface, legality);
        for (int u = 1; u <= surface->NbUPoles(); ++u) {
            for (int v = 1; v <= surface->NbVPoles(); ++v) {
                if (!finitePoint(surface->Pole(u, v))) {
                    legality.finiteControlNet = false;
                    reject(legality, "NonFiniteControlPoint");
                }
                const auto weight = surface->Weight(u, v);
                if (!std::isfinite(weight) || weight <= 0.0) {
                    legality.positiveWeights = false;
                    reject(legality, "NonPositiveOrNonFiniteWeight");
                }
            }
        }
        const auto addControlNetTerm = [&](const gp_Pnt& first,
                                           const gp_Pnt& middle,
                                           const gp_Pnt& last) {
            const auto second =
                first.XYZ() - middle.XYZ() * 2.0 + last.XYZ();
            const auto energy = second.SquareModulus();
            if (std::isfinite(energy)) {
                controlNetEnergy += energy;
                ++controlNetTermCount;
            } else {
                reject(legality, "NonFiniteControlNetEnergy");
            }
        };
        for (int u = 2; u < surface->NbUPoles(); ++u) {
            for (int v = 1; v <= surface->NbVPoles(); ++v) {
                addControlNetTerm(
                    surface->Pole(u - 1, v),
                    surface->Pole(u, v),
                    surface->Pole(u + 1, v));
            }
        }
        for (int u = 1; u <= surface->NbUPoles(); ++u) {
            for (int v = 2; v < surface->NbVPoles(); ++v) {
                addControlNetTerm(
                    surface->Pole(u, v - 1),
                    surface->Pole(u, v),
                    surface->Pole(u, v + 1));
            }
        }

        double uFirst = 0.0;
        double uLast = 0.0;
        double vFirst = 0.0;
        double vLast = 0.0;
        surface->Bounds(uFirst, uLast, vFirst, vLast);
        const auto duParameter =
            (uLast - uFirst) / static_cast<double>(sampleCount);
        const auto dvParameter =
            (vLast - vFirst) / static_cast<double>(sampleCount);
        std::vector<gp_Dir> previousRowNormals;
        std::vector<gp_Pnt> previousRowPoints;
        std::vector<double> previousRowCurvatures;
        previousRowNormals.reserve(sampleCount);
        previousRowPoints.reserve(sampleCount);
        previousRowCurvatures.reserve(sampleCount);
        gp_Dir referenceNormal(0.0, 0.0, 1.0);
        bool referenceNormalSet = false;
        for (int uIndex = 0; uIndex < sampleCount; ++uIndex) {
            std::vector<gp_Dir> rowNormals;
            std::vector<gp_Pnt> rowPoints;
            std::vector<double> rowCurvatures;
            rowNormals.reserve(sampleCount);
            rowPoints.reserve(sampleCount);
            rowCurvatures.reserve(sampleCount);
            for (int vIndex = 0; vIndex < sampleCount; ++vIndex) {
                const auto u =
                    uFirst + (static_cast<double>(uIndex) + 0.5) *
                        duParameter;
                const auto v =
                    vFirst + (static_cast<double>(vIndex) + 0.5) *
                        dvParameter;
                gp_Pnt point;
                gp_Vec du;
                gp_Vec dv;
                gp_Vec duu;
                gp_Vec duv;
                gp_Vec dvv;
                try {
                    surface->D2(
                        u, v, point, du, dv, duu, duv, dvv);
                } catch (const Standard_Failure&) {
                    ++legality.jacobianDegenerateSampleCount;
                    reject(legality, "SurfaceDerivativeEvaluationFailed");
                    rowNormals.emplace_back(0.0, 0.0, 1.0);
                    rowPoints.emplace_back();
                    rowCurvatures.push_back(0.0);
                    continue;
                }
                const auto e = du.SquareMagnitude();
                const auto f = du.Dot(dv);
                const auto g = dv.SquareMagnitude();
                const auto determinant = e * g - f * f;
                if (!finitePoint(point) ||
                    !std::isfinite(determinant) ||
                    e <= kDerivativeTolerance ||
                    g <= kDerivativeTolerance ||
                    determinant <= kDerivativeTolerance *
                        std::max(e * g, 1.0)) {
                    ++legality.jacobianDegenerateSampleCount;
                    reject(legality, "DegenerateSurfaceJacobian");
                    rowNormals.emplace_back(0.0, 0.0, 1.0);
                    rowPoints.push_back(point);
                    rowCurvatures.push_back(0.0);
                    continue;
                }
                const auto trace = e + g;
                const auto discriminant = std::sqrt(std::max(
                    0.0, trace * trace - 4.0 * determinant));
                const auto largest = 0.5 * (trace + discriminant);
                const auto smallest = 0.5 * (trace - discriminant);
                conditionNumbers.push_back(
                    std::sqrt(largest / smallest));
                const auto normalizedJacobian =
                    std::sqrt(determinant / (e * g));
                legality.minimumNormalizedJacobian = std::min(
                    legality.minimumNormalizedJacobian,
                    normalizedJacobian);
                const gp_Dir normal(du.Crossed(dv));
                if (!referenceNormalSet) {
                    referenceNormal = normal;
                    referenceNormalSet = true;
                } else if (normal.Dot(referenceNormal) <= 0.0) {
                    ++legality.jacobianOrientationFlipCount;
                    reject(legality, "SurfaceOrientationFlip");
                }
                const auto l = duu.Dot(gp_Vec(normal));
                const auto m = duv.Dot(gp_Vec(normal));
                const auto n = dvv.Dot(gp_Vec(normal));
                const auto mean =
                    (g * l - 2.0 * f * m + e * n) /
                    (2.0 * determinant);
                const auto gaussian = (l * n - m * m) / determinant;
                const auto principalSquareSum = std::max(
                    0.0, 4.0 * mean * mean - 2.0 * gaussian);
                const auto areaWeight =
                    std::sqrt(determinant) *
                    duParameter * dvParameter;
                result.physicalArea += areaWeight;
                bendingIntegral += principalSquareSum * areaWeight;
                if (!rowNormals.empty()) {
                    const auto distance =
                        point.Distance(rowPoints.back());
                    if (distance > kDerivativeTolerance) {
                        const auto angle = std::acos(std::clamp(
                            normal.Dot(rowNormals.back()), -1.0, 1.0));
                        normalGradientIntegral +=
                            angle * angle / (distance * distance) *
                            areaWeight;
                        curvatureVariationIntegral +=
                            std::pow(
                                mean - rowCurvatures.back(), 2.0) /
                            (distance * distance) * areaWeight;
                        result.maximumNormalVariationDegrees = std::max(
                            result.maximumNormalVariationDegrees,
                            angle * 180.0 / kPi);
                    }
                }
                if (vIndex < static_cast<int>(previousRowNormals.size())) {
                    const auto distance =
                        point.Distance(previousRowPoints[vIndex]);
                    if (distance > kDerivativeTolerance) {
                        const auto angle = std::acos(std::clamp(
                            normal.Dot(previousRowNormals[vIndex]),
                            -1.0,
                            1.0));
                        normalGradientIntegral +=
                            angle * angle / (distance * distance) *
                            areaWeight;
                        curvatureVariationIntegral +=
                            std::pow(
                                mean - previousRowCurvatures[vIndex], 2.0) /
                            (distance * distance) * areaWeight;
                        result.maximumNormalVariationDegrees = std::max(
                            result.maximumNormalVariationDegrees,
                            angle * 180.0 / kPi);
                    }
                }
                rowNormals.push_back(normal);
                rowPoints.push_back(point);
                rowCurvatures.push_back(mean);
            }
            previousRowNormals = std::move(rowNormals);
            previousRowPoints = std::move(rowPoints);
            previousRowCurvatures = std::move(rowCurvatures);
        }

        if (patchIndex >= layout.patches.size()) {
            reject(legality, "MissingPatchParameterization");
            continue;
        }
        const auto& patch = layout.patches[patchIndex];
        const auto& parameters =
            patch.surfaceParameters.size() == patch.vertexIndices.size()
            ? patch.surfaceParameters
            : patch.parameters;
        std::unordered_map<std::size_t, std::size_t> localByGlobal;
        for (std::size_t local = 0;
             local < patch.vertexIndices.size();
             ++local) {
            localByGlobal.emplace(patch.vertexIndices[local], local);
        }
        std::vector<int> triangleOrientations;
        std::string orientationMessage;
        if (!orientPatchTriangles(
                layoutCarrier,
                patch,
                triangleOrientations,
                orientationMessage)) {
            reject(legality, "NonOrientableUvTriangleMesh");
            continue;
        }
        std::vector<double> signedAreas;
        for (const auto triangleIndex : patch.triangleIndices) {
            if (triangleIndex >= layoutCarrier.triangles.size()) continue;
            const auto& triangle =
                layoutCarrier.triangles[triangleIndex];
            std::array<std::size_t, 3> local {};
            auto foundAll = true;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto found =
                    localByGlobal.find(triangle.vertices[corner]);
                if (found == localByGlobal.end() ||
                    found->second >= parameters.size()) {
                    foundAll = false;
                    break;
                }
                local[corner] = found->second;
            }
            if (!foundAll) {
                reject(legality, "IncompleteUvTriangleMapping");
                continue;
            }
            const auto& a = parameters[local[0]];
            const auto& b = parameters[local[1]];
            const auto& c = parameters[local[2]];
            signedAreas.push_back(
                static_cast<double>(
                    triangleOrientations[triangleIndex]) *
                ((b[0] - a[0]) * (c[1] - a[1]) -
                    (b[1] - a[1]) * (c[0] - a[0])));
        }
        const auto positive = std::count_if(
            signedAreas.begin(), signedAreas.end(),
            [](const auto area) { return area > 1.0e-14; });
        const auto negative = std::count_if(
            signedAreas.begin(), signedAreas.end(),
            [](const auto area) { return area < -1.0e-14; });
        legality.uvDegenerateTriangleCount += static_cast<int>(
            signedAreas.size() - positive - negative);
        legality.uvFlippedTriangleCount += static_cast<int>(
            std::min(positive, negative));
    }

    if (legality.uvDegenerateTriangleCount > 0) {
        reject(legality, "DegenerateUvTriangle");
    }
    if (legality.uvFlippedTriangleCount > 0) {
        reject(legality, "FlippedUvTriangle");
    }
    if (!std::isfinite(legality.minimumNormalizedJacobian)) {
        legality.minimumNormalizedJacobian = 0.0;
        reject(legality, "NonFiniteNormalizedJacobian");
    }
    result.legacyControlNetSecondDifferenceEnergy =
        controlNetTermCount > 0
        ? controlNetEnergy / static_cast<double>(controlNetTermCount)
        : 0.0;
    if (result.physicalArea > 0.0) {
        result.areaWeightedBendingEnergy =
            bendingIntegral / result.physicalArea;
        result.areaWeightedNormalGradientEnergy =
            normalGradientIntegral / result.physicalArea;
        result.areaWeightedMeanCurvatureVariation =
            curvatureVariationIntegral / result.physicalArea;
    } else {
        reject(legality, "ZeroPhysicalArea");
    }
    result.conditionNumberP50 = percentile(conditionNumbers, 0.50);
    result.conditionNumberP95 = percentile(conditionNumbers, 0.95);
    result.conditionNumberP99 = percentile(conditionNumbers, 0.99);
    return result;
}

}
