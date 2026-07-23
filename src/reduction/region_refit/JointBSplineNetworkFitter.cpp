#include "reduction/region_refit/JointBSplineNetworkFitter.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseLU>
#include <Eigen/SparseQR>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
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

struct PatchLookup {
    std::unordered_map<std::size_t, std::size_t> localByGlobal;
};

struct SeamSample {
    int seamIndex = -1;
    int firstPatch = -1;
    int secondPatch = -1;
    std::size_t vertex = 0;
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
        std::clamp(uv[0], 0.0, 1.0),
        std::clamp(uv[1], 0.0, 1.0)};
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

std::vector<MappedReferenceSample> mapReferenceSamples(
    const RegionReferenceMesh& fittingReference,
    const RegionReferenceMesh& layoutCarrier,
    const RegionPatchLayout& layout,
    const std::vector<PatchLookup>& lookups) {
    std::unordered_map<FaceId, std::vector<std::size_t>> carrierByFace;
    for (std::size_t vertex = 0; vertex < layoutCarrier.vertices.size(); ++vertex) {
        for (const auto& source : layoutCarrier.vertices[vertex].sources) {
            carrierByFace[source.faceId].push_back(vertex);
        }
    }
    for (auto& [faceId, vertices] : carrierByFace) {
        (void)faceId;
        std::sort(vertices.begin(), vertices.end());
        vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
    }

    std::vector<std::vector<std::pair<int, std::size_t>>> patchLocals(layoutCarrier.vertices.size());
    for (std::size_t patch = 0; patch < lookups.size(); ++patch) {
        for (const auto& [global, local] : lookups[patch].localByGlobal) {
            if (global < patchLocals.size()) {
                patchLocals[global].emplace_back(static_cast<int>(patch), local);
            }
        }
    }
    for (auto& candidates : patchLocals) std::sort(candidates.begin(), candidates.end());

    std::vector<MappedReferenceSample> mapped;
    mapped.reserve(fittingReference.vertices.size());
    for (std::size_t referenceVertex = 0; referenceVertex < fittingReference.vertices.size(); ++referenceVertex) {
        const auto& reference = fittingReference.vertices[referenceVertex];
        std::vector<std::size_t> candidates;
        for (const auto& source : reference.sources) {
            const auto found = carrierByFace.find(source.faceId);
            if (found != carrierByFace.end()) {
                candidates.insert(candidates.end(), found->second.begin(), found->second.end());
            }
        }
        if (candidates.empty()) {
            candidates.resize(layoutCarrier.vertices.size());
            std::iota(candidates.begin(), candidates.end(), std::size_t {0});
        } else {
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        }
        auto closest = layoutCarrier.vertices.size();
        auto distance = std::numeric_limits<double>::infinity();
        for (const auto carrierVertex : candidates) {
            if (carrierVertex >= layoutCarrier.vertices.size() || patchLocals[carrierVertex].empty()) continue;
            const auto candidate = reference.point.SquareDistance(layoutCarrier.vertices[carrierVertex].point);
            if (candidate < distance) {
                closest = carrierVertex;
                distance = candidate;
            }
        }
        if (closest >= layoutCarrier.vertices.size() || patchLocals[closest].empty()) continue;
        const auto [patch, local] = patchLocals[closest].front();
        mapped.push_back({
            referenceVertex,
            closest,
            patch,
            normalizedParameter(layout.patches[static_cast<std::size_t>(patch)].parameters[local])});
    }
    return mapped;
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
    for (std::size_t seamIndex = 0; seamIndex < layout.seams.size(); ++seamIndex) {
        const auto& seam = layout.seams[seamIndex];
        std::set<std::size_t> vertices;
        for (const auto& edge : seam.meshEdges) {
            vertices.insert(edge[0]);
            vertices.insert(edge[1]);
        }
        for (const auto vertex : vertices) {
            result.push_back({
                static_cast<int>(seamIndex), seam.firstPatch, seam.secondPatch, vertex});
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
                layout.patches[firstPatch].parameters[selectedLocal]);
            auto next = samples.size();
            auto maximumDistance = -1.0;
            for (std::size_t index = 0; index < samples.size(); ++index) {
                const auto local = lookups[firstPatch].localByGlobal.at(samples[index]->vertex);
                const auto uv = normalizedParameter(layout.patches[firstPatch].parameters[local]);
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
    std::size_t& seamG1RowCount) {
    const auto& mesh = layoutCarrier;
    std::vector<LinearRow> rows;
    rows.reserve(mesh.vertices.size() * 3 + mesh.boundarySamples.size() + seamSamples.size());
    std::vector<double> areaWeights;
    std::vector<double> boundaryWeights;
    areaWeights.reserve(mesh.vertices.size());
    boundaryWeights.reserve(mesh.boundarySamples.size());
    for (const auto& vertex : mesh.vertices) {
        if (vertex.boundary) boundaryWeights.push_back(vertex.boundaryLengthWeight);
        else areaWeights.push_back(vertex.areaWeight);
    }
    const auto medianAreaWeight = positiveMedian(std::move(areaWeights));
    const auto medianBoundaryWeight = positiveMedian(std::move(boundaryWeights));
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        const auto& parameterization = layout.patches[patch];
        const auto& patchKnots = knots[patch];
        const auto variableOffset = variableOffsets[patch];
        std::vector<std::set<std::size_t>> neighbors(parameterization.vertexIndices.size());
        for (const auto triangleIndex : parameterization.triangleIndices) {
            const auto& triangle = mesh.triangles[triangleIndex];
            for (int edge = 0; edge < 3; ++edge) {
                const auto first = lookups[patch].localByGlobal.at(
                    triangle.vertices[static_cast<std::size_t>(edge)]);
                const auto second = lookups[patch].localByGlobal.at(
                    triangle.vertices[static_cast<std::size_t>((edge + 1) % 3)]);
                neighbors[first].insert(second);
                neighbors[second].insert(first);
            }
        }
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            const auto vertexIndex = parameterization.vertexIndices[local];
            const auto& vertex = mesh.vertices[vertexIndex];
            const auto bucket = static_cast<int>((vertexIndex * 2654435761ull + patch * 40503ull) % 10ull);
            if (!vertex.boundary && bucket >= 7) continue;
            const auto uv = normalizedParameter(parameterization.parameters[local]);
            const auto sampleWeight = vertex.boundary
                ? 100.0 * integrationWeight(vertex.boundaryLengthWeight, medianBoundaryWeight)
                : integrationWeight(vertex.areaWeight, medianAreaWeight);
            appendWeightedRow(
                rows,
                surfaceCoefficients(uv, patchKnots, variableOffset),
                vertex.point,
                sampleWeight);
            const auto tangentBucket = static_cast<int>(
                (vertexIndex * 11400714819323198485ull + patch * 14029467366897019727ull) % 4ull);
            if (!vertex.boundary && tangentBucket != 0) continue;
            double uu = 0.0;
            double uvMoment = 0.0;
            double vv = 0.0;
            gp_Vec uPosition;
            gp_Vec vPosition;
            for (const auto neighbor : neighbors[local]) {
                const auto neighborUv = normalizedParameter(parameterization.parameters[neighbor]);
                const auto du = neighborUv[0] - uv[0];
                const auto dv = neighborUv[1] - uv[1];
                const gp_Vec displacement(
                    vertex.point,
                    mesh.vertices[parameterization.vertexIndices[neighbor]].point);
                uu += du * du;
                uvMoment += du * dv;
                vv += dv * dv;
                uPosition += displacement * du;
                vPosition += displacement * dv;
            }
            const auto tangentDeterminant = uu * vv - uvMoment * uvMoment;
            if (tangentDeterminant > 1.0e-16) {
                const auto targetU = (uPosition * vv - vPosition * uvMoment) /
                    tangentDeterminant;
                const auto targetV = (vPosition * uu - uPosition * uvMoment) /
                    tangentDeterminant;
                constexpr double tangentWeight = 0.02;
                appendWeightedRow(
                    rows,
                    surfaceDerivativeCoefficients(uv, patchKnots, variableOffset, 0),
                    gp_Pnt(targetU.X(), targetU.Y(), targetU.Z()),
                    tangentWeight);
                appendWeightedRow(
                    rows,
                    surfaceDerivativeCoefficients(uv, patchKnots, variableOffset, 1),
                    gp_Pnt(targetV.X(), targetV.Y(), targetV.Z()),
                    tangentWeight);
            }
        }
    }
    std::vector<double> fittingAreaWeights;
    fittingAreaWeights.reserve(fittingReference.vertices.size());
    for (const auto& vertex : fittingReference.vertices) {
        if (!vertex.boundary) fittingAreaWeights.push_back(vertex.areaWeight);
    }
    const auto fittingMedianAreaWeight = positiveMedian(std::move(fittingAreaWeights));
    for (const auto& sample : mappedSamples) {
        const auto& vertex = fittingReference.vertices[sample.referenceVertex];
        if (vertex.boundary) continue;
        const auto bucket = static_cast<int>((sample.referenceVertex * 2654435761ull +
            static_cast<std::size_t>(sample.patch) * 40503ull) % 10ull);
        if (bucket >= 7) continue;
        appendWeightedRow(
            rows,
            surfaceCoefficients(
                sample.uv,
                knots[static_cast<std::size_t>(sample.patch)],
                variableOffsets[static_cast<std::size_t>(sample.patch)]),
            vertex.point,
            integrationWeight(vertex.areaWeight, fittingMedianAreaWeight));
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
        if (!firstDerivative.empty() && !secondDerivative.empty()) {
            firstDerivative.insert(
                firstDerivative.end(), secondDerivative.begin(), secondDerivative.end());
            appendWeightedRow(
                rows,
                std::move(firstDerivative),
                gp_Pnt(0.0, 0.0, 0.0),
                0.1);
            ++seamG1RowCount;
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
    rows.reserve(mesh.vertices.size() * 2);
    std::vector<double> areaWeights;
    areaWeights.reserve(mesh.vertices.size());
    for (const auto& vertex : mesh.vertices) if (!vertex.boundary) areaWeights.push_back(vertex.areaWeight);
    const auto medianAreaWeight = positiveMedian(std::move(areaWeights));
    for (std::size_t patch = 0; patch < layout.patches.size(); ++patch) {
        const auto& parameterization = layout.patches[patch];
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            const auto vertexIndex = parameterization.vertexIndices[local];
            const auto& vertex = mesh.vertices[vertexIndex];
            const auto bucket = static_cast<int>((vertexIndex * 2654435761ull + patch * 40503ull) % 10ull);
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
            const auto smoothBoundary = smoothConstraint != nullptr;
            if ((!smoothBoundary && vertex.boundary) || (!vertex.boundary && bucket >= 7)) continue;
            const auto uv = normalizedParameter(parameterization.parameters[local]);
            const auto weight = integrationWeight(vertex.areaWeight, medianAreaWeight);
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
                for (auto& coefficient : coefficients) coefficient.second *= weight;
                rows.push_back({
                    std::move(coefficients),
                    targetNormal});
                if (smoothBoundary) ++adjacentNormalRowCount;
            }
        }
    }
    for (const auto& sample : mappedSamples) {
        const auto& vertex = fittingReference.vertices[sample.referenceVertex];
        if (vertex.boundary) continue;
        const auto bucket = static_cast<int>((sample.referenceVertex * 2654435761ull +
            static_cast<std::size_t>(sample.patch) * 40503ull) % 10ull);
        if (bucket >= 7) continue;
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
                                               double& parameter) {
            const auto found = constraintByEdge.find(edgeId);
            if (found == constraintByEdge.end()) return false;
            double first = 0.0;
            double last = 0.0;
            const auto curve = BRep_Tool::Curve(found->second->originalEdge, first, last);
            if (curve.IsNull() || last <= first) return false;
            GeomAPI_ProjectPointOnCurve projection(reference, curve, first, last);
            if (projection.NbPoints() == 0) return false;
            parameter = projection.LowerDistanceParameter();
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
                if (!constrainedVertices.emplace(vertexIndex, edgeId).second ||
                    !targetOnOriginalEdge(edgeId, vertex.point, target, parameter)) {
                    continue;
                }
                rows.push_back({
                    surfaceCoefficients(
                        normalizedParameter(parameterization.parameters[local]),
                        knots[patch],
                        variableOffsets[patch]),
                    target});
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
                if (!targetOnOriginalEdge(
                        edgeId, vertex.point, firstTarget, firstParameter) ||
                    !targetOnOriginalEdge(
                        edgeId, nextVertex.point, secondTarget, secondParameter)) {
                    continue;
                }
                const auto found = constraintByEdge.find(edgeId);
                double first = 0.0;
                double last = 0.0;
                const auto curve = BRep_Tool::Curve(found->second->originalEdge, first, last);
                const auto target = curve->Value(
                    std::clamp((firstParameter + secondParameter) * 0.5, first, last));
                const auto firstUv = normalizedParameter(parameterization.parameters[local]);
                const auto secondUv = normalizedParameter(parameterization.parameters[nextLocal]);
                rows.push_back({
                    surfaceCoefficients(
                        {(firstUv[0] + secondUv[0]) * 0.5,
                         (firstUv[1] + secondUv[1]) * 0.5},
                        knots[patch],
                        variableOffsets[patch]),
                    target});
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
                gp_Pnt(0.0, 0.0, 0.0)});
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
            normalizedParameter(layout.patches[firstPatch].parameters[firstFound->second]),
            knots[firstPatch],
            variableOffsets[firstPatch]);
        const auto secondCoefficients = surfaceCoefficients(
            normalizedParameter(layout.patches[secondPatch].parameters[secondFound->second]),
            knots[secondPatch],
            variableOffsets[secondPatch]);
        for (const auto& [column, value] : secondCoefficients) {
            coefficients.emplace_back(column, -value);
        }
        rows.push_back({std::move(coefficients), gp_Pnt(0.0, 0.0, 0.0)});
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

