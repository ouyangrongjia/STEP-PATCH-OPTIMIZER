#include "patch/BoundaryConstrainedSurfaceRetrim.h"

#include "brep/BoundaryWireBuilder.h"
#include "brep/ShapeDocument.h"
#include "brep/TopologyGraph.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace spo {

namespace {

BoundaryConstrainedSurfaceRetrimResult fail(std::string message) {
    BoundaryConstrainedSurfaceRetrimResult result;
    result.message = std::move(message);
    return result;
}

double edge_length(const TopoDS_Edge& edge) {
    GProp_GProps properties;
    BRepGProp::LinearProperties(edge, properties);
    return std::max(0.0, properties.Mass());
}

BoundaryConstrainedSurfaceCandidateReport evaluate_surface(
    const TopoDS_Face& face,
    int patchFaceIndex,
    const std::vector<BoundarySurfaceSample>& samples,
    double projectionTolerance) {
    BoundaryConstrainedSurfaceCandidateReport report;
    report.patchFaceIndex = patchFaceIndex;
    report.boundarySampleCount = static_cast<int>(samples.size());

    const auto surface = BRep_Tool::Surface(face);
    if (surface.IsNull() || samples.empty()) {
        report.failedProjectionCount = report.boundarySampleCount;
        return report;
    }

    double totalDistance = 0.0;
    for (const auto& sample : samples) {
        const auto bestDistance = projectionDistanceToSurface(sample.point, surface);
        report.maxProjectionDistance = std::max(report.maxProjectionDistance, bestDistance);
        totalDistance += bestDistance;
        if (bestDistance <= projectionTolerance) {
            ++report.projectedSampleCount;
        } else {
            ++report.failedProjectionCount;
        }
    }

    report.averageProjectionDistance = samples.empty()
        ? 0.0
        : totalDistance / static_cast<double>(samples.size());
    report.accepted = report.boundarySampleCount > 0 && report.failedProjectionCount == 0;
    return report;
}

bool better_candidate(
    const BoundaryConstrainedSurfaceCandidateReport& lhs,
    const BoundaryConstrainedSurfaceCandidateReport& rhs) {
    if (lhs.failedProjectionCount != rhs.failedProjectionCount) {
        return lhs.failedProjectionCount < rhs.failedProjectionCount;
    }
    if (lhs.maxProjectionDistance != rhs.maxProjectionDistance) {
        return lhs.maxProjectionDistance < rhs.maxProjectionDistance;
    }
    return lhs.averageProjectionDistance < rhs.averageProjectionDistance;
}

}

std::vector<BoundarySurfaceSample> sampleBoundaryForSurfaceProjection(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& boundary,
    int samplesPerEdge) {
    std::vector<BoundarySurfaceSample> samples;
    const auto& topology = document.topology();
    const auto minCount = std::max(2, samplesPerEdge);
    for (const auto edgeId : boundary.ordered_boundary_edges) {
        if (edgeId >= topology.edgeCount()) {
            continue;
        }
        const auto& edge = topology.edge(edgeId);
        double firstParameter = 0.0;
        double lastParameter = 0.0;
        const auto curve = BRep_Tool::Curve(edge, firstParameter, lastParameter);
        if (curve.IsNull()) {
            continue;
        }

        const auto length = edge_length(edge);
        const auto count = std::clamp(
            std::max(minCount, static_cast<int>(std::ceil(length / 0.5)) + 1),
            2,
            96);
        for (int index = 0; index < count; ++index) {
            const double ratio = count == 1
                ? 0.0
                : static_cast<double>(index) / static_cast<double>(count - 1);
            const auto parameter = firstParameter + (lastParameter - firstParameter) * ratio;
            samples.push_back({edgeId, parameter, curve->Value(parameter)});
        }
    }
    return samples;
}

double projectionDistanceToSurface(
    const gp_Pnt& point,
    const Handle(Geom_Surface)& surface) {
    if (surface.IsNull()) {
        return std::numeric_limits<double>::infinity();
    }

    GeomAPI_ProjectPointOnSurf projector(point, surface);
    if (!projector.IsDone() || projector.NbPoints() == 0) {
        return std::numeric_limits<double>::infinity();
    }

    auto bestDistance = std::numeric_limits<double>::infinity();
    for (int index = 1; index <= projector.NbPoints(); ++index) {
        bestDistance = std::min(bestDistance, projector.Distance(index));
    }
    return bestDistance;
}

