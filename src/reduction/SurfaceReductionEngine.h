#pragma once

#include "brep/ShapeDocument.h"
#include "common/GeometryTypes.h"
#include "reduction/StlHybridParameterization.h"
#include "stl/StlMesh.h"
#include "validate/CommercialCadQualityGate.h"
#include "validate/StrictTopologyGate.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <set>
#include <memory>
#include <string>
#include <vector>

namespace spo {

enum class SurfaceReductionOutcome {
    InvalidSelection,
    NoAcceptableReduction,
    Reduced,
    TargetAchieved
};

const char* toString(SurfaceReductionOutcome outcome);

struct SurfaceReductionOptions {
    double tolerance3d = 0.03;
    double normalToleranceDegrees = 15.0;
    double smoothSeamToleranceDegrees = 5.0;
    double targetOutputRatio = 0.5;
    int maximumControlPoints = 1024;
    double stlReferenceWeight = 0.25;
    double maximumStlReferenceDistance = 0.2;
};

struct SurfaceReductionRequest {
    std::vector<FaceId> selectedFaces;
    std::set<EdgeId> lockedEdges;
    SurfaceReductionOptions options;
    std::shared_ptr<const StlMesh> referenceStl;
    bool referenceStlIsSelectionCrop = false;
};

struct SurfaceReductionRejectedGroup {
    std::vector<FaceId> faces;
    std::string reason;
};

struct SurfaceReductionReport {
    SurfaceReductionOutcome outcome = SurfaceReductionOutcome::InvalidSelection;
    int inputFaceCount = 0;
    int outputFaceCount = 0;
    double reductionRatio = 0.0;
    bool targetAchieved = false;
    bool allQualityGatesPassed = false;
    bool selectionConnected = false;
    bool parameterizationValid = false;
    double maxDistance = 0.0;
    double maxNormalAngleDegrees = 0.0;
    double maxSmoothSeamAngleDegrees = 0.0;
    int attemptedMergeCount = 0;
    int acceptedMergeCount = 0;
    bool hybridParameterizationUsed = false;
    int stlReferenceSampleCount = 0;
    double maxStepToStlDistance = 0.0;
    StlHybridParameterizationReport hybridParameterization;
    std::vector<std::vector<FaceId>> outputGroups;
    std::vector<SurfaceReductionRejectedGroup> rejectedGroups;
    std::vector<CommercialCadQualityGateReport> commercialCadReports;
    StrictTopologyGateReport topologyReport;
    std::string message;
};

struct SurfaceReductionReplacement {
    std::vector<FaceId> sourceFaces;
    TopoDS_Face replacementFace;
};

struct SurfaceReductionResult {
    TopoDS_Shape sourceShape;
    TopoDS_Shape previewShape;
    TopoDS_Shape reducedShape;
    SurfaceReductionReport report;
    std::vector<SurfaceReductionReplacement> replacements;

    bool canApply() const {
        return (report.outcome == SurfaceReductionOutcome::Reduced ||
                report.outcome == SurfaceReductionOutcome::TargetAchieved) &&
            report.allQualityGatesPassed && !reducedShape.IsNull();
    }
};

class SurfaceReductionEngine {
public:
    SurfaceReductionResult reduce(
        const ShapeDocument& document,
        const SurfaceReductionRequest& request) const;
};

}
