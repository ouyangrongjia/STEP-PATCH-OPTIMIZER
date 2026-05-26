#include "merge/SphereRegionMerger.h"

#include "validate/ShapeValidator.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <Precision.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace spo {

namespace {

constexpr double kPi = 3.14159265358979323846;

double degreesToRadians(double degrees) {
    return degrees * kPi / 180.0;
}

void fail(RegionMergeResult& result, RegionMergeFailureReason reason, std::string message) {
    result.success = false;
    result.failure_reason = reason;
    result.message = std::move(message);
}

RegionMergeResult baseResult(const ShapeDocument& document, const MergeCandidate& candidate) {
    RegionMergeResult result;
    result.candidate_id = candidate.candidate_id;
    result.candidate_type = candidate.candidate_type;
    result.document = document;
    result.face_count_before = document.stats().faces;
    result.face_count_after = document.stats().faces;
    result.edge_count_before = document.stats().edges;
    result.edge_count_after = document.stats().edges;
    return result;
}

bool containsEdge(const std::vector<EdgeId>& edges, EdgeId edge) {
    return std::find(edges.begin(), edges.end(), edge) != edges.end();
}

bool solveLinear4x4(
    std::array<std::array<double, 4>, 4> matrix,
    std::array<double, 4> rhs,
    std::array<double, 4>& solution) {
    for (int pivot = 0; pivot < 4; ++pivot) {
        int pivotRow = pivot;
        for (int row = pivot + 1; row < 4; ++row) {
            if (std::abs(matrix[row][pivot]) > std::abs(matrix[pivotRow][pivot])) {
                pivotRow = row;
            }
        }
        if (std::abs(matrix[pivotRow][pivot]) < 1.0e-15) {
            return false;
        }
        if (pivotRow != pivot) {
            std::swap(matrix[pivot], matrix[pivotRow]);
            std::swap(rhs[pivot], rhs[pivotRow]);
        }

        const auto pivotValue = matrix[pivot][pivot];
        for (int col = pivot; col < 4; ++col) {
            matrix[pivot][col] /= pivotValue;
        }
        rhs[pivot] /= pivotValue;

        for (int row = 0; row < 4; ++row) {
            if (row == pivot) {
                continue;
            }
            const auto factor = matrix[row][pivot];
            for (int col = pivot; col < 4; ++col) {
                matrix[row][col] -= factor * matrix[pivot][col];
            }
            rhs[row] -= factor * rhs[pivot];
        }
    }

    for (int i = 0; i < 4; ++i) {
        solution[i] = rhs[i];
    }
    return true;
}

std::vector<gp_Pnt> sampleFacePoints(const TopoDS_Face& face, int resolution = 5) {
    std::vector<gp_Pnt> points;
    double uMin = 0.0;
    double uMax = 0.0;
    double vMin = 0.0;
    double vMax = 0.0;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    if (!std::isfinite(uMin) || !std::isfinite(uMax) || !std::isfinite(vMin) || !std::isfinite(vMax)) {
        return points;
    }

    BRepAdaptor_Surface surface(face);
    points.reserve(resolution * resolution);
    for (int i = 0; i < resolution; ++i) {
        for (int j = 0; j < resolution; ++j) {
            const auto u = uMin + (uMax - uMin) * (i + 0.5) / resolution;
            const auto v = vMin + (vMax - vMin) * (j + 0.5) / resolution;
            BRepLProp_SLProps props(surface, u, v, 0, Precision::Confusion());
            points.push_back(props.Value());
        }
    }
    return points;
}

bool fillSphereFitStats(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const SphereRegionMergeOptions& options,
    RegionMergeResult& result) {
    std::vector<gp_Pnt> points;
    const auto& topology = document.topology();
    for (const auto faceId : candidate.faces) {
        const auto facePoints = sampleFacePoints(topology.face(faceId), 5);
        points.insert(points.end(), facePoints.begin(), facePoints.end());
    }
    if (points.size() < 4) {
        fail(result, RegionMergeFailureReason::PrimitiveFitFailed, "Sphere candidate does not have enough sample points.");
        return false;
    }

    std::array<std::array<double, 4>, 4> normalMatrix {};
    std::array<double, 4> normalRhs {};
    for (const auto& point : points) {
        const std::array<double, 4> row {point.X(), point.Y(), point.Z(), 1.0};
        const auto rhs = -(point.X() * point.X() + point.Y() * point.Y() + point.Z() * point.Z());
        for (int i = 0; i < 4; ++i) {
            normalRhs[i] += row[i] * rhs;
            for (int j = 0; j < 4; ++j) {
                normalMatrix[i][j] += row[i] * row[j];
            }
        }
    }

    std::array<double, 4> coefficients {};
    if (!solveLinear4x4(normalMatrix, normalRhs, coefficients)) {
        fail(result, RegionMergeFailureReason::PrimitiveFitFailed, "Sphere least-squares fitting failed.");
        return false;
    }

    const gp_Pnt center(-0.5 * coefficients[0], -0.5 * coefficients[1], -0.5 * coefficients[2]);
    const auto radiusSquared = center.X() * center.X() + center.Y() * center.Y() + center.Z() * center.Z() - coefficients[3];
    if (radiusSquared <= Precision::Confusion()) {
        fail(result, RegionMergeFailureReason::PrimitiveFitFailed, "Sphere fitting produced invalid radius.");
        return false;
    }
    const auto radius = std::sqrt(radiusSquared);

    double maxResidual = 0.0;
    double sumResidual = 0.0;
    double sumSquaredResidual = 0.0;
    for (const auto& point : points) {
        const auto residual = std::abs(center.Distance(point) - radius);
        maxResidual = std::max(maxResidual, residual);
        sumResidual += residual;
        sumSquaredResidual += residual * residual;
    }

    const auto pointCount = static_cast<double>(points.size());
    result.max_deviation = maxResidual;
    result.mean_deviation = sumResidual / pointCount;
    result.rms_deviation = std::sqrt(sumSquaredResidual / pointCount);
    result.primitive_center_x = center.X();
    result.primitive_center_y = center.Y();
    result.primitive_center_z = center.Z();
    result.primitive_radius = radius;
    result.primitive_fit_error = maxResidual;

    if (maxResidual > options.max_deviation || maxResidual > options.sphere_fit_tolerance) {
        fail(result, RegionMergeFailureReason::DeviationTooLarge, "Sphere candidate deviation exceeds tolerance.");
        return false;
    }
    return true;
}

bool validateCandidate(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const SphereRegionMergeOptions& options,
    RegionMergeResult& result) {
    if (!document.hasShape()) {
        fail(result, RegionMergeFailureReason::NotSupported, "SphereRegionMerger requires a loaded shape.");
        return false;
    }
    if (!candidate.valid) {
        fail(result, RegionMergeFailureReason::InvalidCandidate, "Sphere candidate is invalid.");
        return false;
    }
    if (candidate.candidate_type != MergeCandidateType::SphereLike) {
        fail(result, RegionMergeFailureReason::UnsupportedCandidateType, "SphereRegionMerger only supports SphereLike candidates.");
        return false;
    }
    if (candidate.status == MergeCandidateStatus::Rejected) {
        fail(result, RegionMergeFailureReason::RejectedCandidate, "Rejected sphere candidate will not be merged.");
        return false;
    }
    if (candidate.status == MergeCandidateStatus::Hidden) {
        fail(result, RegionMergeFailureReason::HiddenCandidate, "Hidden sphere candidate will not be merged.");
        return false;
    }
    if (candidate.status == MergeCandidateStatus::Pending &&
        options.require_accepted_candidate &&
        !options.allow_pending_candidate) {
        fail(result, RegionMergeFailureReason::NotSupported, "Pending sphere candidate requires explicit experimental allowance.");
        return false;
    }
    if (candidate.face_count < options.min_region_faces ||
        static_cast<int>(candidate.faces.size()) < options.min_region_faces) {
        fail(result, RegionMergeFailureReason::InsufficientFaces, "Sphere candidate does not contain enough faces.");
        return false;
    }
    if (candidate.internal_edges.empty()) {
        fail(result, RegionMergeFailureReason::CandidateNotFound, "Sphere candidate has no internal edges to merge.");
        return false;
    }

    const auto& topology = document.topology();
    for (const auto faceId : candidate.faces) {
        if (faceId >= topology.faceCount()) {
            fail(result, RegionMergeFailureReason::InvalidCandidate, "Sphere candidate references a missing face.");
            return false;
        }
    }
    for (const auto edgeId : candidate.internal_edges) {
        if (edgeId >= topology.edgeCount()) {
            fail(result, RegionMergeFailureReason::InvalidCandidate, "Sphere candidate references a missing internal edge.");
            return false;
        }
        if (containsEdge(candidate.protected_edges, edgeId)) {
            fail(result, RegionMergeFailureReason::ProtectedEdgeConflict, "Sphere candidate internal edge crosses protected edges.");
            return false;
        }
    }

    return fillSphereFitStats(document, candidate, options, result);
}

ShapeDocument unifyInternalEdges(
    const ShapeDocument& document,
    const std::set<EdgeId>& internalEdges,
    const SphereRegionMergeOptions& options) {
    ShapeUpgrade_UnifySameDomain unifier(document.shape(), true, true, false);
    unifier.SetSafeInputMode(true);
    unifier.SetLinearTolerance(std::max(Precision::Confusion(), options.max_deviation));
    unifier.SetAngularTolerance(degreesToRadians(options.max_normal_angle_degrees));

    const auto& topology = document.topology();
    for (EdgeId edgeId = 0; edgeId < topology.edgeCount(); ++edgeId) {
        if (!internalEdges.contains(edgeId)) {
            unifier.KeepShape(topology.edge(edgeId));
        }
    }

    unifier.Build();
    return ShapeDocument(unifier.Shape(), document.sourcePath());
}

void fillReductionStats(RegionMergeResult& result, const ShapeStats& after) {
    result.face_count_after = after.faces;
    result.edge_count_after = after.edges;
    result.face_reduction_ratio = result.face_count_before > 0
        ? static_cast<double>(result.face_count_before - result.face_count_after) / result.face_count_before
        : 0.0;
    result.edge_reduction_ratio = result.edge_count_before > 0
        ? static_cast<double>(result.edge_count_before - result.edge_count_after) / result.edge_count_before
        : 0.0;
}

bool hasUsableTopology(const ShapeStats& after) {
    return after.faces > 0 && after.edges > 0;
}

bool preservesSolidCount(const ShapeStats& before, const ShapeStats& after) {
    return before.solids == 0 || before.solids == after.solids;
}

bool validateMergedDocument(
    const ShapeDocument& before,
    ShapeDocument&& after,
    RegionMergeResult& result,
    const char* noReductionMessage) {
    ShapeValidator validator;
    const auto validation = validator.validate(after);
    result.brep_check_valid = validation.brep_check_valid;

    fillReductionStats(result, after.stats());
    if (!hasUsableTopology(after.stats())) {
        fail(result, RegionMergeFailureReason::ValidationFailed, "Sphere region merge result lost usable topology.");
        return false;
    }
    if (!preservesSolidCount(before.stats(), after.stats())) {
        fail(result, RegionMergeFailureReason::ValidationFailed, "Sphere region merge result changed solid count.");
        return false;
    }
    if (result.face_count_after >= result.face_count_before) {
        fail(result, RegionMergeFailureReason::TopologyReplacementFailed, noReductionMessage);
        return false;
    }

    result.success = true;
    result.failure_reason = RegionMergeFailureReason::None;
    result.message = validation.brep_check_valid
        ? "Sphere region merge completed with same-domain unifier."
        : "Sphere region merge completed with same-domain unifier and BRepCheck warning.";
    result.document = std::move(after);
    return true;
}

}

