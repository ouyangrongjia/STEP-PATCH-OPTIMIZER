#include "app/AppController.h"

#include "command/DetectFeatureCommand.h"
#include "command/ExportStepCommand.h"
#include "command/LoadStepCommand.h"
#include "command/LockedEdgeRef.h"
#include "command/LockEdgeCommand.h"
#include "command/MergePatchCommand.h"
#include "command/PlaneRegionBatchMergeCommand.h"
#include "command/PlaneRegionMergeCommand.h"
#include "command/SphereRegionBatchMergeCommand.h"
#include "command/SphereRegionMergeCommand.h"
#include "command/UnlockEdgeCommand.h"
#include "command/ValidateShapeCommand.h"
#include "io/StlReader.h"
#include "io/StlWriter.h"
#include "merge/MergePlanner.h"
#include "merge/RegionBoundaryAnalyzer.h"

#include <utility>

namespace spo {

namespace {

StlCandidateCropResult crop_error(
    const std::filesystem::path& outputPath,
    StlRegionExtractResult extract,
    std::string message) {
    StlCandidateCropResult result;
    result.outputPath = outputPath;
    result.extract = std::move(extract);
    result.message = std::move(message);
    return result;
}

}

const char* AppController::applicationName() const {
    return kApplicationName;
}

Result AppController::execute(std::unique_ptr<Command> command) {
    return history_.execute(std::move(command), context_);
}

Result AppController::undo() {
    return history_.undo(context_);
}

Result AppController::redo() {
    return history_.redo(context_);
}

bool AppController::canUndo() const {
    return history_.canUndo();
}

bool AppController::canRedo() const {
    return history_.canRedo();
}

Result AppController::openStepFile(const std::filesystem::path& path) {
    const auto result = execute(std::make_unique<LoadStepCommand>(path));
    if (result.success()) {
        history_.clear();
        context_.lockedEdges.clear();
        sourceStlMesh_.clear();
        sourceStlPath_.clear();
    }
    return result;
}

Result AppController::exportStepFile(const std::filesystem::path& path) {
    return execute(std::make_unique<ExportStepCommand>(path));
}

Result AppController::verifyStepFileReadable(const std::filesystem::path& path) {
    CommandContext readContext;
    LoadStepCommand command(path);
    return command.execute(readContext);
}

Result AppController::openStlFile(const std::filesystem::path& path) {
    const auto result = StlReader().read(path);
    if (!result.success) {
        return Result::error(result.message);
    }

    sourceStlMesh_ = result.mesh;
    sourceStlPath_ = path;
    return Result::ok();
}

bool AppController::hasSourceStl() const {
    return !sourceStlMesh_.empty();
}

const std::filesystem::path& AppController::sourceStlPath() const {
    return sourceStlPath_;
}

const StlMesh& AppController::sourceStlMesh() const {
    return sourceStlMesh_;
}

std::size_t AppController::sourceStlTriangleCount() const {
    return sourceStlMesh_.triangleCount();
}

StlBoundingBox AppController::sourceStlBoundingBox() const {
    return sourceStlMesh_.boundingBox();
}

StlCandidateCropResult AppController::cropStlForCandidate(
    const MergeCandidate& candidate,
    const std::filesystem::path& outputPath,
    const StlRegionExtractorOptions& options) const {
    return cropStlForCandidateData(context_.document, sourceStlMesh_, candidate, outputPath, options);
}

StlCandidateCropResult AppController::cropStlForCandidateData(
    const ShapeDocument& document,
    const StlMesh& sourceMesh,
    const MergeCandidate& candidate,
    const std::filesystem::path& outputPath,
    const StlRegionExtractorOptions& options) {
    if (!document.hasShape()) {
        return crop_error(outputPath, {}, "Open a STEP/STP document before cropping STL.");
    }
    if (sourceMesh.empty()) {
        return crop_error(outputPath, {}, "Open the source STL before cropping.");
    }
    if (outputPath.empty()) {
        return crop_error(outputPath, {}, "Output STL path is empty.");
    }
    if (candidate.candidate_type != MergeCandidateType::FeatureBoundedRefit) {
        return crop_error(outputPath, {}, "Current candidate is not FeatureBoundedRefit.");
    }
    if (candidate.status == MergeCandidateStatus::Rejected || candidate.status == MergeCandidateStatus::Hidden) {
        return crop_error(outputPath, {}, "Current candidate is rejected or hidden.");
    }

    const auto boundary = RegionBoundaryAnalyzer().analyze(document, candidate);
    if (!boundary.valid) {
        return crop_error(outputPath, {}, "Candidate boundary is not a valid single closed loop: " + boundary.message);
    }

    auto extract = StlRegionExtractor().extract(document, candidate, sourceMesh, options);
    if (!extract.success) {
        const auto message = extract.report.message.empty()
            ? std::string("STL crop failed.")
            : extract.report.message;
        return crop_error(outputPath, std::move(extract), message);
    }

    const auto write = StlWriter().write(extract.localMesh, outputPath);
    if (!write.success) {
        return crop_error(outputPath, std::move(extract), write.message);
    }

    StlCandidateCropResult result;
    result.success = true;
    result.extract = std::move(extract);
    result.outputPath = outputPath;
    return result;
}

FeatureEdgeDetectionResult AppController::detectFeatureEdges(double angularThresholdDegrees, double minEdgeLength) {
    const auto result = execute(std::make_unique<DetectFeatureCommand>(angularThresholdDegrees, minEdgeLength));
    if (!result.success()) {
        return {};
    }
    return context_.featureEdges;
}

MergePlannerResult AppController::previewMergeCandidates(
    double angularThresholdDegrees,
    double minEdgeLength,
    const MergePlannerOptions& options) {
    if (!context_.document.hasShape()) {
        return {};
    }

    FeatureEdgeDetector detector;
    context_.featureEdges = detector.detect(context_.document.topology(), angularThresholdDegrees, minEdgeLength);

    MergePlanner planner;
    return planner.plan(context_.document, context_.featureEdges, lockedEdges(), options);
}

SameDomainUnifyResult AppController::unifySameDomain(
    double angularThresholdDegrees,
    double minEdgeLength,
    double linearTolerance,
    bool concatBsplines) {
    auto command = std::make_unique<MergePatchCommand>(
        angularThresholdDegrees,
        minEdgeLength,
        linearTolerance,
        concatBsplines);
    auto* commandPtr = command.get();
    const auto status = execute(std::move(command));
    if (!status.success()) {
        return {};
    }
    return commandPtr->result();
}

RegionMergeResult AppController::mergePlaneCandidate(
    const MergeCandidate& candidate,
    const PlaneRegionMergeOptions& options) {
    RegionMergeResult result;
    auto command = std::make_unique<PlaneRegionMergeCommand>(candidate, options, &result);
    const auto status = execute(std::move(command));
    if (!status.success()) {
        return result;
    }
    return result;
}

RegionMergeResult AppController::mergePlaneCandidates(
    const std::vector<MergeCandidate>& candidates,
    const PlaneRegionMergeOptions& options) {
    RegionMergeResult result;
    auto command = std::make_unique<PlaneRegionBatchMergeCommand>(candidates, options, &result);
    const auto status = execute(std::move(command));
    if (!status.success()) {
        return result;
    }
    return result;
}

RegionMergeResult AppController::mergeSphereCandidate(
    const MergeCandidate& candidate,
    const SphereRegionMergeOptions& options) {
    RegionMergeResult result;
    auto command = std::make_unique<SphereRegionMergeCommand>(candidate, options, &result);
    const auto status = execute(std::move(command));
    if (!status.success()) {
        return result;
    }
    return result;
}

RegionMergeResult AppController::mergeSphereCandidates(
    const std::vector<MergeCandidate>& candidates,
    const SphereRegionMergeOptions& options) {
    RegionMergeResult result;
    auto command = std::make_unique<SphereRegionBatchMergeCommand>(candidates, options, &result);
    const auto status = execute(std::move(command));
    if (!status.success()) {
        return result;
    }
    return result;
}

bool AppController::hasDocument() const {
    return context_.document.hasShape();
}

const ShapeDocument& AppController::document() const {
    return context_.document;
}

const FeatureEdgeDetectionResult& AppController::featureEdges() const {
    return context_.featureEdges;
}

ShapeValidationReport AppController::validateShape() {
    const auto result = execute(std::make_unique<ValidateShapeCommand>());
    if (!result.success()) {
        return {};
    }
    return context_.validationReport;
}

Result AppController::lockEdges(const std::vector<EdgeId>& edgeIds) {
    return execute(std::make_unique<LockEdgeCommand>(edgeIds));
}

Result AppController::unlockEdges(const std::vector<EdgeId>& edgeIds) {
    return execute(std::make_unique<UnlockEdgeCommand>(edgeIds));
}

std::set<EdgeId> AppController::lockedEdges() const {
    return lockedEdgeIds(context_.lockedEdges);
}

const CommandHistory& AppController::history() const {
    return history_;
}

}
