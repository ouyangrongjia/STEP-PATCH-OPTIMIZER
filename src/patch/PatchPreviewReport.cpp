#include "patch/PatchPreviewReport.h"

#include "brep/ShapeDocument.h"
#include "merge/MergeCandidate.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

#include <cmath>
#include <sstream>

namespace spo {

namespace {

void append_warning(PatchPreviewReport& report, const std::string& warning) {
    if (warning.empty()) {
        return;
    }
    if (!report.warningMessage.empty()) {
        report.warningMessage += " ";
    }
    report.warningMessage += warning;
}

double diagonal(
    double minX,
    double minY,
    double minZ,
    double maxX,
    double maxY,
    double maxZ) {
    const auto dx = maxX - minX;
    const auto dy = maxY - minY;
    const auto dz = maxZ - minZ;
    if (dx < 0.0 || dy < 0.0 || dz < 0.0) {
        return 0.0;
    }
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double center_distance(const PatchPreviewReport& report) {
    const auto candidateCx = (report.candidateBBoxMinX + report.candidateBBoxMaxX) * 0.5;
    const auto candidateCy = (report.candidateBBoxMinY + report.candidateBBoxMaxY) * 0.5;
    const auto candidateCz = (report.candidateBBoxMinZ + report.candidateBBoxMaxZ) * 0.5;
    const auto patchCx = (report.patchBBoxMinX + report.patchBBoxMaxX) * 0.5;
    const auto patchCy = (report.patchBBoxMinY + report.patchBBoxMaxY) * 0.5;
    const auto patchCz = (report.patchBBoxMinZ + report.patchBBoxMaxZ) * 0.5;
    const auto dx = patchCx - candidateCx;
    const auto dy = patchCy - candidateCy;
    const auto dz = patchCz - candidateCz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool candidate_bbox_from_document(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    PatchPreviewReportInput& input) {
    if (!document.hasShape() || candidate.faces.empty()) {
        return false;
    }

    Bnd_Box box;
    bool added = false;
    for (const auto faceId : candidate.faces) {
        if (faceId < 0 || static_cast<std::size_t>(faceId) >= document.topology().faceCount()) {
            continue;
        }
        BRepBndLib::Add(document.topology().face(faceId), box);
        added = true;
    }
    if (!added || box.IsVoid()) {
        return false;
    }

    input.candidateBboxValid = true;
    box.Get(
        input.candidateBBoxMinX,
        input.candidateBBoxMinY,
        input.candidateBBoxMinZ,
        input.candidateBBoxMaxX,
        input.candidateBBoxMaxY,
        input.candidateBBoxMaxZ);
    return true;
}

std::string import_failure_message(const ImportedPatchInfo& imported) {
    if (!imported.errorMessage.empty()) {
        return imported.errorMessage;
    }
    if (!imported.message.empty()) {
        return imported.message;
    }
    return "Patch import failed.";
}

}

PatchPreviewReport buildPatchPreviewReport(const PatchPreviewReportInput& input) {
    PatchPreviewReport report;
    report.candidateId = input.candidateId;
    report.sourceFaceCount = input.sourceFaceCount;
    report.sourceBoundaryEdgeCount = input.sourceBoundaryEdgeCount;
    report.localStlPath = input.artifactPaths.localStlPath;
    report.patchStepPath = input.artifactPaths.patchStepPath;
    report.patchIgesSidecarPath = input.artifactPaths.patchIgesSidecarPath;
    report.fitRegionLogPath = input.artifactPaths.fitRegionLogPath;
    report.candidateBboxValid = input.candidateBboxValid;
    report.candidateBBoxMinX = input.candidateBBoxMinX;
    report.candidateBBoxMinY = input.candidateBBoxMinY;
    report.candidateBBoxMinZ = input.candidateBBoxMinZ;
    report.candidateBBoxMaxX = input.candidateBBoxMaxX;
    report.candidateBBoxMaxY = input.candidateBBoxMaxY;
    report.candidateBBoxMaxZ = input.candidateBBoxMaxZ;

    if (input.importedPatch == nullptr) {
        report.message = "Patch preview report requires an imported patch.";
        report.recommendedAction = "Import a Geomagic patch before preview.";
        report.highRisk = true;
        return report;
    }

    const auto& imported = *input.importedPatch;
    if (!imported.success) {
        report.message = import_failure_message(imported);
        report.recommendedAction = "Regenerate the Geomagic patch or choose a valid STEP/IGES patch file.";
        report.highRisk = true;
        return report;
    }

    report.success = true;
    report.message = "Patch preview report generated.";
    report.patchFaceCount = imported.faceCount;
    report.patchEdgeCount = imported.edgeCount;
    report.patchSolidCount = imported.solidCount;
    report.patchShellCount = imported.shellCount;
    report.patchBRepCheckValid = imported.brepCheckValid;
    report.patchBboxValid = imported.bboxValid;
    report.patchBBoxMinX = imported.bboxMinX;
    report.patchBBoxMinY = imported.bboxMinY;
    report.patchBBoxMinZ = imported.bboxMinZ;
    report.patchBBoxMaxX = imported.bboxMaxX;
    report.patchBBoxMaxY = imported.bboxMaxY;
    report.patchBBoxMaxZ = imported.bboxMaxZ;

    if (report.patchStepPath.empty() && report.patchIgesSidecarPath.empty()) {
        report.patchStepPath = imported.attemptedStepPath;
    }
    if (report.patchStepPath.empty() && report.patchIgesSidecarPath.empty()) {
        report.patchStepPath = imported.sourcePath;
    }

    if (!report.patchBRepCheckValid) {
        report.highRisk = true;
        append_warning(report, "Imported patch failed BRepCheck.");
    }
    if (!report.patchBboxValid) {
        report.highRisk = true;
        append_warning(report, "Imported patch has no valid bounding box.");
    }
    if (report.patchFaceCount > 1) {
        append_warning(
            report,
            "Geomagic patch contains multiple B-rep faces. Preview is allowed, but T6 replacement may require one-face surface extraction or main-surface selection.");
    }
    if (report.patchFaceCount > 32) {
        report.highRisk = true;
        append_warning(report, "Geomagic patch face count is high for a replacement preview.");
    }

    if (report.candidateBboxValid && report.patchBboxValid) {
        const auto candidateDiag = diagonal(
            report.candidateBBoxMinX,
            report.candidateBBoxMinY,
            report.candidateBBoxMinZ,
            report.candidateBBoxMaxX,
            report.candidateBBoxMaxY,
            report.candidateBBoxMaxZ);
        const auto patchDiag = diagonal(
            report.patchBBoxMinX,
            report.patchBBoxMinY,
            report.patchBBoxMinZ,
            report.patchBBoxMaxX,
            report.patchBBoxMaxY,
            report.patchBBoxMaxZ);

        report.bboxCenterDistance = center_distance(report);
        if (candidateDiag > 0.0) {
            report.bboxDiagonalRatio = patchDiag / candidateDiag;
            if (report.bboxCenterDistance / candidateDiag > 0.25) {
                report.highRisk = true;
                append_warning(report, "Patch bounding box center is far from the selected candidate.");
            }
            if (report.bboxDiagonalRatio < 0.25 || report.bboxDiagonalRatio > 4.0) {
                report.highRisk = true;
                append_warning(report, "Patch bounding box size differs significantly from the selected candidate.");
            }
        } else {
            report.highRisk = true;
            append_warning(report, "Selected candidate bounding box has zero diagonal.");
        }
    } else if (!report.candidateBboxValid) {
        report.recommendedAction = "Select a candidate before preview to evaluate patch-to-candidate spatial deviation.";
    }

    if (report.recommendedAction.empty()) {
        report.recommendedAction = report.highRisk
            ? "Review the warning before allowing Apply in later stages."
            : "Patch overlay preview may proceed. Apply/replacement is intentionally not part of T5.3.";
    }

    return report;
}

PatchPreviewReport buildPatchPreviewReport(
    const ShapeDocument* document,
    const MergeCandidate* candidate,
    const ImportedPatchInfo& importedPatch,
    const PatchArtifactPaths& artifactPaths) {
    PatchPreviewReportInput input;
    input.importedPatch = &importedPatch;
    input.artifactPaths = artifactPaths;

    if (candidate != nullptr) {
        input.candidateId = candidate->candidate_id;
        input.sourceFaceCount = candidate->face_count > 0
            ? candidate->face_count
            : static_cast<int>(candidate->faces.size());
        input.sourceBoundaryEdgeCount = candidate->boundary_edge_count > 0
            ? candidate->boundary_edge_count
            : static_cast<int>(candidate->boundary_edges.size());
    }

    if (document != nullptr && candidate != nullptr) {
        candidate_bbox_from_document(*document, *candidate, input);
    }

    return buildPatchPreviewReport(input);
}

}
