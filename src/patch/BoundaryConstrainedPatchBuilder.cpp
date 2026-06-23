#include "patch/BoundaryConstrainedPatchBuilder.h"

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"
#include "merge/RegionBoundaryAnalyzer.h"
#include "patch/BoundaryConstrainedMultiSurfaceShellBuilder.h"
#include "patch/ImportedPatchInfo.h"
#include "patch/PatchReplacementReport.h"

#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Compound.hxx>

#include <cmath>
#include <string>

namespace spo {

namespace {

struct BBoxInfo {
    bool valid = false;
    double minX = 0.0;
    double minY = 0.0;
    double minZ = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    double maxZ = 0.0;
};

void append_warning(BoundaryConstrainedPatchBuildResult& result, const std::string& warning) {
    if (warning.empty()) {
        return;
    }
    if (!result.warningMessage.empty()) {
        result.warningMessage += " ";
    }
    result.warningMessage += warning;
}

BoundaryConstrainedPatchBuildResult fail(
    BoundaryConstrainedPatchBuildResult result,
    BoundaryConstrainedBuildFailureReason reason,
    std::string message) {
    result.failureReason = reason;
    result.message = std::move(message);
    return result;
}

bool boundary_supported(const RegionBoundaryAnalysis& boundary) {
    return boundary.valid &&
        boundary.connected_component_count == 1 &&
        boundary.outer_wire_count == 1 &&
        boundary.inner_wire_count == 0 &&
        boundary.boundary_closed &&
        !boundary.ordered_boundary_edges.empty() &&
        !boundary.has_holes &&
        !boundary.has_non_manifold_edges &&
        !boundary.has_branching_boundary;
}

BBoxInfo bbox_from_box(const Bnd_Box& box) {
    BBoxInfo info;
    if (box.IsVoid()) {
        return info;
    }
    box.Get(info.minX, info.minY, info.minZ, info.maxX, info.maxY, info.maxZ);
    info.valid = true;
    return info;
}

BBoxInfo candidate_bbox(const ShapeDocument& document, const MergeCandidate& candidate) {
    Bnd_Box box;
    bool added = false;
    for (const auto faceId : candidate.faces) {
        if (faceId < 0 || static_cast<std::size_t>(faceId) >= document.topology().faceCount()) {
            continue;
        }
        BRepBndLib::Add(document.topology().face(faceId), box);
        added = true;
    }
    return added ? bbox_from_box(box) : BBoxInfo {};
}

BBoxInfo shape_bbox(const TopoDS_Shape& shape) {
    if (shape.IsNull()) {
        return {};
    }
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    return bbox_from_box(box);
}

double diagonal(const BBoxInfo& box) {
    const auto dx = box.maxX - box.minX;
    const auto dy = box.maxY - box.minY;
    const auto dz = box.maxZ - box.minZ;
    if (dx < 0.0 || dy < 0.0 || dz < 0.0) {
        return 0.0;
    }
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double center_distance(const BBoxInfo& lhs, const BBoxInfo& rhs) {
    const auto lhsX = (lhs.minX + lhs.maxX) * 0.5;
    const auto lhsY = (lhs.minY + lhs.maxY) * 0.5;
    const auto lhsZ = (lhs.minZ + lhs.maxZ) * 0.5;
    const auto rhsX = (rhs.minX + rhs.maxX) * 0.5;
    const auto rhsY = (rhs.minY + rhs.maxY) * 0.5;
    const auto rhsZ = (rhs.minZ + rhs.maxZ) * 0.5;
    const auto dx = rhsX - lhsX;
    const auto dy = rhsY - lhsY;
    const auto dz = rhsZ - lhsZ;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool bbox_mismatch(const BBoxInfo& candidate, const BBoxInfo& patch, double toleranceRatio) {
    if (!candidate.valid || !patch.valid || toleranceRatio <= 0.0) {
        return false;
    }

    const auto candidateDiagonal = diagonal(candidate);
    const auto patchDiagonal = diagonal(patch);
    if (candidateDiagonal <= 0.0 || patchDiagonal <= 0.0) {
        return true;
    }

    const auto ratio = patchDiagonal / candidateDiagonal;
    return ratio > toleranceRatio ||
        ratio < (1.0 / toleranceRatio) ||
        center_distance(candidate, patch) / candidateDiagonal > toleranceRatio;
}

std::vector<TopoDS_Face> valid_faces(const MultiFacePatchAnalysis& analysis) {
    std::vector<TopoDS_Face> faces;
    for (const auto& face : analysis.faces) {
        if (!face.IsNull()) {
            faces.push_back(face);
        }
    }
    return faces;
}

void copy_retrim_report(
    BoundaryConstrainedPatchBuildResult& result,
    const BoundaryConstrainedSurfaceRetrimResult& retrim) {
    result.retrimSelectedPatchFaceIndex = retrim.selectedPatchFaceIndex;
    result.retrimBoundarySampleCount = retrim.boundarySampleCount;
    result.retrimProjectedSampleCount = retrim.projectedSampleCount;
    result.retrimFailedProjectionCount = retrim.failedProjectionCount;
    result.retrimMaxProjectionDistance = retrim.maxProjectionDistance;
    result.retrimAverageProjectionDistance = retrim.averageProjectionDistance;
    result.retrimSurfaceCoverageProjectedSampleCount = retrim.surfaceCoverageProjectedSampleCount;
    result.retrimSurfaceCoverageFailedProjectionCount = retrim.surfaceCoverageFailedProjectionCount;
    result.retrimSurfaceCoverageMaxProjectionDistance = retrim.surfaceCoverageMaxProjectionDistance;
    result.retrimSurfaceCoverageAverageProjectionDistance = retrim.surfaceCoverageAverageProjectionDistance;
    result.retrimSurfaceCoverageUncoveredEdgeIds = retrim.surfaceCoverageUncoveredEdgeIds;
    result.retrimBoundaryEdgePcurveRebuildAttemptCount = retrim.boundaryEdgePcurveRebuildAttemptCount;
    result.retrimBoundaryEdgePcurveRebuildSuccessCount = retrim.boundaryEdgePcurveRebuildSuccessCount;
    result.retrimBoundaryEdgePcurveRebuildFailureCount = retrim.boundaryEdgePcurveRebuildFailureCount;
    result.retrimBoundaryEdgeSameParameterCheckCount = retrim.boundaryEdgeSameParameterCheckCount;
    result.retrimBoundaryEdgeSameParameterFailureCount = retrim.boundaryEdgeSameParameterFailureCount;
    result.retrimBoundaryEdgeMaxSameParameterDeviation = retrim.boundaryEdgeMaxSameParameterDeviation;
    result.retrimBoundaryEdgePcurveRebuildFailedEdgeIds = retrim.boundaryEdgePcurveRebuildFailedEdgeIds;
    result.retrimBoundaryEdgeSameParameterFailedEdgeIds = retrim.boundaryEdgeSameParameterFailedEdgeIds;
}

void copy_multi_surface_report(
    BoundaryConstrainedPatchBuildResult& result,
    const BoundaryConstrainedMultiSurfaceShellResult& shell) {
    result.attemptedMultiSurfaceBoundaryShell = shell.attempted;
    result.multiSurfaceBoundarySampleCount = shell.boundarySampleCount;
    result.multiSurfaceProjectedSampleCount = shell.projectedSampleCount;
    result.multiSurfaceFailedProjectionCount = shell.failedProjectionCount;
    result.multiSurfaceMaxProjectionDistance = shell.maxProjectionDistance;
    result.multiSurfaceAverageProjectionDistance = shell.averageProjectionDistance;
    result.multiSurfaceAssignedBoundarySegmentCount = shell.assignedBoundarySegmentCount;
    result.multiSurfaceSplitBoundaryEdgeCount = shell.splitBoundaryEdgeCount;
    result.multiSurfaceBuiltFaceCount = shell.builtFaceCount;
    result.multiSurfaceClosedWireCount = shell.closedWireCount;
    result.multiSurfaceOpenWireCount = shell.openWireCount;
    result.multiSurfaceMultipleClosedWireFaceCount = shell.multipleClosedWireFaceCount;
    result.multiSurfaceSkippedUnownedOpenWireFaceCount =
        shell.skippedUnownedOpenWireFaceCount;
    result.multiSurfaceFailedPatchFaceIndex = shell.failedPatchFaceIndex;
    result.multiSurfaceFailedFaceEdgeCount = shell.failedFaceEdgeCount;
    result.multiSurfaceFailedFaceOriginalBoundarySegmentCount =
        shell.failedFaceOriginalBoundarySegmentCount;
    result.multiSurfaceFailedFaceInternalEdgeCount = shell.failedFaceInternalEdgeCount;
    result.multiSurfaceFailedOpenWireEdgeCount = shell.failedOpenWireEdgeCount;
    result.multiSurfaceFailedOpenWireLength = shell.failedOpenWireLength;
    result.multiSurfaceFailedOpenWireEndpointGap = shell.failedOpenWireEndpointGap;
    result.multiSurfaceFailedOpenWireStartPointValid = shell.failedOpenWireStartPointValid;
    result.multiSurfaceFailedOpenWireStartX = shell.failedOpenWireStartX;
    result.multiSurfaceFailedOpenWireStartY = shell.failedOpenWireStartY;
    result.multiSurfaceFailedOpenWireStartZ = shell.failedOpenWireStartZ;
    result.multiSurfaceFailedOpenWireEndPointValid = shell.failedOpenWireEndPointValid;
    result.multiSurfaceFailedOpenWireEndX = shell.failedOpenWireEndX;
    result.multiSurfaceFailedOpenWireEndY = shell.failedOpenWireEndY;
    result.multiSurfaceFailedOpenWireEndZ = shell.failedOpenWireEndZ;
    result.multiSurfaceSelectedWireConnectTolerance = shell.selectedWireConnectTolerance;
    result.multiSurfaceFallbackWireConnectAttempted = shell.fallbackWireConnectAttempted;
    result.multiSurfaceFallbackWireConnectSucceeded = shell.fallbackWireConnectSucceeded;
    result.multiSurfaceBoundaryEdgePcurveRebuildAttemptCount = shell.boundaryEdgePcurveRebuildAttemptCount;
    result.multiSurfaceBoundaryEdgePcurveRebuildSuccessCount = shell.boundaryEdgePcurveRebuildSuccessCount;
    result.multiSurfaceBoundaryEdgePcurveRebuildFailureCount = shell.boundaryEdgePcurveRebuildFailureCount;
    result.multiSurfaceBoundaryEdgeSameParameterCheckCount = shell.boundaryEdgeSameParameterCheckCount;
    result.multiSurfaceBoundaryEdgeSameParameterFailureCount = shell.boundaryEdgeSameParameterFailureCount;
    result.multiSurfaceBoundaryEdgeMaxSameParameterDeviation = shell.boundaryEdgeMaxSameParameterDeviation;
    result.multiSurfaceSplitBoundarySegments.clear();
    result.multiSurfaceSplitBoundarySegments.reserve(shell.splitBoundarySegments.size());
    for (const auto& segment : shell.splitBoundarySegments) {
        result.multiSurfaceSplitBoundarySegments.push_back({
            segment.edgeId,
            segment.firstParameter,
            segment.lastParameter,
            segment.edge,
            segment.patchFaceIndex});
    }
    result.multiSurfaceFailedEdgeIds = shell.failedEdgeIds;
    result.multiSurfaceFailedFaceOriginalBoundaryEdgeIds =
        shell.failedFaceOriginalBoundaryEdgeIds;
    result.multiSurfaceBoundaryEdgePcurveRebuildFailedEdgeIds = shell.boundaryEdgePcurveRebuildFailedEdgeIds;
    result.multiSurfaceBoundaryEdgeSameParameterFailedEdgeIds = shell.boundaryEdgeSameParameterFailedEdgeIds;
}

}

const char* toString(BoundaryConstrainedBuildFailureReason reason) {
    switch (reason) {
    case BoundaryConstrainedBuildFailureReason::None:
        return "None";
    case BoundaryConstrainedBuildFailureReason::InvalidInput:
        return "InvalidInput";
    case BoundaryConstrainedBuildFailureReason::InvalidBoundary:
        return "InvalidBoundary";
    case BoundaryConstrainedBuildFailureReason::NoPatchFaces:
        return "NoPatchFaces";
    case BoundaryConstrainedBuildFailureReason::ReplacementBuildFailed:
        return "ReplacementBuildFailed";
    case BoundaryConstrainedBuildFailureReason::BoundaryMismatch:
        return "BoundaryMismatch";
    case BoundaryConstrainedBuildFailureReason::ShapeFixFailed:
        return "ShapeFixFailed";
    }
    return "Unknown";
}

BoundaryConstrainedPatchBuildResult BoundaryConstrainedPatchBuilder::build(
    const PatchReplacementInput& input,
    const MultiFacePatchAnalysis& analysis,
    const BoundaryConstrainedPatchBuildOptions& options) const {
    BoundaryConstrainedPatchBuildResult result;
    auto effectiveOptions = options;
    if (input.strictOriginalBoundaryRetrim) {
        effectiveOptions.surfaceRetrimOptions.rebuildBoundaryPcurves = true;
    }

    if (input.candidate != nullptr) {
        result.sourceFaceIds = input.candidate->faces;
        result.sourceFaceCount = input.candidate->face_count > 0
            ? input.candidate->face_count
            : static_cast<int>(input.candidate->faces.size());
    }
    result.patchFaceCount = analysis.faceCount;

    if (input.boundary == nullptr || !boundary_supported(*input.boundary)) {
        return fail(result, BoundaryConstrainedBuildFailureReason::InvalidBoundary, "Boundary constrained patch build requires one valid closed outer boundary.");
    }

    const auto inputReport = validatePatchReplacementInput(input);
    if (!inputReport.success) {
        return fail(result, BoundaryConstrainedBuildFailureReason::InvalidInput, inputReport.message);
    }

    auto faces = valid_faces(analysis);
    if (faces.empty() || analysis.faceCount <= 0) {
        return fail(result, BoundaryConstrainedBuildFailureReason::NoPatchFaces, "Imported patch analysis contains no usable faces.");
    }

    const auto candidateBox = candidate_bbox(*input.document, *input.candidate);
    const auto patchBox = shape_bbox(input.importedPatch->shape);
    if (bbox_mismatch(candidateBox, patchBox, effectiveOptions.bboxToleranceRatio)) {
        result.boundaryMismatch = true;
        append_warning(result, "Patch bounding box differs from the selected candidate boundary reference.");
    }

    if (effectiveOptions.keepInternalPatchEdges) {
        result.internalPatchEdges = analysis.internalEdges;
        result.internalPatchEdgeCount = static_cast<int>(result.internalPatchEdges.size());
    }

    if (effectiveOptions.preferOriginalBoundarySurfaceRetrim) {
        TopoDS_Face sourceOrientationFace;
        if (!result.sourceFaceIds.empty()) {
            const auto sourceFaceId = result.sourceFaceIds.front();
            if (sourceFaceId >= 0 && static_cast<std::size_t>(sourceFaceId) < input.document->topology().faceCount()) {
                sourceOrientationFace = input.document->topology().face(sourceFaceId);
            }
        }

        const auto retrim = BoundaryConstrainedSurfaceRetrim().retrim(
            *input.document,
            *input.boundary,
            faces,
            sourceOrientationFace,
            effectiveOptions.surfaceRetrimOptions);
        copy_retrim_report(result, retrim);
        if (retrim.success) {
            result.replacementFaces = {retrim.replacementFace};
            result.replacementFaceCount = 1;
            result.replacementShape = retrim.replacementFace;
            result.usedOriginalBoundarySurfaceRetrim = true;
            result.success = true;
            result.failureReason = BoundaryConstrainedBuildFailureReason::None;
            result.message = retrim.message;
            append_warning(result, retrim.warningMessage);
            return result;
        }

        append_warning(result, retrim.warningMessage);
        append_warning(result, retrim.message);
        std::string replacementBuildFailureMessage = retrim.message.empty()
            ? "Boundary-constrained surface re-trim failed."
            : retrim.message;
        if (effectiveOptions.enableMultiSurfaceBoundaryShell &&
            retrim.surfaceCoverageFailedProjectionCount == 0 &&
            faces.size() > 1) {
            BoundaryConstrainedMultiSurfaceShellOptions shellOptions;
            shellOptions.samplesPerEdge = effectiveOptions.surfaceRetrimOptions.samplesPerEdge;
            shellOptions.projectionTolerance = effectiveOptions.surfaceRetrimOptions.projectionTolerance;
            shellOptions.wireConnectTolerance = effectiveOptions.surfaceRetrimOptions.projectionTolerance;

            const auto shell = BoundaryConstrainedMultiSurfaceShellBuilder().build(
                *input.document,
                *input.boundary,
                analysis,
                shellOptions);
            copy_multi_surface_report(result, shell);
            append_warning(result, shell.warningMessage);
            if (shell.success) {
                result.replacementShape = shell.replacementShape;
                result.replacementFaces = shell.replacementFaces;
                result.replacementFaceCount = static_cast<int>(result.replacementFaces.size());
                result.usedMultiSurfaceBoundaryShell = true;
                result.usedMultiFaceFragment = result.replacementFaceCount > 1;
                result.success = true;
                result.failureReason = BoundaryConstrainedBuildFailureReason::None;
                result.message = shell.message;
                return result;
            }
            append_warning(result, shell.message);
            if (!shell.message.empty()) {
                replacementBuildFailureMessage = shell.message;
            }
        }
        if (!effectiveOptions.allowPatchOuterBoundaryFallback) {
            return fail(
                result,
                BoundaryConstrainedBuildFailureReason::ReplacementBuildFailed,
                replacementBuildFailureMessage);
        }
    }

    result.replacementFaces = faces;
    result.replacementFaceCount = static_cast<int>(result.replacementFaces.size());

    if (analysis.isSingleFace && result.replacementFaceCount == 1) {
        if (!effectiveOptions.allowOneFaceSpecialPath) {
            return fail(result, BoundaryConstrainedBuildFailureReason::ReplacementBuildFailed, "One-face replacement path is disabled.");
        }
        result.replacementShape = result.replacementFaces.front();
        result.usedOneFaceSpecialPath = true;
        append_warning(result, "One-face special path starts from the imported patch face; PatchReplacementCommand may re-trim it with the original CAD boundary, and StrictTopologyGate must still validate the final document.");
    } else {
        if (!effectiveOptions.allowMultiFaceFragment) {
            return fail(result, BoundaryConstrainedBuildFailureReason::ReplacementBuildFailed, "Multi-face replacement fragment path is disabled.");
        }

        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        for (const auto& face : result.replacementFaces) {
            builder.Add(compound, face);
        }
        result.replacementShape = compound;
        result.usedMultiFaceFragment = true;
    }

    if (result.replacementShape.IsNull()) {
        return fail(result, BoundaryConstrainedBuildFailureReason::ReplacementBuildFailed, "Replacement fragment shape is empty.");
    }

    result.success = true;
    result.failureReason = BoundaryConstrainedBuildFailureReason::None;
    result.message = result.usedMultiFaceFragment
        ? "Built multi-face replacement fragment from imported patch faces."
        : "Built one-face replacement fragment from imported patch face.";
    return result;
}

}