RegionMergeResult SphereRegionMerger::merge(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const SphereRegionMergeOptions& options) const {
    auto result = baseResult(document, candidate);
    if (!validateCandidate(document, candidate, options, result)) {
        return result;
    }

    const std::set<EdgeId> internalEdges(candidate.internal_edges.begin(), candidate.internal_edges.end());
    auto mergedDocument = unifyInternalEdges(document, internalEdges, options);
    validateMergedDocument(document, std::move(mergedDocument), result, "Sphere region merge did not reduce face count.");
    return result;
}

RegionMergeResult SphereRegionMerger::merge(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const RegionMergeOptions& options) const {
    SphereRegionMergeOptions sphereOptions;
    sphereOptions.max_deviation = options.max_deviation;
    sphereOptions.min_region_faces = options.min_region_faces;
    sphereOptions.allow_pending_candidate = options.allow_pending_candidate;
    sphereOptions.require_accepted_candidate = options.require_accepted_candidate;
    return merge(document, candidate, sphereOptions);
}

RegionMergeResult SphereRegionMerger::mergeBatch(
    const ShapeDocument& document,
    const std::vector<MergeCandidate>& candidates,
    const SphereRegionMergeOptions& options) const {
    RegionMergeResult result;
    result.document = document;
    result.face_count_before = document.stats().faces;
    result.face_count_after = document.stats().faces;
    result.edge_count_before = document.stats().edges;
    result.edge_count_after = document.stats().edges;
    result.candidate_type = MergeCandidateType::SphereLike;

    if (!document.hasShape()) {
        fail(result, RegionMergeFailureReason::NotSupported, "SphereRegionMerger batch requires a loaded shape.");
        return result;
    }
    if (candidates.empty()) {
        fail(result, RegionMergeFailureReason::CandidateNotFound, "No sphere candidates were provided for batch merge.");
        return result;
    }

    std::set<EdgeId> internalEdges;
    std::set<FaceId> usedFaces;
    int accepted = 0;
    int skipped = 0;
    for (const auto& candidate : candidates) {
        auto candidateResult = baseResult(document, candidate);
        if (!validateCandidate(document, candidate, options, candidateResult)) {
            ++skipped;
            continue;
        }

        bool overlaps = false;
        for (const auto faceId : candidate.faces) {
            if (usedFaces.contains(faceId)) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            ++skipped;
            continue;
        }

        usedFaces.insert(candidate.faces.begin(), candidate.faces.end());
        internalEdges.insert(candidate.internal_edges.begin(), candidate.internal_edges.end());
        result.max_deviation = std::max(result.max_deviation, candidateResult.max_deviation);
        result.primitive_fit_error = std::max(result.primitive_fit_error, candidateResult.primitive_fit_error);
        result.mean_deviation += candidateResult.mean_deviation;
        result.rms_deviation += candidateResult.rms_deviation;
        if (accepted == 0) {
            result.candidate_id = candidate.candidate_id;
            result.primitive_center_x = candidateResult.primitive_center_x;
            result.primitive_center_y = candidateResult.primitive_center_y;
            result.primitive_center_z = candidateResult.primitive_center_z;
            result.primitive_radius = candidateResult.primitive_radius;
        }
        ++accepted;
    }

    if (accepted == 0 || internalEdges.empty()) {
        fail(result, RegionMergeFailureReason::CandidateNotFound, "No sphere candidates passed batch merge validation.");
        return result;
    }
    result.mean_deviation /= static_cast<double>(accepted);
    result.rms_deviation /= static_cast<double>(accepted);

    auto mergedDocument = unifyInternalEdges(document, internalEdges, options);
    if (!validateMergedDocument(document, std::move(mergedDocument), result, "Sphere region batch merge did not reduce face count.")) {
        return result;
    }

    std::ostringstream message;
    message << "Sphere region batch merge completed with same-domain unifier: merged "
            << accepted << " candidates, skipped " << skipped << ".";
    result.message = message.str();
    return result;
}

}
