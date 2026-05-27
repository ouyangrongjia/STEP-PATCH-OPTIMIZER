#include "merge/PlaneRegionMerger.h"

#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "validate/ShapeValidator.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <BRepTools_ReShape.hxx>
#include <Geom_Plane.hxx>
#include <Precision.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace spo {

namespace {

constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

RegionMergeResult baseResult(const ShapeDocument& document, const MergeCandidate& candidate) {
    RegionMergeResult result;
    result.candidate_id = candidate.candidate_id;
    result.candidate_type = candidate.candidate_type;
    result.document = document;
    const auto& stats = document.stats();
    result.face_count_before = stats.faces;
    result.face_count_after = stats.faces;
    result.edge_count_before = stats.edges;
    result.edge_count_after = stats.edges;
    return result;
}

bool containsEdge(const std::vector<EdgeId>& edges, EdgeId edge) {
    return std::find(edges.begin(), edges.end(), edge) != edges.end();
}

void fail(RegionMergeResult& result, RegionMergeFailureReason reason, std::string message) {
    result.success = false;
    result.failure_reason = reason;
    result.message = std::move(message);
}

bool validateCandidate(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const PlaneRegionMergeOptions& options,
    RegionMergeResult& result) {
    if (!document.hasShape()) {
        fail(result, RegionMergeFailureReason::NotSupported, "PlaneRegionMerger requires a loaded shape.");
        return false;
    }
    if (!candidate.valid) {
        fail(result, RegionMergeFailureReason::InvalidCandidate, "Plane candidate is invalid.");
        return false;
    }
    if (candidate.candidate_type != MergeCandidateType::PlaneLike) {
        fail(result, RegionMergeFailureReason::UnsupportedCandidateType, "PlaneRegionMerger only supports PlaneLike candidates.");
        return false;
    }
    if (candidate.status == MergeCandidateStatus::Rejected) {
        fail(result, RegionMergeFailureReason::RejectedCandidate, "Rejected plane candidate will not be merged.");
        return false;
    }
    if (candidate.status == MergeCandidateStatus::Hidden) {
        fail(result, RegionMergeFailureReason::HiddenCandidate, "Hidden plane candidate will not be merged.");
        return false;
    }
    if (candidate.status == MergeCandidateStatus::Pending &&
        options.require_accepted_candidate &&
        !options.allow_pending_candidate) {
        fail(result, RegionMergeFailureReason::NotSupported, "Pending plane candidate requires explicit experimental allowance.");
        return false;
    }
    if (candidate.face_count < options.min_region_faces ||
        static_cast<int>(candidate.faces.size()) < options.min_region_faces) {
        fail(result, RegionMergeFailureReason::InsufficientFaces, "Plane candidate does not contain enough faces.");
        return false;
    }
    if (candidate.boundary_edges.empty()) {
        fail(result, RegionMergeFailureReason::BoundaryLoopInvalid, "Plane candidate has no boundary edges.");
        return false;
    }

    const auto& topology = document.topology();
    for (const auto faceId : candidate.faces) {
        if (faceId >= topology.faceCount()) {
            fail(result, RegionMergeFailureReason::InvalidCandidate, "Plane candidate references a missing face.");
            return false;
        }
        BRepAdaptor_Surface surface(topology.face(faceId));
        if (surface.GetType() != GeomAbs_Plane && !options.allow_approximate_planar_surfaces) {
            fail(
                result,
                RegionMergeFailureReason::ApproximateSurfaceNotSupported,
                "B-spline backed planar-like candidate is preview-only and not supported by strict PlaneRegionMerge.");
            return false;
        }
    }
    for (const auto edgeId : candidate.boundary_edges) {
        if (edgeId >= topology.edgeCount()) {
            fail(result, RegionMergeFailureReason::BoundaryLoopInvalid, "Plane candidate references a missing boundary edge.");
            return false;
        }
    }
    for (const auto edgeId : candidate.internal_edges) {
        if (containsEdge(candidate.protected_edges, edgeId)) {
            fail(result, RegionMergeFailureReason::ProtectedEdgeConflict, "Plane candidate internal edge crosses protected edges.");
            return false;
        }
        if (edgeId >= topology.edgeCount()) {
            fail(result, RegionMergeFailureReason::InvalidCandidate, "Plane candidate references a missing internal edge.");
            return false;
        }
    }
    return true;
}

bool candidateUsesApproximatePlanarSurface(const ShapeDocument& document, const MergeCandidate& candidate) {
    const auto& topology = document.topology();
    return std::any_of(candidate.faces.begin(), candidate.faces.end(), [&topology](FaceId faceId) {
        return faceId < topology.faceCount() && BRepAdaptor_Surface(topology.face(faceId)).GetType() != GeomAbs_Plane;
    });
}

std::optional<gp_Pln> planeFromFace(const TopoDS_Face& face) {
    BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane) {
        return std::nullopt;
    }
    return surface.Plane();
}

