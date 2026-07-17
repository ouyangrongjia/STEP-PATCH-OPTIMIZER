#include "reduction/SurfaceReductionFitCache.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS.hxx>
#include <Standard_Version.hxx>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <sstream>

namespace spo {
namespace {

constexpr int cacheSchema = 1;

QJsonObject timingToJson(const RestrictedBSplinePatchFitTiming& value) {
    return {
        {"topologyPreparationMs", value.topologyPreparationMs},
        {"scanCropMs", value.scanCropMs},
        {"samplingMs", value.samplingMs},
        {"parameterizationMs", value.parameterizationMs},
        {"kktSolveMs", value.kktSolveMs},
        {"uvProjectionMs", value.uvProjectionMs},
        {"boundaryRefinementMs", value.boundaryRefinementMs},
        {"validationMs", value.validationMs},
        {"retrimMs", value.retrimMs},
        {"brepCheckMs", value.brepCheckMs},
        {"totalMs", value.totalMs}};
}

RestrictedBSplinePatchFitTiming timingFromJson(const QJsonObject& value) {
    RestrictedBSplinePatchFitTiming result;
    result.topologyPreparationMs = value["topologyPreparationMs"].toDouble();
    result.scanCropMs = value["scanCropMs"].toDouble();
    result.samplingMs = value["samplingMs"].toDouble();
    result.parameterizationMs = value["parameterizationMs"].toDouble();
    result.kktSolveMs = value["kktSolveMs"].toDouble();
    result.uvProjectionMs = value["uvProjectionMs"].toDouble();
    result.boundaryRefinementMs = value["boundaryRefinementMs"].toDouble();
    result.validationMs = value["validationMs"].toDouble();
    result.retrimMs = value["retrimMs"].toDouble();
    result.brepCheckMs = value["brepCheckMs"].toDouble();
    result.totalMs = value["totalMs"].toDouble();
    return result;
}

QJsonObject reportToJson(const RestrictedBSplinePatchFitReport& value) {
    QJsonArray uvIterations;
    for (const auto& iteration : value.uvIterations) {
        uvIterations.append(QJsonObject {
            {"iteration", iteration.iteration},
            {"objectiveBefore", iteration.objectiveBefore},
            {"objectiveAfter", iteration.objectiveAfter},
            {"maximumShift", iteration.maximumShift},
            {"backtrackingSteps", iteration.backtrackingSteps},
            {"accepted", iteration.accepted},
            {"reason", QString::fromStdString(iteration.reason)}});
    }
    return {
        {"success", value.success},
        {"failureStage", static_cast<int>(value.failureStage)},
        {"message", QString::fromStdString(value.message)},
        {"trainingSampleCount", value.trainingSampleCount},
        {"selectionSampleCount", value.selectionSampleCount},
        {"holdoutSampleCount", value.holdoutSampleCount},
        {"boundaryHoldoutSampleCount", value.boundaryHoldoutSampleCount},
        {"controlPointCount", value.controlPointCount},
        {"uControlPointCount", value.uControlPointCount},
        {"vControlPointCount", value.vControlPointCount},
        {"iterationCount", value.iterationCount},
        {"knotInsertionCount", value.knotInsertionCount},
        {"boundaryConstraintCount", value.boundaryConstraintCount},
        {"boundaryConstraintRefinementCount", value.boundaryConstraintRefinementCount},
        {"exteriorGuardSampleCount", value.exteriorGuardSampleCount},
        {"maxBoundaryConstraintResidual", value.maxBoundaryConstraintResidual},
        {"maxBoundaryUvShift", value.maxBoundaryUvShift},
        {"maxBoundaryDistance", value.maxBoundaryDistance},
        {"supportSurfaceExtentRatio", value.supportSurfaceExtentRatio},
        {"supportSurfaceMaxPoleOverflowRatio", value.supportSurfaceMaxPoleOverflowRatio},
        {"maxSelectionDistance", value.maxSelectionDistance},
        {"maxSelectionNormalAngleDegrees", value.maxSelectionNormalAngleDegrees},
        {"maxHoldoutDistance", value.maxHoldoutDistance},
        {"maxHoldoutNormalAngleDegrees", value.maxHoldoutNormalAngleDegrees},
        {"minimumNormalizedJacobian", value.minimumNormalizedJacobian},
        {"parameterizationValid", value.parameterizationValid},
        {"selectionPassed", value.selectionPassed},
        {"holdoutPassed", value.holdoutPassed},
        {"brepCheckValid", value.brepCheckValid},
        {"parameterizationSource", static_cast<int>(value.parameterizationSource)},
        {"scanReferenceCoverage", static_cast<int>(value.scanReferenceCoverage)},
        {"scanReferenceSampleCount", value.scanReferenceSampleCount},
        {"maxStepToScanDistance", value.maxStepToScanDistance},
        {"kktSolveCount", value.kktSolveCount},
        {"boundaryProjectionCount", value.boundaryProjectionCount},
        {"scanIndexCandidateTriangleCount", value.scanIndexCandidateTriangleCount},
        {"parameterization", QJsonObject {
            {"success", value.parameterization.success},
            {"source", static_cast<int>(value.parameterization.source)},
            {"message", QString::fromStdString(value.parameterization.message)},
            {"sourceTriangleCount", value.parameterization.sourceTriangleCount},
            {"coarseTriangleCount", value.parameterization.coarseTriangleCount},
            {"croppedTriangleCount", value.parameterization.croppedTriangleCount},
            {"vertexCount", value.parameterization.vertexCount},
            {"boundaryVertexCount", value.parameterization.boundaryVertexCount},
            {"flippedTriangleCount", value.parameterization.flippedTriangleCount},
            {"minimumNormalizedJacobian", value.parameterization.minimumNormalizedJacobian}}},
        {"timing", timingToJson(value.timing)},
        {"uvIterations", uvIterations}};
}

RestrictedBSplinePatchFitReport reportFromJson(const QJsonObject& value) {
    RestrictedBSplinePatchFitReport result;
    result.success = value["success"].toBool();
    result.failureStage = static_cast<RestrictedBSplinePatchFitFailureStage>(value["failureStage"].toInt());
    result.message = value["message"].toString().toStdString();
    result.trainingSampleCount = value["trainingSampleCount"].toInt();
    result.selectionSampleCount = value["selectionSampleCount"].toInt();
    result.holdoutSampleCount = value["holdoutSampleCount"].toInt();
    result.boundaryHoldoutSampleCount = value["boundaryHoldoutSampleCount"].toInt();
    result.controlPointCount = value["controlPointCount"].toInt();
    result.uControlPointCount = value["uControlPointCount"].toInt();
    result.vControlPointCount = value["vControlPointCount"].toInt();
    result.iterationCount = value["iterationCount"].toInt();
    result.knotInsertionCount = value["knotInsertionCount"].toInt();
    result.boundaryConstraintCount = value["boundaryConstraintCount"].toInt();
    result.boundaryConstraintRefinementCount = value["boundaryConstraintRefinementCount"].toInt();
    result.exteriorGuardSampleCount = value["exteriorGuardSampleCount"].toInt();
    result.maxBoundaryConstraintResidual = value["maxBoundaryConstraintResidual"].toDouble();
    result.maxBoundaryUvShift = value["maxBoundaryUvShift"].toDouble();
    result.maxBoundaryDistance = value["maxBoundaryDistance"].toDouble();
    result.supportSurfaceExtentRatio = value["supportSurfaceExtentRatio"].toDouble();
    result.supportSurfaceMaxPoleOverflowRatio = value["supportSurfaceMaxPoleOverflowRatio"].toDouble();
    result.maxSelectionDistance = value["maxSelectionDistance"].toDouble();
    result.maxSelectionNormalAngleDegrees = value["maxSelectionNormalAngleDegrees"].toDouble();
    result.maxHoldoutDistance = value["maxHoldoutDistance"].toDouble();
    result.maxHoldoutNormalAngleDegrees = value["maxHoldoutNormalAngleDegrees"].toDouble();
    result.minimumNormalizedJacobian = value["minimumNormalizedJacobian"].toDouble();
    result.parameterizationValid = value["parameterizationValid"].toBool();
    result.selectionPassed = value["selectionPassed"].toBool();
    result.holdoutPassed = value["holdoutPassed"].toBool();
    result.brepCheckValid = value["brepCheckValid"].toBool();
    result.parameterizationSource = static_cast<SurfaceParameterizationSource>(value["parameterizationSource"].toInt());
    result.scanReferenceCoverage = static_cast<ScanReferenceCoverage>(value["scanReferenceCoverage"].toInt());
    result.scanReferenceSampleCount = value["scanReferenceSampleCount"].toInt();
    result.maxStepToScanDistance = value["maxStepToScanDistance"].toDouble();
    result.kktSolveCount = value["kktSolveCount"].toInt();
    result.boundaryProjectionCount = value["boundaryProjectionCount"].toInt();
    result.scanIndexCandidateTriangleCount = value["scanIndexCandidateTriangleCount"].toInt();
    const auto parameterization = value["parameterization"].toObject();
    result.parameterization.success = parameterization["success"].toBool();
    result.parameterization.source = static_cast<SurfaceParameterizationSource>(parameterization["source"].toInt());
    result.parameterization.message = parameterization["message"].toString().toStdString();
    result.parameterization.sourceTriangleCount = parameterization["sourceTriangleCount"].toInt();
    result.parameterization.coarseTriangleCount = parameterization["coarseTriangleCount"].toInt();
    result.parameterization.croppedTriangleCount = parameterization["croppedTriangleCount"].toInt();
    result.parameterization.vertexCount = parameterization["vertexCount"].toInt();
    result.parameterization.boundaryVertexCount = parameterization["boundaryVertexCount"].toInt();
    result.parameterization.flippedTriangleCount = parameterization["flippedTriangleCount"].toInt();
    result.parameterization.minimumNormalizedJacobian = parameterization["minimumNormalizedJacobian"].toDouble();
    result.timing = timingFromJson(value["timing"].toObject());
    for (const auto& item : value["uvIterations"].toArray()) {
        const auto iteration = item.toObject();
        result.uvIterations.push_back({
            iteration["iteration"].toInt(),
            iteration["objectiveBefore"].toDouble(),
            iteration["objectiveAfter"].toDouble(),
            iteration["maximumShift"].toDouble(),
            iteration["backtrackingSteps"].toInt(),
            iteration["accepted"].toBool(),
            iteration["reason"].toString().toStdString()});
    }
    return result;
}

QJsonObject fitOptionsToJson(const RestrictedBSplinePatchFitOptions& value) {
    return {
        {"tolerance3d", value.tolerance3d},
        {"normalToleranceDegrees", value.normalToleranceDegrees},
        {"minimumNormalizedJacobian", value.minimumNormalizedJacobian},
        {"fairnessWeight", value.fairnessWeight},
        {"maximumControlPoints", value.maximumControlPoints},
        {"maximumBoundaryConstraintRefinements", value.maximumBoundaryConstraintRefinements},
        {"maximumUvIterations", value.maximumUvIterations},
        {"maximumUvBacktrackingSteps", value.maximumUvBacktrackingSteps},
        {"uvConvergenceTolerance", value.uvConvergenceTolerance},
        {"scanReferenceCoverage", static_cast<int>(value.scanReferenceCoverage)},
        {"scanReferenceWeight", value.scanReferenceWeight},
        {"maximumScanReferenceDistance", value.maximumScanReferenceDistance}};
}

}

SurfaceReductionFitCache::SurfaceReductionFitCache(SurfaceReductionExecutionOptions execution)
    : execution_(std::move(execution)) {}

std::string SurfaceReductionFitCache::key(
    const std::vector<FaceId>& faces,
    const RestrictedBSplinePatchFitOptions& options) const {
    QJsonArray faceIds;
    for (const auto face : faces) faceIds.append(QString::number(face));
    QJsonObject identity {
        {"schema", cacheSchema},
        {"source", QString::fromStdString(execution_.sourceFingerprint)},
        {"scan", QString::fromStdString(execution_.scanFingerprint)},
        {"algorithm", QString::fromStdString(execution_.algorithmFingerprint)},
        {"build", QStringLiteral("qt-%1|occt-%2|ptr-%3|%4")
            .arg(QT_VERSION_STR)
            .arg(OCC_VERSION_COMPLETE)
            .arg(static_cast<qulonglong>(sizeof(void*)))
            .arg(
#ifdef _DEBUG
            "debug"
#else
            "release"
#endif
        )},
        {"faces", faceIds},
        {"options", fitOptionsToJson(options)}};
    return QCryptographicHash::hash(
        QJsonDocument(identity).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex().toStdString();
}

std::optional<SurfaceReductionFitCacheEntry> SurfaceReductionFitCache::load(
    const std::string& keyValue) const {
    const auto root = QString::fromStdWString(execution_.cacheDirectory.wstring());
    const auto key = QString::fromStdString(keyValue);
    QFile file(QDir(root).filePath(key.left(2) + "/" + key + ".json"));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    const auto object = document.object();
    if (object["schema"].toInt() != cacheSchema || object["key"].toString() != key ||
        !object["complete"].toBool() || !object["report"].isObject()) {
        return std::nullopt;
    }
    SurfaceReductionFitCacheEntry entry;
    entry.success = object["success"].toBool();
    entry.report = reportFromJson(object["report"].toObject());
    if (!entry.success) return entry;
    const auto brep = QByteArray::fromBase64(object["brep"].toString().toLatin1());
    if (brep.isEmpty()) return std::nullopt;
    try {
        std::istringstream stream(std::string(brep.constData(), static_cast<std::size_t>(brep.size())));
        TopoDS_Shape shape;
        BRep_Builder builder;
        BRepTools::Read(shape, stream, builder);
        if (shape.IsNull() || shape.ShapeType() != TopAbs_FACE) return std::nullopt;
        entry.face = TopoDS::Face(shape);
        entry.surface = BRep_Tool::Surface(entry.face);
        if (entry.surface.IsNull()) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return entry;
}

bool SurfaceReductionFitCache::store(
    const std::string& keyValue,
    const SurfaceReductionFitCacheEntry& entry) const {
    const auto root = QString::fromStdWString(execution_.cacheDirectory.wstring());
    const auto key = QString::fromStdString(keyValue);
    const auto directory = QDir(root).filePath(key.left(2));
    if (!QDir().mkpath(directory)) return false;
    QString brep;
    if (entry.success) {
        if (entry.face.IsNull()) return false;
        try {
            std::ostringstream stream;
            BRepTools::Write(entry.face, stream);
            brep = QString::fromLatin1(QByteArray::fromStdString(stream.str()).toBase64());
        } catch (...) {
            return false;
        }
    }
    const QJsonObject object {
        {"schema", cacheSchema},
        {"key", key},
        {"complete", true},
        {"success", entry.success},
        {"report", reportToJson(entry.report)},
        {"brep", brep}};
    QSaveFile file(QDir(directory).filePath(key + ".json"));
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0) return false;
    return file.commit();
}

}