bool localSupportDomainPassed(
    const Handle(Geom_BSplineSurface)& surface,
    const RegionReferenceMesh& carrier,
    const RegionPatchParameterization& parameterization,
    const RegionRefitOptions& options,
    std::string& reason) {
    std::array<double, 3> minimum {
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    std::array<double, 3> maximum {
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    gp_Vec referenceNormal;
    for (const auto vertexIndex : parameterization.vertexIndices) {
        const auto& vertex = carrier.vertices[vertexIndex];
        const std::array<double, 3> coordinate {vertex.point.X(), vertex.point.Y(), vertex.point.Z()};
        for (std::size_t axis = 0; axis < coordinate.size(); ++axis) {
            minimum[axis] = std::min(minimum[axis], coordinate[axis]);
            maximum[axis] = std::max(maximum[axis], coordinate[axis]);
        }
    }
    for (const auto triangleIndex : parameterization.triangleIndices) {
        const auto& triangle = carrier.triangles[triangleIndex];
        gp_Vec first(
            carrier.vertices[triangle.vertices[0]].point,
            carrier.vertices[triangle.vertices[1]].point);
        gp_Vec second(
            carrier.vertices[triangle.vertices[0]].point,
            carrier.vertices[triangle.vertices[2]].point);
        referenceNormal += first.Crossed(second);
    }
    const auto diagonal = std::sqrt(
        std::pow(maximum[0] - minimum[0], 2.0) +
        std::pow(maximum[1] - minimum[1], 2.0) +
        std::pow(maximum[2] - minimum[2], 2.0));
    const auto margin = std::max(options.tolerance3d * 5.0, diagonal * 0.1);
    for (int u = 1; u <= surface->NbUPoles(); ++u) {
        for (int v = 1; v <= surface->NbVPoles(); ++v) {
            const auto pole = surface->Pole(u, v);
            if (pole.X() < minimum[0] - margin || pole.X() > maximum[0] + margin ||
                pole.Y() < minimum[1] - margin || pole.Y() > maximum[1] + margin ||
                pole.Z() < minimum[2] - margin || pole.Z() > maximum[2] + margin) {
                reason = "A fitted control net expands beyond its local support-domain envelope.";
                return false;
            }
        }
    }
    if (referenceNormal.SquareMagnitude() <= 1.0e-24) {
        reason = "A local support domain has no stable reference normal.";
        return false;
    }
    referenceNormal.Normalize();
    auto orientation = 0.0;
    for (int uIndex = 0; uIndex <= 10; ++uIndex) {
        for (int vIndex = 0; vIndex <= 10; ++vIndex) {
            gp_Pnt point;
            gp_Vec derivativeU;
            gp_Vec derivativeV;
            surface->D1(
                static_cast<double>(uIndex) / 10.0,
                static_cast<double>(vIndex) / 10.0,
                point,
                derivativeU,
                derivativeV);
            const auto jacobian = derivativeU.Crossed(derivativeV);
            if (jacobian.SquareMagnitude() <= 1.0e-20) {
                reason = "A fitted local support domain has a collapsed Jacobian.";
                return false;
            }
            const auto signedJacobian = jacobian.Dot(referenceNormal);
            if (std::abs(signedJacobian) <= 1.0e-12) {
                reason = "A fitted local support domain has an unstable Jacobian orientation.";
                return false;
            }
            if (orientation == 0.0) orientation = signedJacobian > 0.0 ? 1.0 : -1.0;
            if (orientation * signedJacobian <= 0.0) {
                reason = "A fitted local support domain contains a Jacobian flip.";
                return false;
            }
        }
    }
    return true;
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
        for (std::size_t local = 0; local < parameterization.vertexIndices.size(); ++local) {
            const auto vertexIndex = parameterization.vertexIndices[local];
            const auto& vertex = layoutCarrier.vertices[vertexIndex];
            const auto uv = normalizedParameter(parameterization.parameters[local]);
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

double normalizedViolationScore(
    const ValidationMetrics& metrics,
    const RegionRefitOptions& options) {
    return std::max({
        metrics.holdoutMaximumDistance / std::max(options.tolerance3d, 1.0e-12),
        metrics.holdoutMaximumNormalDegrees / std::max(options.normalToleranceDegrees, 1.0e-12),
        metrics.boundaryMaximumDistance / std::max(options.curveOnSurfaceTolerance, 1.0e-12),
        metrics.seamMaximumDistance / std::max(options.curveOnSurfaceTolerance, 1.0e-12)});
}

std::shared_ptr<RegionFittedPatchNetwork> fittedNetwork(
    const RegionPatchLayout& layout,
    std::vector<Handle(Geom_BSplineSurface)> surfaces,
    const ValidationMetrics& metrics,
    const RegionRefitOptions& options,
    const bool gatePassed) {
    auto network = std::make_shared<RegionFittedPatchNetwork>();
    network->layout = layout;
    network->surfaces = std::move(surfaces);
    network->inspectionShape = inspectionShape(network->surfaces);
    network->holdoutMaximumDistance = metrics.holdoutMaximumDistance;
    network->holdoutMaximumNormalDegrees = metrics.holdoutMaximumNormalDegrees;
    network->boundaryMaximumDistance = metrics.boundaryMaximumDistance;
    network->seamMaximumDistance = metrics.seamMaximumDistance;
    network->normalizedViolationScore = normalizedViolationScore(metrics, options);
    network->fitGatePassed = gatePassed;
    return network;
}

JointBSplineNetworkFitResult JointBSplineNetworkFitter::fit(
    const RegionReferenceMesh& fittingReference,
    const RegionReferenceMesh& layoutCarrier,
    const RegionTopologyConstraints& constraints,
    const std::vector<RegionPatchLayout>& candidates,
    const RegionRefitOptions& options,
    const std::chrono::steady_clock::time_point deadline) const {
    JointBSplineNetworkFitResult result;
    double bestEffortScore = std::numeric_limits<double>::infinity();
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
        const auto& layout = candidates[candidateIndex];
        RegionFitAttemptReport attempt;
        attempt.patchCount = layout.patchCount;
        attempt.layoutHash = layout.hash;
        const auto lookups = buildLookups(layout);
        const auto mappedSamples = mapReferenceSamples(
            fittingReference, layoutCarrier, layout, lookups);
        if (mappedSamples.size() != fittingReference.vertices.size()) {
            attempt.stopReason = "Not every fitting-reference sample could be mapped to the layout carrier.";
            result.attempts.push_back(std::move(attempt));
            continue;
        }
        std::vector<KnotConfiguration> knots(static_cast<std::size_t>(layout.patchCount));
        double previousSelection = std::numeric_limits<double>::infinity();
        int lowImprovementRounds = 0;
        std::vector<Handle(Geom_BSplineSurface)> bestSurfaces;
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
            if (!attempt.iterations.empty()) {
                const auto forecastMilliseconds = static_cast<long long>(std::ceil(
                    std::max(1.0, attempt.iterations.back().wallMilliseconds) * 1.25));
                if (std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(forecastMilliseconds) >= attemptDeadline) {
                    iterationReport.stopReason =
                        "Single-fit deadline reserve is insufficient for another iteration.";
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason = iterationReport.stopReason;
                    break;
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
            std::size_t seamG1RowCount = 0;
            const auto rows = buildSystemRows(
                fittingReference,
                layoutCarrier,
                layout,
                knots,
                variableOffsets,
                lookups,
                seamConstraintSamples,
                mappedSamples,
                seamG1RowCount);
            std::size_t adjacentNormalRowCount = 0;
            const auto normalRows = buildNormalRows(
                fittingReference,
                layoutCarrier,
                constraints,
                layout,
                knots,
                variableOffsets,
                mappedSamples,
                adjacentNormalRowCount);
            iterationReport.adjacentNormalSoftConstraintCount = adjacentNormalRowCount;
            auto equalityRows = buildBoundaryEqualityRows(
                layoutCarrier,
                constraints,
                layout,
                knots,
                variableOffsets);
            const auto boundaryEqualityRowCount = equalityRows.size();
            auto seamEqualityRows = buildSeamEqualityRows(
                layout,
                knots,
                variableOffsets,
                lookups,
                seamConstraintSamples);
            iterationReport.boundaryHardConstraintCount = boundaryEqualityRowCount;
            iterationReport.seamG0ConstraintCount = seamEqualityRows.size();
            iterationReport.seamG1SoftConstraintCount = seamG1RowCount;
            if (boundaryEqualityRowCount > 0) {
                iterationReport.boundaryTargetMinimum = {
                    std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity()};
                iterationReport.boundaryTargetMaximum = {
                    -std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity()};
                for (std::size_t row = 0; row < boundaryEqualityRowCount; ++row) {
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
            auto solveEqualityRows = equalityRows;
            if (!equalityRows.empty()) {
                std::vector<Eigen::Triplet<double>> transposeTriplets;
                std::size_t equalityNonZeros = 0;
                for (const auto& row : equalityRows) equalityNonZeros += row.coefficients.size();
                transposeTriplets.reserve(equalityNonZeros);
                for (std::size_t row = 0; row < equalityRows.size(); ++row) {
                    for (const auto& [column, value] : equalityRows[row].coefficients) {
                        transposeTriplets.emplace_back(
                            column, static_cast<int>(row), value);
                    }
                }
                Eigen::SparseMatrix<double> equalityTranspose(
                    controlVariableCount, static_cast<int>(equalityRows.size()));
                equalityTranspose.setFromTriplets(
                    transposeTriplets.begin(), transposeTriplets.end());
                Eigen::SparseQR<
                    Eigen::SparseMatrix<double>,
                    Eigen::COLAMDOrdering<int>> rankRevealingQr;
                rankRevealingQr.setPivotThreshold(
                    std::sqrt(std::numeric_limits<double>::epsilon()));
                rankRevealingQr.compute(equalityTranspose);
                if (rankRevealingQr.info() == Eigen::Success &&
                    rankRevealingQr.rank() < static_cast<int>(equalityRows.size())) {
                    solveEqualityRows.clear();
                    solveEqualityRows.reserve(
                        static_cast<std::size_t>(rankRevealingQr.rank()));
                    const auto& permutation = rankRevealingQr.colsPermutation().indices();
                    for (int column = 0; column < rankRevealingQr.rank(); ++column) {
                        solveEqualityRows.push_back(
                            equalityRows[static_cast<std::size_t>(permutation[column])]);
                    }
                }
            }
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
            for (std::size_t rowIndex = 0;
                 rowIndex < boundaryEqualityRowCount;
                 ++rowIndex) {
                const auto& row = equalityRows[rowIndex];
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
            if (!equalityRows.empty()) {
                Eigen::SparseQR<
                    Eigen::SparseMatrix<double>,
                    Eigen::COLAMDOrdering<int>> initialSolver;
                initialSolver.compute(scaledMatrix);
                if (initialSolver.info() != Eigen::Success) {
                    iterationReport.stopReason =
                        "Initial local support fit factorization failed.";
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason = iterationReport.stopReason;
                    break;
                }
                const Eigen::VectorXd initialScaledSolution =
                    initialSolver.solve(targets);
                if (initialSolver.info() != Eigen::Success ||
                    !initialScaledSolution.allFinite()) {
                    iterationReport.stopReason = "Initial local support fit solve failed.";
                    attempt.iterations.push_back(iterationReport);
                    attempt.stopReason = iterationReport.stopReason;
                    break;
                }
                try {
                    Eigen::MatrixXd initialSolution(controlVariableCount, 3);
                    for (int variable = 0; variable < controlVariableCount; ++variable) {
                        for (int axis = 0; axis < 3; ++axis) {
                            const auto coupled = variable * 3 + axis;
                            initialSolution(variable, axis) =
                                initialScaledSolution[coupled] / columnScales[coupled];
                        }
                    }
                    std::vector<Handle(Geom_BSplineSurface)> initialSurfaces;
                    initialSurfaces.reserve(static_cast<std::size_t>(layout.patchCount));
                    for (int patch = 0; patch < layout.patchCount; ++patch) {
                        initialSurfaces.push_back(makeSurface(
                            knots[static_cast<std::size_t>(patch)],
                            initialSolution,
                            variableOffsets[static_cast<std::size_t>(patch)]));
                    }
                    const auto initialMetrics = validateNetwork(
                        fittingReference,
                        layoutCarrier,
                        layout,
                        lookups,
                        initialSurfaces,
                        options,
                        mappedSamples);
                    iterationReport.initialTrainingMaximumDistance =
                        initialMetrics.trainingMaximumDistance;
                    iterationReport.initialBoundaryMaximumDistance =
                        initialMetrics.boundaryMaximumDistance;
                } catch (const Standard_Failure&) {
                }

                auto diagonalScale = 0.0;
                for (int diagonal = 0; diagonal < coupledVariableCount; ++diagonal) {
                    diagonalScale += std::max(
                        0.0, normalMatrix.coeff(diagonal, diagonal) - 1.0e-8);
                }
                diagonalScale /= static_cast<double>(std::max(1, coupledVariableCount));
                iterationReport.constraintCorrectionAnchorScale =
                    options.constraintCorrectionAnchorRelativeWeight * diagonalScale;
                for (int diagonal = 0; diagonal < coupledVariableCount; ++diagonal) {
                    normalMatrix.coeffRef(diagonal, diagonal) +=
                        iterationReport.constraintCorrectionAnchorScale;
                    normalTargets[diagonal] +=
                        iterationReport.constraintCorrectionAnchorScale *
                        initialScaledSolution[diagonal];
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
                iterationReport.stopReason = "Single-fit deadline reached before sparse factorization.";
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
                    iterationReport.stopReason = "Constrained joint-fit factorization failed.";
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
                    iterationReport.stopReason = "Constrained joint-fit solve failed.";
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
            std::vector<Handle(Geom_BSplineSurface)> surfaces;
            surfaces.reserve(static_cast<std::size_t>(layout.patchCount));
            try {
                for (int patch = 0; patch < layout.patchCount; ++patch) {
                    surfaces.push_back(makeSurface(
                        knots[static_cast<std::size_t>(patch)],
                        solution,
                        variableOffsets[static_cast<std::size_t>(patch)]));
                }
                std::string supportFailure;
                for (std::size_t patch = 0; patch < surfaces.size(); ++patch) {
                    if (!localSupportDomainPassed(
                            surfaces[patch],
                            layoutCarrier,
                            layout.patches[patch],
                            options,
                            supportFailure)) {
                        iterationReport.stopReason = supportFailure;
                        attempt.iterations.push_back(iterationReport);
                        attempt.stopReason = supportFailure;
                        break;
                    }
                }
                if (!supportFailure.empty()) break;
                const auto metrics = validateNetwork(
                    fittingReference,
                    layoutCarrier,
                    layout,
                    lookups,
                    surfaces,
                    options,
                    mappedSamples);
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
                if (equalityRows.empty()) {
                    iterationReport.initialTrainingMaximumDistance = metrics.trainingMaximumDistance;
                    iterationReport.initialBoundaryMaximumDistance = metrics.boundaryMaximumDistance;
                }
                iterationReport.correctedTrainingMaximumDistance = metrics.trainingMaximumDistance;
                iterationReport.correctedBoundaryMaximumDistance = metrics.boundaryMaximumDistance;
                const auto accepted = metrics.holdoutMaximumDistance <= options.tolerance3d &&
                    metrics.holdoutMaximumNormalDegrees <= options.normalToleranceDegrees &&
                    metrics.boundaryMaximumDistance <= options.curveOnSurfaceTolerance &&
                    metrics.seamMaximumDistance <= options.curveOnSurfaceTolerance;
                if (bestSurfaces.empty() || normalizedViolationScore(metrics, options) <
                        normalizedViolationScore(bestMetrics, options)) {
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
                    result.network = fittedNetwork(layout, std::move(surfaces), metrics, options, true);
                    result.status = RegionRefitStatus::Success;
                    result.message = "The smallest fully fitted layout passed all fitting gates.";
                    return result;
                }
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
        if (!bestSurfaces.empty()) {
            auto candidate = fittedNetwork(layout, std::move(bestSurfaces), bestMetrics, options, false);
            if (candidate->normalizedViolationScore < bestEffortScore) {
                bestEffortScore = candidate->normalizedViolationScore;
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
    const RegionReferenceMesh& mesh,
    const RegionTopologyConstraints& constraints,
    const std::vector<RegionPatchLayout>& candidates,
    const RegionRefitOptions& options,
    const std::chrono::steady_clock::time_point deadline) const {
    return fit(mesh, mesh, constraints, candidates, options, deadline);
}

}
