#include "reduction/region_refit/JointBSplineNetworkFitter.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Builder.hxx>
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

struct PatchLookup {
    std::unordered_map<std::size_t, std::size_t> localByGlobal;
};

struct SeamSample {
    int firstPatch = -1;
    int secondPatch = -1;
    std::size_t vertex = 0;
};

struct LinearRow {
    std::vector<std::pair<int, double>> coefficients;
    gp_Pnt target;
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

std::array<double, 2> normalizedParameter(const std::array<double, 2>& uv) {
    return {
        std::clamp((uv[0] + 1.0) * 0.5, 0.0, 1.0),
        std::clamp((uv[1] + 1.0) * 0.5, 0.0, 1.0)};
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

std::vector<SeamSample> buildSeamSamples(
    const RegionPatchLayout& layout) {
    std::vector<SeamSample> result;
    for (const auto& seam : layout.seams) {
        std::set<std::size_t> vertices;
        for (const auto& edge : seam.meshEdges) {
            vertices.insert(edge[0]);
            vertices.insert(edge[1]);
        }
        for (const auto vertex : vertices) {
            result.push_back({seam.firstPatch, seam.secondPatch, vertex});
        }
    }
    return result;
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

std::vector<LinearRow> buildSystemRows(
    const RegionReferenceMesh& mesh,
    const RegionPatchLayout& layout,
    const std::vector<KnotConfiguration>& knots,
    const std::vector<int>& variableOffsets,
    const std::vector<PatchLookup>& lookups,
    const std::vector<SeamSample>& seamSamples,
    const std::vector<Handle(Geom_BSplineSurface)>& linearizationSurfaces) {
    std::vector<LinearRow> rows;
    rows.reserve(mesh.vertices.size() * 3 + mesh.boundarySamples.size() + seamSamples.size());
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        const auto& parameterization = layout.patches[patch];
        const auto& patchKnots = knots[patch];
        const auto variableOffset = variableOffsets[patch];
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            const auto vertexIndex = parameterization.vertexIndices[local];
            const auto& vertex = mesh.vertices[vertexIndex];
            const auto bucket = static_cast<int>((vertexIndex * 2654435761ull + patch * 40503ull) % 10ull);
            if (!vertex.boundary && bucket >= 7) continue;
            const auto uv = normalizedParameter(parameterization.parameters[local]);
            appendWeightedRow(
                rows,
                surfaceCoefficients(uv, patchKnots, variableOffset),
                vertex.point,
                vertex.boundary ? 100.0 : 1.0);
            if (linearizationSurfaces.size() == layout.patches.size()) {
                gp_Pnt fittedPoint;
                gp_Vec tangentU;
                gp_Vec tangentV;
                linearizationSurfaces[patch]->D1(uv[0], uv[1], fittedPoint, tangentU, tangentV);
                const gp_Vec referenceNormal(vertex.normal);
                tangentU -= referenceNormal * tangentU.Dot(referenceNormal);
                tangentV -= referenceNormal * tangentV.Dot(referenceNormal);
                const auto tangentWeight = vertex.boundary ? 0.05 : 0.01;
                appendWeightedRow(
                    rows,
                    surfaceDerivativeCoefficients(uv, patchKnots, variableOffset, 0),
                    gp_Pnt(tangentU.X(), tangentU.Y(), tangentU.Z()),
                    tangentWeight);
                appendWeightedRow(
                    rows,
                    surfaceDerivativeCoefficients(uv, patchKnots, variableOffset, 1),
                    gp_Pnt(tangentV.X(), tangentV.Y(), tangentV.Z()),
                    tangentWeight);
            }
        }
    }
    for (const auto& seam : seamSamples) {
        const auto firstFound = lookups[static_cast<std::size_t>(seam.firstPatch)].localByGlobal.find(seam.vertex);
        const auto secondFound = lookups[static_cast<std::size_t>(seam.secondPatch)].localByGlobal.find(seam.vertex);
        if (firstFound == lookups[static_cast<std::size_t>(seam.firstPatch)].localByGlobal.end() ||
            secondFound == lookups[static_cast<std::size_t>(seam.secondPatch)].localByGlobal.end()) {
            continue;
        }
        const auto firstUv = normalizedParameter(
            layout.patches[static_cast<std::size_t>(seam.firstPatch)].parameters[firstFound->second]);
        const auto secondUv = normalizedParameter(
            layout.patches[static_cast<std::size_t>(seam.secondPatch)].parameters[secondFound->second]);
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
    const RegionReferenceMesh& mesh,
    const RegionPatchLayout& layout,
    const std::vector<PatchLookup>& lookups,
    const std::vector<Handle(Geom_BSplineSurface)>& surfaces) {
    ValidationMetrics metrics;
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        const auto& parameterization = layout.patches[patch];
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            const auto vertexIndex = parameterization.vertexIndices[local];
            const auto& vertex = mesh.vertices[vertexIndex];
            const auto uv = normalizedParameter(parameterization.parameters[local]);
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
            const auto distance = projection.distance;
            if (vertex.boundary) {
                metrics.boundaryMaximumDistance = std::max(metrics.boundaryMaximumDistance, distance);
                continue;
            }
            if (bucket < 9) {
                metrics.selectionMaximumDistance = std::max(metrics.selectionMaximumDistance, distance);
                metrics.selectionResiduals.push_back({
                    static_cast<int>(patch), projection.u, projection.v, distance});
            } else {
                metrics.holdoutDistances.push_back(distance);
                if (distance > metrics.holdoutMaximumDistance) {
                    metrics.holdoutMaximumDistance = distance;
                    metrics.holdoutWorstDistanceVertex = vertexIndex;
                    metrics.holdoutWorstDistancePatch = static_cast<int>(patch);
                    metrics.holdoutWorstDistanceInternalSeam = vertex.internalSeam;
                }
                auto fittedNormal = projection.du.Crossed(projection.dv);
                const auto normalDegrees = normalAngleDegrees(fittedNormal, vertex.normal);
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
    for (const auto& seam : layout.seams) {
        std::set<std::size_t> vertices;
        for (const auto& edge : seam.meshEdges) {
            vertices.insert(edge[0]);
            vertices.insert(edge[1]);
        }
        for (const auto vertex : vertices) {
            const auto firstLocal = lookups[static_cast<std::size_t>(seam.firstPatch)].localByGlobal.at(vertex);
            const auto secondLocal = lookups[static_cast<std::size_t>(seam.secondPatch)].localByGlobal.at(vertex);
            const auto firstUv = normalizedParameter(
                layout.patches[static_cast<std::size_t>(seam.firstPatch)].parameters[firstLocal]);
            const auto secondUv = normalizedParameter(
                layout.patches[static_cast<std::size_t>(seam.secondPatch)].parameters[secondLocal]);
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

struct ParameterUpdateStats {
    std::size_t projectionCount = 0;
    std::size_t uvIterationCount = 0;
    std::size_t projectionFallbackCount = 0;
};

ParameterUpdateStats updateClosestPointParameters(
    const RegionReferenceMesh& mesh,
    RegionPatchLayout& layout,
    const std::vector<Handle(Geom_BSplineSurface)>& surfaces) {
    ParameterUpdateStats stats;
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        auto& parameterization = layout.patches[patch];
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            const auto initial = normalizedParameter(parameterization.parameters[local]);
            const auto projection = projectToSurface(
                surfaces[patch],
                mesh.vertices[parameterization.vertexIndices[local]].point,
                initial);
            ++stats.projectionCount;
            stats.uvIterationCount += projection.iterationCount;
            stats.projectionFallbackCount += projection.fallbackCount;
            if (std::isfinite(projection.u) && std::isfinite(projection.v)) {
                parameterization.parameters[local] = {
                    projection.u * 2.0 - 1.0,
                    projection.v * 2.0 - 1.0};
            }
        }
    }
    return stats;
}

bool insertResidualKnot(
    const std::vector<ResidualSample>& residuals,
    std::vector<KnotConfiguration>& knots,
    const double minimumError) {
    if (residuals.empty()) return false;
    const auto median = [](std::vector<double> values) {
        const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
        std::nth_element(values.begin(), middle, values.end());
        return *middle;
    };
    const auto insert = [](std::vector<double>& target, double value) {
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

    bool insertedAny = false;
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
        auto& patchKnots = knots[static_cast<std::size_t>(targetPatch)];
        const auto insertedU = insert(patchKnots.u, uMedian);
        const auto insertedV = insert(patchKnots.v, vMedian);
        insertedAny = insertedAny || insertedU || insertedV;
    }
    return insertedAny;
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

}

bool JointBSplineNetworkFitResult::success() const {
    return status == RegionRefitStatus::Success && network != nullptr;
}

JointBSplineNetworkFitResult JointBSplineNetworkFitter::fit(
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const std::vector<RegionPatchLayout>& candidates,
    const RegionRefitOptions& options,
    const std::chrono::steady_clock::time_point deadline) const {
    (void)constraints;
    JointBSplineNetworkFitResult result;
    const auto maximumAttempts = std::min<std::size_t>(
        candidates.size(), static_cast<std::size_t>(options.maximumFullFitCount));
    for (std::size_t candidateIndex = 0; candidateIndex < maximumAttempts; ++candidateIndex) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.status = RegionRefitStatus::TimeBudgetExceeded;
            result.message = "Joint fitting reached the cumulative fit deadline.";
            return result;
        }
        const auto attemptStarted = std::chrono::steady_clock::now();
        const auto attemptDeadline = std::min(
            deadline,
            attemptStarted + std::chrono::milliseconds(options.singleFitBudgetMilliseconds));
        RegionPatchLayout workingLayout = candidates[candidateIndex];
        const auto& layout = candidates[candidateIndex];
        RegionFitAttemptReport attempt;
        attempt.patchCount = layout.patchCount;
        attempt.layoutHash = layout.hash;
        const auto lookups = buildLookups(workingLayout);
        std::vector<KnotConfiguration> knots(static_cast<std::size_t>(layout.patchCount));
        double previousSelection = std::numeric_limits<double>::infinity();
        int lowImprovementRounds = 0;
        std::vector<Handle(Geom_BSplineSurface)> bestSurfaces;
        std::vector<Handle(Geom_BSplineSurface)> linearizationSurfaces;
        ValidationMetrics bestMetrics;

        const auto maximumIterations = std::min(options.maximumFitIterations, 20);
        for (int iteration = 0; iteration < maximumIterations; ++iteration) {
            const auto iterationStarted = std::chrono::steady_clock::now();
            RegionFitIterationReport iterationReport;
            iterationReport.iteration = iteration + 1;
            for (const auto& patchKnots : knots) {
                iterationReport.uControlCount = std::max(
                    iterationReport.uControlCount, patchKnots.uControlCount());
                iterationReport.vControlCount = std::max(
                    iterationReport.vControlCount, patchKnots.vControlCount());
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
            const auto seamSamples = buildSeamSamples(workingLayout);
            const auto variableCount = controlVariableCount;
            const auto rows = buildSystemRows(
                mesh,
                workingLayout,
                knots,
                variableOffsets,
                lookups,
                seamSamples,
                linearizationSurfaces);
            std::vector<Eigen::Triplet<double>> triplets;
            std::size_t nonZeros = 0;
            for (const auto& row : rows) nonZeros += row.coefficients.size();
            triplets.reserve(nonZeros);
            Eigen::MatrixXd targets(static_cast<int>(rows.size()), 3);
            for (std::size_t row = 0; row < rows.size(); ++row) {
                for (const auto& [column, value] : rows[row].coefficients) {
                    triplets.emplace_back(static_cast<int>(row), column, value);
                }
                targets(static_cast<int>(row), 0) = rows[row].target.X();
                targets(static_cast<int>(row), 1) = rows[row].target.Y();
                targets(static_cast<int>(row), 2) = rows[row].target.Z();
            }
            Eigen::SparseMatrix<double> matrix(static_cast<int>(rows.size()), variableCount);
            matrix.setFromTriplets(triplets.begin(), triplets.end());
            Eigen::VectorXd columnScales = Eigen::VectorXd::Zero(variableCount);
            for (int column = 0; column < matrix.outerSize(); ++column) {
                for (Eigen::SparseMatrix<double>::InnerIterator value(matrix, column); value; ++value) {
                    columnScales[column] += value.value() * value.value();
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
            for (int diagonal = 0; diagonal < variableCount; ++diagonal) {
                normalMatrix.coeffRef(diagonal, diagonal) += 1.0e-8;
            }
            normalMatrix.makeCompressed();
            iterationReport.matrixRows = rows.size();
            iterationReport.matrixColumns = static_cast<std::size_t>(variableCount);
            iterationReport.matrixNonZeros = static_cast<std::size_t>(matrix.nonZeros());
            iterationReport.estimatedSolverBytes =
                static_cast<std::uint64_t>(matrix.nonZeros()) * 16ull +
                static_cast<std::uint64_t>(normalMatrix.nonZeros()) * 16ull * 8ull +
                static_cast<std::uint64_t>(variableCount) * 256ull;
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
                iterationReport.stopReason = "Single-fit deadline reached before sparse factorization.";
                attempt.iterations.push_back(iterationReport);
                attempt.stopReason = iterationReport.stopReason;
                break;
            }
            Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
            solver.compute(normalMatrix);
            if (solver.info() != Eigen::Success) {
                iterationReport.stopReason = "Joint-fit symbolic or numeric factorization failed.";
                attempt.iterations.push_back(iterationReport);
                attempt.stopReason = iterationReport.stopReason;
                break;
            }
            const Eigen::MatrixXd normalTargets = scaledMatrix.transpose() * targets;
            Eigen::MatrixXd solution = solver.solve(normalTargets);
                iterationReport.symbolicFactorizationReuseCount = 2;
            if (solver.info() != Eigen::Success || !solution.allFinite()) {
                iterationReport.stopReason = "Joint-fit solve failed.";
                attempt.iterations.push_back(iterationReport);
                attempt.stopReason = iterationReport.stopReason;
                break;
            }
            for (int variable = 0; variable < variableCount; ++variable) {
                solution.row(variable) /= columnScales[variable];
            }
            std::vector<Handle(Geom_BSplineSurface)> surfaces;
            surfaces.reserve(static_cast<std::size_t>(layout.patchCount));
            try {
                for (int patch = 0; patch < layout.patchCount; ++patch) {
                    surfaces.push_back(makeSurface(
                        knots[static_cast<std::size_t>(patch)],
                        solution,
                        variableOffsets[static_cast<std::size_t>(patch)]));
                }
                const auto metrics = validateNetwork(mesh, workingLayout, lookups, surfaces);
                linearizationSurfaces = surfaces;
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
                const auto accepted = metrics.holdoutMaximumDistance <= options.tolerance3d &&
                    metrics.holdoutMaximumNormalDegrees <= options.normalToleranceDegrees &&
                    metrics.boundaryMaximumDistance <= options.curveOnSurfaceTolerance &&
                    metrics.seamMaximumDistance <= options.curveOnSurfaceTolerance;
                if (bestSurfaces.empty() || metrics.selectionMaximumDistance < bestMetrics.selectionMaximumDistance) {
                    bestSurfaces = surfaces;
                    bestMetrics = metrics;
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
                    auto network = std::make_shared<RegionFittedPatchNetwork>();
                    network->layout = workingLayout;
                    network->surfaces = std::move(surfaces);
                    network->inspectionShape = inspectionShape(network->surfaces);
                    network->holdoutMaximumDistance = metrics.holdoutMaximumDistance;
                    network->holdoutMaximumNormalDegrees = metrics.holdoutMaximumNormalDegrees;
                    network->boundaryMaximumDistance = metrics.boundaryMaximumDistance;
                    network->seamMaximumDistance = metrics.seamMaximumDistance;
                    result.network = std::move(network);
                    result.status = RegionRefitStatus::Success;
                    result.message = "The smallest fully fitted layout passed all fitting gates.";
                    return result;
                }
                const auto improvement = std::isfinite(previousSelection) && previousSelection > 0.0
                    ? (previousSelection - metrics.selectionMaximumDistance) / previousSelection
                    : 1.0;
                lowImprovementRounds = iteration >= maximumIterations - 3 && improvement < 0.01
                    ? lowImprovementRounds + 1 : 0;
                previousSelection = metrics.selectionMaximumDistance;
                const auto parameterUpdate = updateClosestPointParameters(
                    mesh, workingLayout, surfaces);
                iterationReport.projectionCount += parameterUpdate.projectionCount;
                iterationReport.uvIterationCount += parameterUpdate.uvIterationCount;
                iterationReport.projectionFallbackCount += parameterUpdate.projectionFallbackCount;
                if (lowImprovementRounds >= 2) {
                    iterationReport.stopReason = "Two consecutive mature-grid rounds improved less than 1%.";
                    attempt.stopReason = iterationReport.stopReason;
                } else if (!insertResidualKnot(
                        metrics.selectionResiduals,
                        knots,
                        options.tolerance3d * 0.5)) {
                    iterationReport.stopReason = "No additional residual-driven knot could be inserted.";
                    attempt.stopReason = iterationReport.stopReason;
                } else {
                    iterationReport.stopReason = "Inserted one knot at the concentrated selection residual.";
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

}