struct FacePlaneSample {
    gp_Pnt point;
    gp_Dir normal;
};

std::optional<FacePlaneSample> sampleFacePlaneCenter(const TopoDS_Face& face) {
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

    FacePlaneSample sample {props.Value(), props.Normal()};
    if (face.Orientation() == TopAbs_REVERSED) {
        sample.normal.Reverse();
    }
    return sample;
}

std::optional<gp_Pln> estimatePlaneFromFace(const TopoDS_Face& face) {
    const auto analyticPlane = planeFromFace(face);
    if (analyticPlane.has_value()) {
        return analyticPlane;
    }

    const auto sample = sampleFacePlaneCenter(face);
    if (!sample.has_value()) {
        return std::nullopt;
    }
    return gp_Pln(sample->point, sample->normal);
}

bool normalCompatible(const gp_Dir& lhs, const gp_Dir& rhs, double toleranceDegrees) {
    const auto angle = lhs.Angle(rhs) * kRadiansToDegrees;
    const auto reversedAngle = lhs.Reversed().Angle(rhs) * kRadiansToDegrees;
    return std::min(angle, reversedAngle) <= toleranceDegrees;
}

std::optional<gp_Pln> estimatePlaneFromCandidate(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const PlaneRegionMergeOptions& options,
    RegionMergeResult& result) {
    const auto& topology = document.topology();
    const auto seedSample = sampleFacePlaneCenter(topology.face(candidate.faces.front()));
    if (!seedSample.has_value()) {
        fail(result, RegionMergeFailureReason::PrimitiveFitFailed, "Plane candidate seed face cannot provide a planar support.");
        return std::nullopt;
    }

    double px = 0.0;
    double py = 0.0;
    double pz = 0.0;
    double nx = 0.0;
    double ny = 0.0;
    double nz = 0.0;
    int sampleCount = 0;
    for (const auto faceId : candidate.faces) {
        const auto sample = sampleFacePlaneCenter(topology.face(faceId));
        if (!sample.has_value()) {
            fail(result, RegionMergeFailureReason::PrimitiveFitFailed, "PlaneRegionMerger cannot sample one candidate face.");
            return std::nullopt;
        }
        if (!normalCompatible(sample->normal, seedSample->normal, options.normal_angle_tolerance_degrees)) {
            fail(result, RegionMergeFailureReason::PrimitiveFitFailed, "Plane candidate contains center normals outside tolerance.");
            return std::nullopt;
        }

        const auto dot = sample->normal.Dot(seedSample->normal);
        const auto normalSign = dot < 0.0 ? -1.0 : 1.0;
        px += sample->point.X();
        py += sample->point.Y();
        pz += sample->point.Z();
        nx += sample->normal.X() * normalSign;
        ny += sample->normal.Y() * normalSign;
        nz += sample->normal.Z() * normalSign;
        ++sampleCount;
    }

    if (sampleCount == 0 || std::abs(nx) + std::abs(ny) + std::abs(nz) <= Precision::Confusion()) {
        fail(result, RegionMergeFailureReason::PrimitiveFitFailed, "Plane candidate cannot provide a stable average normal.");
        return std::nullopt;
    }

    const auto invCount = 1.0 / static_cast<double>(sampleCount);
    return gp_Pln(gp_Pnt(px * invCount, py * invCount, pz * invCount), gp_Dir(nx, ny, nz));
}