BoundarySurfaceCoverageReport evaluateBoundarySurfaceCoverage(
    const std::vector<TopoDS_Face>& faces,
    const std::vector<BoundarySurfaceSample>& samples,
    double projectionTolerance) {
    BoundarySurfaceCoverageReport report;
    report.boundarySampleCount = static_cast<int>(samples.size());
    if (samples.empty()) {
        return report;
    }

    std::vector<Handle(Geom_Surface)> surfaces;
    surfaces.reserve(faces.size());
    for (const auto& face : faces) {
        if (face.IsNull()) {
            continue;
        }
        const auto surface = BRep_Tool::Surface(face);
        if (!surface.IsNull()) {
            surfaces.push_back(surface);
        }
    }

    double totalDistance = 0.0;
    for (const auto& sample : samples) {
        auto bestDistance = std::numeric_limits<double>::infinity();
        for (const auto& surface : surfaces) {
            bestDistance = std::min(
                bestDistance,
                projectionDistanceToSurface(sample.point, surface));
        }

        report.maxProjectionDistance = std::max(report.maxProjectionDistance, bestDistance);
        totalDistance += bestDistance;
        if (bestDistance <= projectionTolerance) {
            ++report.projectedSampleCount;
        } else {
            ++report.failedProjectionCount;
            if (std::find(report.uncoveredEdgeIds.begin(), report.uncoveredEdgeIds.end(), sample.edgeId) ==
                report.uncoveredEdgeIds.end()) {
                report.uncoveredEdgeIds.push_back(sample.edgeId);
            }
        }
    }

    report.averageProjectionDistance = totalDistance / static_cast<double>(samples.size());
    return report;
}

BoundaryConstrainedSurfaceRetrimResult BoundaryConstrainedSurfaceRetrim::retrim(
    const ShapeDocument& document,
    const RegionBoundaryAnalysis& boundary,
    const std::vector<TopoDS_Face>& patchFaces,
    const TopoDS_Face& sourceOrientationFace,
    const BoundaryConstrainedSurfaceRetrimOptions& options) const {
    if (options.samplesPerEdge < 2) {
        return fail("Boundary-constrained surface re-trim requires at least two samples per edge.");
    }
    if (options.projectionTolerance < 0.0) {
        return fail("Boundary-constrained surface re-trim projection tolerance must not be negative.");
    }
    if (!document.hasShape()) {
        return fail("Boundary-constrained surface re-trim requires a loaded document.");
    }
    if (patchFaces.empty()) {
        return fail("Boundary-constrained surface re-trim requires at least one patch face.");
    }

    const auto wire = BoundaryWireBuilder().buildOuterWire(document, boundary);
    if (!wire.success) {
        return fail(wire.message);
    }

    const auto samples = sampleBoundaryForSurfaceProjection(document, boundary, options.samplesPerEdge);
    if (samples.empty()) {
        return fail("Boundary-constrained surface re-trim could not sample the original CAD boundary.");
    }

    BoundaryConstrainedSurfaceRetrimResult result;
    result.candidates.reserve(patchFaces.size());
    const auto coverage = evaluateBoundarySurfaceCoverage(
        patchFaces,
        samples,
        options.projectionTolerance);
    result.surfaceCoverageProjectedSampleCount = coverage.projectedSampleCount;
    result.surfaceCoverageFailedProjectionCount = coverage.failedProjectionCount;
    result.surfaceCoverageMaxProjectionDistance = coverage.maxProjectionDistance;
    result.surfaceCoverageAverageProjectionDistance = coverage.averageProjectionDistance;
    result.surfaceCoverageUncoveredEdgeIds = coverage.uncoveredEdgeIds;

    for (std::size_t index = 0; index < patchFaces.size(); ++index) {
        if (patchFaces[index].IsNull()) {
            continue;
        }
        result.candidates.push_back(evaluate_surface(
            patchFaces[index],
            static_cast<int>(index),
            samples,
            options.projectionTolerance));
    }
    if (result.candidates.empty()) {
        return fail("Boundary-constrained surface re-trim found no usable patch faces.");
    }

    const auto selected = std::min_element(
        result.candidates.begin(),
        result.candidates.end(),
        better_candidate);
    result.selectedPatchFaceIndex = selected->patchFaceIndex;
    result.boundarySampleCount = selected->boundarySampleCount;
    result.projectedSampleCount = selected->projectedSampleCount;
    result.failedProjectionCount = selected->failedProjectionCount;
    result.maxProjectionDistance = selected->maxProjectionDistance;
    result.averageProjectionDistance = selected->averageProjectionDistance;

    if (!selected->accepted) {
        if (result.surfaceCoverageFailedProjectionCount == 0) {
            result.message = "No single imported Geomagic surface covers the original CAD boundary loop within projection tolerance.";
            result.warningMessage = "The patch requires multi-surface boundary-constrained shell construction; direct patch outer-boundary replacement remains disabled.";
        } else {
            result.message = "No imported Geomagic surface set covers the original CAD boundary loop within projection tolerance.";
            result.warningMessage = "Some original CAD boundary samples are not supported by any imported Geomagic surface; direct patch outer-boundary replacement remains disabled.";
        }
        return result;
    }

    const auto surface = BRep_Tool::Surface(patchFaces[static_cast<std::size_t>(result.selectedPatchFaceIndex)]);
    BRepBuilderAPI_MakeFace faceBuilder(surface, wire.wire, Standard_True);
    if (!faceBuilder.IsDone()) {
        return fail("Could not re-trim the selected Geomagic surface with the original CAD boundary wire.");
    }

    result.replacementFace = faceBuilder.Face();
    if (!sourceOrientationFace.IsNull()) {
        result.replacementFace.Orientation(sourceOrientationFace.Orientation());
    }
    BRepLib::BuildCurves3d(result.replacementFace);

    result.success = true;
    result.message = "Re-trimmed selected Geomagic surface with the original CAD boundary wire.";
    return result;
}

}