bool computeDeviation(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const gp_Pln& plane,
    const PlaneRegionMergeOptions& options,
    RegionMergeResult& result) {
    double maxDistance = 0.0;
    double sumDistance = 0.0;
    double sumSquaredDistance = 0.0;
    int sampleCount = 0;
    const auto& topology = document.topology();

    for (const auto faceId : candidate.faces) {
        const auto& face = topology.face(faceId);
        const auto sample = sampleFacePlaneCenter(face);
        if (!sample.has_value()) {
            fail(result, RegionMergeFailureReason::PrimitiveFitFailed, "PlaneRegionMerger cannot sample one candidate face.");
            return false;
        }
        if (!normalCompatible(sample->normal, plane.Axis().Direction(), options.normal_angle_tolerance_degrees)) {
            fail(result, RegionMergeFailureReason::PrimitiveFitFailed, "Plane candidate contains center normals outside tolerance.");
            return false;
        }

        const auto distance = std::abs(plane.Distance(sample->point));
        maxDistance = std::max(maxDistance, distance);
        sumDistance += distance;
        sumSquaredDistance += distance * distance;
        ++sampleCount;
    }

    if (sampleCount == 0) {
        fail(result, RegionMergeFailureReason::InvalidCandidate, "Plane candidate contains no faces.");
        return false;
    }

    result.max_deviation = maxDistance;
    result.mean_deviation = sumDistance / sampleCount;
    result.rms_deviation = std::sqrt(sumSquaredDistance / sampleCount);
    if (result.max_deviation > options.max_deviation ||
        result.max_deviation > options.plane_distance_tolerance) {
        fail(result, RegionMergeFailureReason::DeviationTooLarge, "Plane candidate deviation exceeds tolerance.");
        return false;
    }
    return true;
}

std::optional<TopoDS_Wire> makeBoundaryWire(const ShapeDocument& document, const std::vector<EdgeId>& orderedBoundaryEdges) {
    std::vector<TopoDS_Edge> remaining;
    remaining.reserve(orderedBoundaryEdges.size());
    for (const auto edgeId : orderedBoundaryEdges) {
        remaining.push_back(document.topology().edge(edgeId));
    }
    if (remaining.empty()) {
        return std::nullopt;
    }

    BRepBuilderAPI_MakeWire wireBuilder;
    TopoDS_Vertex startVertex;
    TopoDS_Vertex currentVertex;
    TopExp::Vertices(remaining.front(), startVertex, currentVertex);
    if (startVertex.IsNull() || currentVertex.IsNull()) {
        return std::nullopt;
    }
    wireBuilder.Add(remaining.front());
    remaining.erase(remaining.begin());

    while (!remaining.empty()) {
        auto next = remaining.end();
        TopoDS_Edge orientedEdge;
        TopoDS_Vertex nextEnd;
        for (auto it = remaining.begin(); it != remaining.end(); ++it) {
            TopoDS_Vertex v1;
            TopoDS_Vertex v2;
            TopExp::Vertices(*it, v1, v2);
            if (v1.IsSame(currentVertex)) {
                next = it;
                orientedEdge = *it;
                nextEnd = v2;
                break;
            }
            if (v2.IsSame(currentVertex)) {
                next = it;
                orientedEdge = TopoDS::Edge(it->Reversed());
                nextEnd = v1;
                break;
            }
        }

        if (next == remaining.end() || orientedEdge.IsNull() || nextEnd.IsNull()) {
            return std::nullopt;
        }
        wireBuilder.Add(orientedEdge);
        if (wireBuilder.Error() != BRepBuilderAPI_WireDone) {
            return std::nullopt;
        }
        currentVertex = nextEnd;
        remaining.erase(next);
    }

    if (!wireBuilder.IsDone()) {
        return std::nullopt;
    }
    if (!currentVertex.IsSame(startVertex)) {
        return std::nullopt;
    }

    auto wire = wireBuilder.Wire();
    if (wire.IsNull() || !wire.Closed()) {
        return std::nullopt;
    }
    return wire;
}

bool hasSingleOuterWire(const TopoDS_Face& face) {
    int wireCount = 0;
    for (TopExp_Explorer explorer(face, TopAbs_WIRE); explorer.More(); explorer.Next()) {
        ++wireCount;
    }
    return wireCount == 1;
}

struct PreparedPlaneMerge {
    int candidate_id = -1;
    TopoDS_Face merged_face;
    std::set<FaceId> region_faces;
    std::set<EdgeId> boundary_edges;
    double max_deviation = 0.0;
    double mean_deviation = 0.0;
    double rms_deviation = 0.0;
    gp_Dir plane_normal = gp_Dir(0.0, 0.0, 1.0);
};

std::optional<PreparedPlaneMerge> preparePlaneMerge(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const PlaneRegionMergeOptions& options,
    RegionMergeResult& result) {
    if (!validateCandidate(document, candidate, options, result)) {
        return std::nullopt;
    }

    const auto targetPlane = estimatePlaneFromCandidate(document, candidate, options, result);
    if (!targetPlane.has_value()) {
        return std::nullopt;
    }
    const auto normal = targetPlane->Axis().Direction();
    result.plane_normal_x = normal.X();
    result.plane_normal_y = normal.Y();
    result.plane_normal_z = normal.Z();
    result.primitive_axis_x = normal.X();
    result.primitive_axis_y = normal.Y();
    result.primitive_axis_z = normal.Z();
    if (!computeDeviation(document, candidate, *targetPlane, options, result)) {
        return std::nullopt;
    }

    result.primitive_fit_error = result.max_deviation;

    if (options.allow_approximate_planar_surfaces && candidateUsesApproximatePlanarSurface(document, candidate)) {
        fail(
            result,
            RegionMergeFailureReason::NotImplemented,
            "Approximate planar surface passed fitting checks, but B-spline planar rebuild is not implemented in A1.");
        return std::nullopt;
    }

    const RegionBoundaryAnalyzer boundaryAnalyzer;
    const auto boundaryAnalysis = boundaryAnalyzer.analyze(document, candidate);
    if (!boundaryAnalysis.valid) {
        fail(result, boundaryAnalysis.failure_reason, boundaryAnalysis.message);
        return std::nullopt;
    }

    const auto boundaryWire = makeBoundaryWire(document, boundaryAnalysis.ordered_boundary_edges);
    if (!boundaryWire.has_value()) {
        fail(result, RegionMergeFailureReason::BoundaryLoopInvalid, "Plane candidate boundary edges do not form one closed wire.");
        return std::nullopt;
    }

    Handle(Geom_Plane) planeSurface = new Geom_Plane(*targetPlane);
    BRepBuilderAPI_MakeFace faceBuilder(planeSurface, *boundaryWire, Standard_True);
    if (!faceBuilder.IsDone()) {
        fail(result, RegionMergeFailureReason::SurfaceConstructionFailed, "Failed to build planar trimmed face.");
        return std::nullopt;
    }

    const auto mergedFace = faceBuilder.Face();
    if (mergedFace.IsNull() || !hasSingleOuterWire(mergedFace)) {
        fail(result, RegionMergeFailureReason::BoundaryLoopInvalid, "Built planar face does not have one stable outer loop.");
        return std::nullopt;
    }

    PreparedPlaneMerge prepared;
    prepared.candidate_id = candidate.candidate_id;
    prepared.merged_face = mergedFace;
    prepared.region_faces.insert(candidate.faces.begin(), candidate.faces.end());
    prepared.boundary_edges.insert(candidate.boundary_edges.begin(), candidate.boundary_edges.end());
    prepared.max_deviation = result.max_deviation;
    prepared.mean_deviation = result.mean_deviation;
    prepared.rms_deviation = result.rms_deviation;
    prepared.plane_normal = normal;
    return prepared;
}

TopoDS_Shape applyPreparedMerges(
    const ShapeDocument& document,
    const std::vector<PreparedPlaneMerge>& preparedMerges) {
    BRepTools_ReShape reshaper;
    const auto& topology = document.topology();
    for (const auto& prepared : preparedMerges) {
        bool replacedFirstFace = false;
        for (const auto faceId : prepared.region_faces) {
            if (!replacedFirstFace) {
                reshaper.Replace(topology.face(faceId), prepared.merged_face);
                replacedFirstFace = true;
            } else {
                reshaper.Remove(topology.face(faceId));
            }
        }
    }
    return reshaper.Apply(document.shape());
}

TopoDS_Shape simplifyPreparedBoundaryEdges(
    const ShapeDocument& originalDocument,
    const TopoDS_Shape& mergedShape,
    const std::vector<PreparedPlaneMerge>& preparedMerges,
    const PlaneRegionMergeOptions& options) {
    std::set<EdgeId> simplifiableEdges;
    for (const auto& prepared : preparedMerges) {
        simplifiableEdges.insert(prepared.boundary_edges.begin(), prepared.boundary_edges.end());
    }

    ShapeUpgrade_UnifySameDomain unifier(mergedShape, Standard_True, Standard_False, Standard_False);
    unifier.SetSafeInputMode(true);
    unifier.SetLinearTolerance(options.plane_distance_tolerance);
    unifier.SetAngularTolerance(options.normal_angle_tolerance_degrees * kDegreesToRadians);

    const auto& topology = originalDocument.topology();
    for (EdgeId edgeId = 0; edgeId < topology.edgeCount(); ++edgeId) {
        if (!simplifiableEdges.contains(edgeId)) {
            unifier.KeepShape(topology.edge(edgeId));
        }
    }

    unifier.Build();
    const auto simplifiedShape = unifier.Shape();
    return simplifiedShape.IsNull() ? mergedShape : simplifiedShape;
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
    if (after.faces <= 0 || after.edges <= 0) {
        return false;
    }
    return true;
}

bool preservesSolidCount(const ShapeStats& before, const ShapeStats& after) {
    return before.solids == 0 || before.solids == after.solids;
}

std::filesystem::path makeRoundtripTempPath(const PlaneRegionMergeOptions& options) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path path = options.roundtrip_temp_directory.empty()
        ? std::filesystem::temp_directory_path()
        : options.roundtrip_temp_directory;
    path /= "step-patch-optimizer-plane-roundtrip-" + std::to_string(tick) + ".stp";
    return path;
}

bool validateMergedDocument(
    const ShapeDocument& originalDocument,
    const ShapeDocument& mergedDocument,
    const PlaneRegionMergeOptions& options,
    const char* operationName,
    RegionMergeResult& result) {
    ShapeValidator validator;
    const auto validation = validator.validate(mergedDocument);
    result.brep_check_valid = validation.brep_check_valid;

    fillReductionStats(result, mergedDocument.stats());
    if (!hasUsableTopology(mergedDocument.stats())) {
        fail(result, RegionMergeFailureReason::ValidationFailed, std::string(operationName) + " result lost usable topology.");
        return false;
    }
    if (!preservesSolidCount(originalDocument.stats(), mergedDocument.stats())) {
        fail(result, RegionMergeFailureReason::ValidationFailed, std::string(operationName) + " result changed solid count.");
        return false;
    }
    if (result.face_count_after >= result.face_count_before) {
        fail(result, RegionMergeFailureReason::TopologyReplacementFailed, std::string(operationName) + " did not reduce face count.");
        return false;
    }
    if (!validation.brep_check_valid) {
        fail(result, RegionMergeFailureReason::ValidationFailed, std::string(operationName) + " result failed BRepCheck.");
        return false;
    }

    const auto tempPath = makeRoundtripTempPath(options);
    bool removeTemp = false;
    const auto cleanup = [&]() {
        if (removeTemp) {
            std::error_code ignored;
            std::filesystem::remove(tempPath, ignored);
        }
    };

    StepWriter writer;
    const auto writeStatus = writer.write(mergedDocument, tempPath);
    removeTemp = std::filesystem::exists(tempPath);
    if (!writeStatus.success()) {
        fail(result, RegionMergeFailureReason::ExportRoundtripFailed, std::string(operationName) + " export roundtrip failed while writing STEP: " + writeStatus.message());
        cleanup();
        return false;
    }

    StepReader reader;
    const auto readResult = reader.read(tempPath);
    if (!readResult.status.success()) {
        fail(result, RegionMergeFailureReason::ExportRoundtripFailed, std::string(operationName) + " export roundtrip failed while reading STEP: " + readResult.status.message());
        cleanup();
        return false;
    }
    if (!readResult.document.hasShape()) {
        fail(result, RegionMergeFailureReason::ExportRoundtripFailed, std::string(operationName) + " export roundtrip produced an empty document.");
        cleanup();
        return false;
    }

    const auto roundtripValidation = validator.validate(readResult.document);
    if (!roundtripValidation.brep_check_valid) {
        fail(result, RegionMergeFailureReason::ExportRoundtripFailed, std::string(operationName) + " export roundtrip failed BRepCheck.");
        cleanup();
        return false;
    }
    if (!preservesSolidCount(originalDocument.stats(), roundtripValidation.stats)) {
        fail(result, RegionMergeFailureReason::ExportRoundtripFailed, std::string(operationName) + " export roundtrip changed solid count.");
        cleanup();
        return false;
    }
    if (!hasUsableTopology(roundtripValidation.stats)) {
        fail(result, RegionMergeFailureReason::ExportRoundtripFailed, std::string(operationName) + " export roundtrip lost usable topology.");
        cleanup();
        return false;
    }
    if (roundtripValidation.free_edges > validation.free_edges ||
        roundtripValidation.multiple_edges > validation.multiple_edges) {
        fail(result, RegionMergeFailureReason::ExportRoundtripFailed, std::string(operationName) + " export roundtrip increased free or multiple edge count.");
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

}

RegionMergeResult PlaneRegionMerger::merge(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const PlaneRegionMergeOptions& options) const {
    auto result = baseResult(document, candidate);
    const auto prepared = preparePlaneMerge(document, candidate, options, result);
    if (!prepared.has_value()) {
        return result;
    }

    const auto mergedShape = simplifyPreparedBoundaryEdges(document, applyPreparedMerges(document, {*prepared}), {*prepared}, options);
    if (mergedShape.IsNull()) {
        fail(result, RegionMergeFailureReason::TopologyReplacementFailed, "Plane region merge replacement produced an empty shape.");
        return result;
    }

    auto mergedDocument = ShapeDocument(mergedShape, document.sourcePath());
    if (!validateMergedDocument(document, mergedDocument, options, "Plane region merge", result)) {
        return result;
    }

    result.success = true;
    result.failure_reason = RegionMergeFailureReason::None;
    result.message = "Plane region merge completed with export roundtrip validation passed.";
    result.document = std::move(mergedDocument);
    return result;
}

RegionMergeResult PlaneRegionMerger::mergeBatch(
    const ShapeDocument& document,
    const std::vector<MergeCandidate>& candidates,
    const PlaneRegionMergeOptions& options) const {
    RegionMergeResult result;
    result.document = document;
    const auto& stats = document.stats();
    result.face_count_before = stats.faces;
    result.face_count_after = stats.faces;
    result.edge_count_before = stats.edges;
    result.edge_count_after = stats.edges;
    if (!document.hasShape()) {
        fail(result, RegionMergeFailureReason::NotSupported, "PlaneRegionMerger batch requires a loaded shape.");
        return result;
    }
    if (candidates.empty()) {
        fail(result, RegionMergeFailureReason::CandidateNotFound, "No plane candidates were provided for batch merge.");
        return result;
    }

    std::vector<PreparedPlaneMerge> preparedMerges;
    std::set<FaceId> usedFaces;
    int skipped = 0;
    for (const auto& candidate : candidates) {
        auto candidateResult = baseResult(document, candidate);
        const auto prepared = preparePlaneMerge(document, candidate, options, candidateResult);
        if (!prepared.has_value()) {
            ++skipped;
            continue;
        }

        bool overlaps = false;
        for (const auto faceId : prepared->region_faces) {
            if (usedFaces.contains(faceId)) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            ++skipped;
            continue;
        }

        usedFaces.insert(prepared->region_faces.begin(), prepared->region_faces.end());
        result.max_deviation = std::max(result.max_deviation, prepared->max_deviation);
        result.primitive_fit_error = std::max(result.primitive_fit_error, prepared->max_deviation);
        result.mean_deviation += prepared->mean_deviation;
        result.rms_deviation += prepared->rms_deviation;
        if (preparedMerges.empty()) {
            result.plane_normal_x = prepared->plane_normal.X();
            result.plane_normal_y = prepared->plane_normal.Y();
            result.plane_normal_z = prepared->plane_normal.Z();
            result.primitive_axis_x = prepared->plane_normal.X();
            result.primitive_axis_y = prepared->plane_normal.Y();
            result.primitive_axis_z = prepared->plane_normal.Z();
        }
        preparedMerges.push_back(*prepared);
    }

    if (preparedMerges.empty()) {
        fail(result, RegionMergeFailureReason::CandidateNotFound, "No mergeable plane candidates were found.");
        return result;
    }
    result.mean_deviation /= static_cast<double>(preparedMerges.size());
    result.rms_deviation /= static_cast<double>(preparedMerges.size());

    const auto mergedShape = simplifyPreparedBoundaryEdges(document, applyPreparedMerges(document, preparedMerges), preparedMerges, options);
    if (mergedShape.IsNull()) {
        fail(result, RegionMergeFailureReason::TopologyReplacementFailed, "Plane region batch merge replacement produced an empty shape.");
        return result;
    }

    auto mergedDocument = ShapeDocument(mergedShape, document.sourcePath());
    if (!validateMergedDocument(document, mergedDocument, options, "Plane region batch merge", result)) {
        return result;
    }

    std::ostringstream message;
    message << "Plane region batch merge completed: merged " << preparedMerges.size()
            << " candidates, skipped " << skipped;
    message << ", export roundtrip validation passed.";

    result.success = true;
    result.failure_reason = RegionMergeFailureReason::None;
    result.message = message.str();
    result.document = std::move(mergedDocument);
    return result;
}

}
