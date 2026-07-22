#include "reduction/region_refit/RegionalSurfaceRefitCache.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QSaveFile>
#include <QString>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace spo {

namespace {

constexpr quint32 kSamplingMagic = 0x52534653u;
constexpr quint32 kLayoutMagic = 0x5253464cu;
constexpr quint32 kFitMagic = 0x52534646u;
constexpr quint32 kVersion = 1;
constexpr quint64 kMaximumSerializedCount = 100000000ull;

void configure(QDataStream& stream) {
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
}

bool readCount(QDataStream& stream, std::size_t& result) {
    quint64 value = 0;
    stream >> value;
    if (stream.status() != QDataStream::Ok || value > kMaximumSerializedCount) return false;
    result = static_cast<std::size_t>(value);
    return true;
}

void writeString(QDataStream& stream, const std::string& value) {
    stream << QString::fromStdString(value);
}

bool readString(QDataStream& stream, std::string& value) {
    QString text;
    stream >> text;
    if (stream.status() != QDataStream::Ok) return false;
    value = text.toStdString();
    return true;
}

void writeSamplingReport(QDataStream& stream, const StepAdaptiveRegionSamplingReport& report) {
    stream << qint32(report.status);
    writeString(stream, report.message);
    stream << quint64(report.sourceFaceCount) << quint64(report.vertexCount)
           << quint64(report.triangleCount) << quint64(report.boundarySampleCount)
           << quint64(report.boundaryEndpointCount) << quint64(report.internalSeamVertexCount)
           << quint64(report.weldedInternalSeamVertexCount) << quint64(report.normalRefinementCount)
           << quint64(report.discardedDegenerateTriangleCount)
           << quint64(report.normalDiscontinuityVertexCount)
           << report.linearDeflection << report.angularDeflectionDegrees
           << report.boundaryChordTolerance << report.maximumInteriorChordError
           << report.maximumInteriorNormalErrorDegrees
           << quint64(report.maximumInteriorNormalFaceId)
           << quint64(report.maximumInteriorNormalTriangleIndex)
           << report.maximumBoundaryChordError;
    writeString(stream, report.meshFingerprint);
}

bool readSamplingReport(QDataStream& stream, StepAdaptiveRegionSamplingReport& report) {
    qint32 status = 0;
    quint64 sourceFaces = 0;
    quint64 vertices = 0;
    quint64 triangles = 0;
    quint64 boundarySamples = 0;
    quint64 endpoints = 0;
    quint64 internalSeams = 0;
    quint64 weldedSeams = 0;
    quint64 normalRefinements = 0;
    quint64 discarded = 0;
    quint64 discontinuities = 0;
    quint64 maximumNormalFace = 0;
    quint64 maximumNormalTriangle = 0;
    stream >> status;
    if (!readString(stream, report.message)) return false;
    stream >> sourceFaces >> vertices >> triangles >> boundarySamples >> endpoints
           >> internalSeams >> weldedSeams >> normalRefinements >> discarded >> discontinuities
           >> report.linearDeflection >> report.angularDeflectionDegrees
           >> report.boundaryChordTolerance >> report.maximumInteriorChordError
           >> report.maximumInteriorNormalErrorDegrees >> maximumNormalFace
           >> maximumNormalTriangle >> report.maximumBoundaryChordError;
    if (!readString(stream, report.meshFingerprint) || stream.status() != QDataStream::Ok) return false;
    report.status = static_cast<RegionRefitStatus>(status);
    report.sourceFaceCount = static_cast<std::size_t>(sourceFaces);
    report.vertexCount = static_cast<std::size_t>(vertices);
    report.triangleCount = static_cast<std::size_t>(triangles);
    report.boundarySampleCount = static_cast<std::size_t>(boundarySamples);
    report.boundaryEndpointCount = static_cast<std::size_t>(endpoints);
    report.internalSeamVertexCount = static_cast<std::size_t>(internalSeams);
    report.weldedInternalSeamVertexCount = static_cast<std::size_t>(weldedSeams);
    report.normalRefinementCount = static_cast<std::size_t>(normalRefinements);
    report.discardedDegenerateTriangleCount = static_cast<std::size_t>(discarded);
    report.normalDiscontinuityVertexCount = static_cast<std::size_t>(discontinuities);
    report.maximumInteriorNormalFaceId = static_cast<FaceId>(maximumNormalFace);
    report.maximumInteriorNormalTriangleIndex = static_cast<std::size_t>(maximumNormalTriangle);
    return true;
}

void writeMesh(QDataStream& stream, const RegionReferenceMesh& mesh) {
    stream << quint64(mesh.vertices.size());
    for (const auto& vertex : mesh.vertices) {
        stream << vertex.point.X() << vertex.point.Y() << vertex.point.Z()
               << vertex.normal.X() << vertex.normal.Y() << vertex.normal.Z()
               << vertex.boundary << vertex.hardBoundary << vertex.boundaryEndpoint
               << vertex.hardCorner << vertex.internalSeam << vertex.normalDiscontinuity;
        stream << quint64(vertex.boundaryEdgeIds.size());
        for (const auto edgeId : vertex.boundaryEdgeIds) stream << quint64(edgeId);
        stream << quint64(vertex.sources.size());
        for (const auto& source : vertex.sources) {
            stream << quint64(source.faceId) << source.uv.X() << source.uv.Y()
                   << source.normal.X() << source.normal.Y() << source.normal.Z()
                   << source.normalDefined << source.minimumCurvature
                   << source.maximumCurvature << source.curvatureDefined;
        }
    }
    stream << quint64(mesh.triangles.size());
    for (const auto& triangle : mesh.triangles) {
        stream << quint64(triangle.vertices[0]) << quint64(triangle.vertices[1])
               << quint64(triangle.vertices[2]) << quint64(triangle.faceId);
    }
    stream << quint64(mesh.boundarySamples.size());
    for (const auto& sample : mesh.boundarySamples) {
        stream << quint64(sample.edgeId) << sample.parameter << quint64(sample.vertexIndex)
               << sample.endpoint << sample.hardCorner;
    }
}

bool readMesh(QDataStream& stream, RegionReferenceMesh& mesh) {
    std::size_t count = 0;
    if (!readCount(stream, count)) return false;
    mesh.vertices.resize(count);
    for (auto& vertex : mesh.vertices) {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        double nx = 0.0;
        double ny = 0.0;
        double nz = 1.0;
        stream >> x >> y >> z >> nx >> ny >> nz
               >> vertex.boundary >> vertex.hardBoundary >> vertex.boundaryEndpoint
               >> vertex.hardCorner >> vertex.internalSeam >> vertex.normalDiscontinuity;
        if (stream.status() != QDataStream::Ok || nx * nx + ny * ny + nz * nz <= 1.0e-24) return false;
        vertex.point = gp_Pnt(x, y, z);
        vertex.normal = gp_Dir(nx, ny, nz);
        if (!readCount(stream, count)) return false;
        vertex.boundaryEdgeIds.resize(count);
        for (auto& edgeId : vertex.boundaryEdgeIds) {
            quint64 value = 0;
            stream >> value;
            edgeId = static_cast<EdgeId>(value);
        }
        if (!readCount(stream, count)) return false;
        vertex.sources.resize(count);
        for (auto& source : vertex.sources) {
            quint64 faceId = 0;
            double u = 0.0;
            double v = 0.0;
            stream >> faceId >> u >> v >> nx >> ny >> nz
                   >> source.normalDefined >> source.minimumCurvature
                   >> source.maximumCurvature >> source.curvatureDefined;
            if (stream.status() != QDataStream::Ok || nx * nx + ny * ny + nz * nz <= 1.0e-24) return false;
            source.faceId = static_cast<FaceId>(faceId);
            source.uv = gp_Pnt2d(u, v);
            source.normal = gp_Dir(nx, ny, nz);
        }
    }
    if (!readCount(stream, count)) return false;
    mesh.triangles.resize(count);
    for (auto& triangle : mesh.triangles) {
        quint64 first = 0;
        quint64 second = 0;
        quint64 third = 0;
        quint64 faceId = 0;
        stream >> first >> second >> third >> faceId;
        triangle.vertices = {static_cast<std::size_t>(first), static_cast<std::size_t>(second), static_cast<std::size_t>(third)};
        triangle.faceId = static_cast<FaceId>(faceId);
    }
    if (!readCount(stream, count)) return false;
    mesh.boundarySamples.resize(count);
    for (auto& sample : mesh.boundarySamples) {
        quint64 edgeId = 0;
        quint64 vertexIndex = 0;
        stream >> edgeId >> sample.parameter >> vertexIndex >> sample.endpoint >> sample.hardCorner;
        sample.edgeId = static_cast<EdgeId>(edgeId);
        sample.vertexIndex = static_cast<std::size_t>(vertexIndex);
    }
    return stream.status() == QDataStream::Ok && !mesh.empty();
}

void writeCandidateReport(QDataStream& stream, const RegionLayoutCandidateReport& report) {
    stream << report.patchCount << report.topologyPassed << report.parameterizationPassed
           << report.coarseFitPassed << report.selectedForFullFit << report.proxyError
           << report.maximumDistortion << report.flippedTriangleCount
           << quint64(report.matrixRows) << quint64(report.matrixNonZeros)
           << quint64(report.estimatedSolverBytes) << report.wallMilliseconds;
    writeString(stream, report.layoutHash);
    writeString(stream, report.rejectionReason);
}

bool readCandidateReport(QDataStream& stream, RegionLayoutCandidateReport& report) {
    quint64 rows = 0;
    quint64 nonZeros = 0;
    quint64 bytes = 0;
    stream >> report.patchCount >> report.topologyPassed >> report.parameterizationPassed
           >> report.coarseFitPassed >> report.selectedForFullFit >> report.proxyError
           >> report.maximumDistortion >> report.flippedTriangleCount >> rows >> nonZeros
           >> bytes >> report.wallMilliseconds;
    if (!readString(stream, report.layoutHash) || !readString(stream, report.rejectionReason)) return false;
    report.matrixRows = static_cast<std::size_t>(rows);
    report.matrixNonZeros = static_cast<std::size_t>(nonZeros);
    report.estimatedSolverBytes = bytes;
    return stream.status() == QDataStream::Ok;
}

void writeLayout(QDataStream& stream, const RegionPatchLayout& layout) {
    stream << layout.patchCount << quint64(layout.trianglePatches.size());
    for (const auto patch : layout.trianglePatches) stream << patch;
    stream << quint64(layout.patches.size());
    for (const auto& patch : layout.patches) {
        stream << quint64(patch.triangleIndices.size());
        for (const auto index : patch.triangleIndices) stream << quint64(index);
        stream << quint64(patch.vertexIndices.size());
        for (const auto index : patch.vertexIndices) stream << quint64(index);
        stream << quint64(patch.parameters.size());
        for (const auto& uv : patch.parameters) stream << uv[0] << uv[1];
        stream << quint64(patch.boundaryLoop.size());
        for (const auto index : patch.boundaryLoop) stream << quint64(index);
        stream << quint64(patch.matrixRows) << quint64(patch.matrixNonZeros)
               << patch.flippedTriangleCount << patch.maximumDistortion;
    }
    stream << quint64(layout.seams.size());
    for (const auto& seam : layout.seams) {
        stream << seam.firstPatch << seam.secondPatch << quint64(seam.meshEdges.size());
        for (const auto& edge : seam.meshEdges) stream << quint64(edge[0]) << quint64(edge[1]);
    }
    stream << layout.proxyError << quint64(layout.estimatedSolverBytes);
    writeString(stream, layout.hash);
}

bool readLayout(QDataStream& stream, RegionPatchLayout& layout) {
    std::size_t count = 0;
    stream >> layout.patchCount;
    if (!readCount(stream, count)) return false;
    layout.trianglePatches.resize(count);
    for (auto& patch : layout.trianglePatches) stream >> patch;
    if (!readCount(stream, count)) return false;
    layout.patches.resize(count);
    for (auto& patch : layout.patches) {
        if (!readCount(stream, count)) return false;
        patch.triangleIndices.resize(count);
        for (auto& index : patch.triangleIndices) {
            quint64 value = 0;
            stream >> value;
            index = static_cast<std::size_t>(value);
        }
        if (!readCount(stream, count)) return false;
        patch.vertexIndices.resize(count);
        for (auto& index : patch.vertexIndices) {
            quint64 value = 0;
            stream >> value;
            index = static_cast<std::size_t>(value);
        }
        if (!readCount(stream, count)) return false;
        patch.parameters.resize(count);
        for (auto& uv : patch.parameters) stream >> uv[0] >> uv[1];
        if (!readCount(stream, count)) return false;
        patch.boundaryLoop.resize(count);
        for (auto& index : patch.boundaryLoop) {
            quint64 value = 0;
            stream >> value;
            index = static_cast<std::size_t>(value);
        }
        quint64 rows = 0;
        quint64 nonZeros = 0;
        stream >> rows >> nonZeros >> patch.flippedTriangleCount >> patch.maximumDistortion;
        patch.matrixRows = static_cast<std::size_t>(rows);
        patch.matrixNonZeros = static_cast<std::size_t>(nonZeros);
    }
    if (!readCount(stream, count)) return false;
    layout.seams.resize(count);
    for (auto& seam : layout.seams) {
        stream >> seam.firstPatch >> seam.secondPatch;
        if (!readCount(stream, count)) return false;
        seam.meshEdges.resize(count);
        for (auto& edge : seam.meshEdges) {
            quint64 first = 0;
            quint64 second = 0;
            stream >> first >> second;
            edge = {static_cast<std::size_t>(first), static_cast<std::size_t>(second)};
        }
    }
    quint64 bytes = 0;
    stream >> layout.proxyError >> bytes;
    layout.estimatedSolverBytes = bytes;
    return readString(stream, layout.hash) && stream.status() == QDataStream::Ok;
}

void writeIteration(QDataStream& stream, const RegionFitIterationReport& report) {
    stream << report.iteration << report.uControlCount << report.vControlCount
           << quint64(report.matrixRows) << quint64(report.matrixColumns)
           << quint64(report.matrixNonZeros) << quint64(report.estimatedSolverBytes)
           << report.symbolicFactorizationReuseCount << quint64(report.projectionCount)
           << quint64(report.uvIterationCount) << quint64(report.projectionFallbackCount)
           << report.trainingMaximumDistance
           << report.selectionMaximumDistance << report.holdoutMaximumDistance
           << report.holdoutMaximumNormalDegrees << report.holdout95Distance
           << report.holdout95NormalDegrees << quint64(report.holdoutWorstDistanceVertex)
           << quint64(report.holdoutWorstNormalVertex) << report.holdoutWorstDistancePatch
           << report.holdoutWorstNormalPatch << report.holdoutWorstDistanceInternalSeam
           << report.holdoutWorstNormalInternalSeam << report.boundaryMaximumDistance
           << report.seamMaximumDistance << report.wallMilliseconds;
    writeString(stream, report.stopReason);
}

bool readIteration(QDataStream& stream, RegionFitIterationReport& report) {
    quint64 rows = 0;
    quint64 columns = 0;
    quint64 nonZeros = 0;
    quint64 bytes = 0;
    quint64 worstDistance = 0;
    quint64 worstNormal = 0;
    quint64 projectionCount = 0;
    quint64 uvIterations = 0;
    quint64 projectionFallbacks = 0;
    stream >> report.iteration >> report.uControlCount >> report.vControlCount
           >> rows >> columns >> nonZeros >> bytes >> report.symbolicFactorizationReuseCount
           >> projectionCount >> uvIterations >> projectionFallbacks
           >> report.trainingMaximumDistance >> report.selectionMaximumDistance
           >> report.holdoutMaximumDistance >> report.holdoutMaximumNormalDegrees
           >> report.holdout95Distance >> report.holdout95NormalDegrees
           >> worstDistance >> worstNormal >> report.holdoutWorstDistancePatch
           >> report.holdoutWorstNormalPatch >> report.holdoutWorstDistanceInternalSeam
           >> report.holdoutWorstNormalInternalSeam >> report.boundaryMaximumDistance
           >> report.seamMaximumDistance >> report.wallMilliseconds;
    report.matrixRows = static_cast<std::size_t>(rows);
    report.matrixColumns = static_cast<std::size_t>(columns);
    report.matrixNonZeros = static_cast<std::size_t>(nonZeros);
    report.estimatedSolverBytes = bytes;
    report.projectionCount = static_cast<std::size_t>(projectionCount);
    report.uvIterationCount = static_cast<std::size_t>(uvIterations);
    report.projectionFallbackCount = static_cast<std::size_t>(projectionFallbacks);
    report.holdoutWorstDistanceVertex = static_cast<std::size_t>(worstDistance);
    report.holdoutWorstNormalVertex = static_cast<std::size_t>(worstNormal);
    return readString(stream, report.stopReason) && stream.status() == QDataStream::Ok;
}

void writeAttempts(QDataStream& stream, const std::vector<RegionFitAttemptReport>& attempts) {
    stream << quint64(attempts.size());
    for (const auto& attempt : attempts) {
        stream << attempt.patchCount;
        writeString(stream, attempt.layoutHash);
        stream << attempt.success << attempt.wallMilliseconds;
        writeString(stream, attempt.stopReason);
        stream << quint64(attempt.iterations.size());
        for (const auto& iteration : attempt.iterations) writeIteration(stream, iteration);
    }
}

bool readAttempts(QDataStream& stream, std::vector<RegionFitAttemptReport>& attempts) {
    std::size_t count = 0;
    if (!readCount(stream, count)) return false;
    attempts.resize(count);
    for (auto& attempt : attempts) {
        stream >> attempt.patchCount;
        if (!readString(stream, attempt.layoutHash)) return false;
        stream >> attempt.success >> attempt.wallMilliseconds;
        if (!readString(stream, attempt.stopReason) || !readCount(stream, count)) return false;
        attempt.iterations.resize(count);
        for (auto& iteration : attempt.iterations) {
            if (!readIteration(stream, iteration)) return false;
        }
    }
    return stream.status() == QDataStream::Ok;
}

bool saveDataFile(
    const std::filesystem::path& path,
    const std::function<void(QDataStream&)>& writer) {
    std::filesystem::create_directories(path.parent_path());
    QSaveFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    configure(stream);
    writer(stream);
    return stream.status() == QDataStream::Ok && file.commit();
}

bool openDataFile(
    const std::filesystem::path& path,
    const std::function<bool(QDataStream&)>& reader) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly)) return false;
    QDataStream stream(&file);
    configure(stream);
    return reader(stream) && stream.status() == QDataStream::Ok;
}

std::string makeKey(const RegionRefitRequest& request) {
    if (request.options.inputStepSha256.empty()) return {};
    auto faces = request.selectedFaces;
    auto edges = request.lockedEdges;
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::ostringstream text;
    text << std::setprecision(17)
         << "native-regional-refit-cache-v1|" << request.options.inputStepSha256 << '|'
         << (request.options.algorithmFingerprint.empty()
                ? "native-regional-refit-v15"
                : request.options.algorithmFingerprint)
         << '|' << static_cast<int>(request.layoutMode) << '|';
    for (const auto face : faces) text << face << ',';
    text << '|';
    for (const auto edge : edges) text << edge << ',';
    text << '|' << request.options.tolerance3d
         << '|' << request.options.normalToleranceDegrees
         << '|' << request.options.curveOnSurfaceTolerance
         << '|' << request.options.maximumSampleTriangles
         << '|' << request.options.fixedPatchCount
         << '|' << request.options.maximumAdaptivePatchCount
         << '|' << request.options.maximumFullFitCount
         << '|' << request.options.maximumFitIterations
         << '|' << request.options.singleFitBudgetMilliseconds
         << '|' << request.options.totalFitBudgetMilliseconds
         << '|' << request.options.solverMemoryPreflightMiB
         << '|' << request.options.maximumParameterDistortion;
    const auto digest = QCryptographicHash::hash(
        QByteArray::fromStdString(text.str()), QCryptographicHash::Sha256);
    return digest.toHex().toStdString();
}

RegionRefitCacheEvent disabledEvent(const char* layer) {
    RegionRefitCacheEvent event;
    event.layer = layer;
    event.message = "Cache is disabled or has no stable STEP hash.";
    return event;
}

}

RegionalSurfaceRefitCache::RegionalSurfaceRefitCache(const RegionRefitRequest& request)
    : policy_(request.options.cachePolicy), key_(makeKey(request)) {
    if (!key_.empty() && !request.options.cacheDirectory.empty()) {
        entryDirectory_ = request.options.cacheDirectory / key_;
    }
}

bool RegionalSurfaceRefitCache::enabled() const {
    return policy_ != RegionRefitCachePolicy::Disabled && !entryDirectory_.empty();
}

const std::string& RegionalSurfaceRefitCache::key() const {
    return key_;
}

bool RegionalSurfaceRefitCache::mayRead() const {
    return enabled() && (policy_ == RegionRefitCachePolicy::ReadWrite ||
        policy_ == RegionRefitCachePolicy::ReadOnly);
}

bool RegionalSurfaceRefitCache::mayWrite() const {
    return enabled() && (policy_ == RegionRefitCachePolicy::ReadWrite ||
        policy_ == RegionRefitCachePolicy::Refresh);
}

std::filesystem::path RegionalSurfaceRefitCache::cacheFile(const char* name) const {
    return entryDirectory_ / name;
}

RegionRefitCacheEvent RegionalSurfaceRefitCache::recordConstraintResult(const bool success) const {
    RegionRefitCacheEvent event;
    event.layer = "ConstraintCache";
    if (!enabled()) return disabledEvent("ConstraintCache");
    const auto started = std::chrono::steady_clock::now();
    if (mayRead()) {
        QFile file(QString::fromStdWString(cacheFile("constraints.ok").wstring()));
        event.hit = file.exists();
    }
    event.readMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (success && mayWrite()) {
        const auto writeStarted = std::chrono::steady_clock::now();
        std::filesystem::create_directories(entryDirectory_);
        QSaveFile file(QString::fromStdWString(cacheFile("constraints.ok").wstring()));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QByteArray::fromStdString(key_));
            file.commit();
        }
        event.writeMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - writeStarted).count();
    }
    event.message = event.hit
        ? "Constraint identity cache hit; document-owned topology was rehydrated from STEP."
        : "Constraint identity cache miss.";
    return event;
}

bool RegionalSurfaceRefitCache::loadSampling(
    StepAdaptiveRegionSamplingResult& result,
    RegionRefitCacheEvent& event) const {
    event.layer = "SamplingCache";
    if (!mayRead()) {
        event = disabledEvent("SamplingCache");
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto loaded = openDataFile(cacheFile("sampling.bin"), [&](QDataStream& stream) {
        quint32 magic = 0;
        quint32 version = 0;
        stream >> magic >> version;
        return magic == kSamplingMagic && version == kVersion &&
            readSamplingReport(stream, result.report) && readMesh(stream, result.mesh) &&
            result.report.meshFingerprint == result.mesh.deterministicFingerprint();
    });
    event.readMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    event.hit = loaded;
    event.message = loaded ? "Rich STEP sampling cache hit." : "Rich STEP sampling cache miss.";
    return loaded;
}

RegionRefitCacheEvent RegionalSurfaceRefitCache::saveSampling(
    const StepAdaptiveRegionSamplingResult& result) const {
    RegionRefitCacheEvent event;
    event.layer = "SamplingCache";
    if (!mayWrite()) return disabledEvent("SamplingCache");
    const auto started = std::chrono::steady_clock::now();
    const auto saved = saveDataFile(cacheFile("sampling.bin"), [&](QDataStream& stream) {
        stream << kSamplingMagic << kVersion;
        writeSamplingReport(stream, result.report);
        writeMesh(stream, result.mesh);
    });
    event.writeMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    event.message = saved ? "Stored rich STEP sampling cache." : "Failed to store rich STEP sampling cache.";
    return event;
}

bool RegionalSurfaceRefitCache::loadLayouts(
    RegionPatchLayoutGenerationResult& result,
    RegionRefitCacheEvent& event) const {
    event.layer = "LayoutCache";
    if (!mayRead()) {
        event = disabledEvent("LayoutCache");
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto loaded = openDataFile(cacheFile("layouts.bin"), [&](QDataStream& stream) {
        quint32 magic = 0;
        quint32 version = 0;
        qint32 status = 0;
        stream >> magic >> version >> status;
        if (magic != kLayoutMagic || version != kVersion || !readString(stream, result.message)) return false;
        result.status = static_cast<RegionRefitStatus>(status);
        std::size_t count = 0;
        if (!readCount(stream, count)) return false;
        result.candidates.resize(count);
        for (auto& candidate : result.candidates) {
            if (!readCandidateReport(stream, candidate)) return false;
        }
        if (!readCount(stream, count)) return false;
        result.selectedCandidates.resize(count);
        for (auto& layout : result.selectedCandidates) {
            if (!readLayout(stream, layout)) return false;
        }
        return result.success();
    });
    event.readMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    event.hit = loaded;
    event.message = loaded ? "Deterministic layout cache hit." : "Deterministic layout cache miss.";
    return loaded;
}

RegionRefitCacheEvent RegionalSurfaceRefitCache::saveLayouts(
    const RegionPatchLayoutGenerationResult& result) const {
    RegionRefitCacheEvent event;
    event.layer = "LayoutCache";
    if (!mayWrite()) return disabledEvent("LayoutCache");
    const auto started = std::chrono::steady_clock::now();
    const auto saved = saveDataFile(cacheFile("layouts.bin"), [&](QDataStream& stream) {
        stream << kLayoutMagic << kVersion << qint32(result.status);
        writeString(stream, result.message);
        stream << quint64(result.candidates.size());
        for (const auto& candidate : result.candidates) writeCandidateReport(stream, candidate);
        stream << quint64(result.selectedCandidates.size());
        for (const auto& layout : result.selectedCandidates) writeLayout(stream, layout);
    });
    event.writeMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    event.message = saved ? "Stored deterministic layout cache." : "Failed to store deterministic layout cache.";
    return event;
}

bool RegionalSurfaceRefitCache::loadFit(
    const std::vector<RegionPatchLayout>& candidates,
    JointBSplineNetworkFitResult& result,
    RegionRefitCacheEvent& event) const {
    event.layer = "FitCache";
    if (!mayRead()) {
        event = disabledEvent("FitCache");
        return false;
    }
    std::string layoutHash;
    bool hasNetwork = false;
    double holdoutDistance = 0.0;
    double holdoutNormal = 0.0;
    double boundaryDistance = 0.0;
    double seamDistance = 0.0;
    const auto started = std::chrono::steady_clock::now();
    const auto metadataLoaded = openDataFile(cacheFile("fit.bin"), [&](QDataStream& stream) {
        quint32 magic = 0;
        quint32 version = 0;
        qint32 status = 0;
        stream >> magic >> version >> status;
        if (magic != kFitMagic || version != kVersion || !readString(stream, result.message) ||
            !readAttempts(stream, result.attempts)) return false;
        stream >> hasNetwork;
        if (!readString(stream, layoutHash)) return false;
        stream >> holdoutDistance >> holdoutNormal >> boundaryDistance >> seamDistance;
        result.status = static_cast<RegionRefitStatus>(status);
        return stream.status() == QDataStream::Ok;
    });
    if (!metadataLoaded) {
        event.readMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        event.message = "Joint-fit cache miss.";
        return false;
    }
    if (!hasNetwork) {
        event.hit = true;
        event.negativeEntry = true;
        event.readMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        event.message = "Negative joint-fit cache hit.";
        return true;
    }
    const auto layout = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
        return candidate.hash == layoutHash;
    });
    if (layout == candidates.end()) {
        event.message = "Joint-fit cache layout is not present in the selected candidate family.";
        return false;
    }
    TopoDS_Shape shape;
    BRep_Builder builder;
    std::ifstream input(cacheFile("fit_surfaces.brep"), std::ios::binary);
    if (!input) return false;
    try {
        BRepTools::Read(shape, input, builder);
    } catch (...) {
        return false;
    }
    auto network = std::make_shared<RegionFittedPatchNetwork>();
    network->layout = *layout;
    network->inspectionShape = shape;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const auto surface = Handle(Geom_BSplineSurface)::DownCast(
            BRep_Tool::Surface(TopoDS::Face(explorer.Current())));
        if (surface.IsNull()) return false;
        network->surfaces.push_back(surface);
    }
    if (network->surfaces.size() != static_cast<std::size_t>(network->layout.patchCount)) return false;
    network->holdoutMaximumDistance = holdoutDistance;
    network->holdoutMaximumNormalDegrees = holdoutNormal;
    network->boundaryMaximumDistance = boundaryDistance;
    network->seamMaximumDistance = seamDistance;
    result.network = std::move(network);
    event.hit = true;
    event.readMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    event.message = "Positive joint-fit cache hit; final topology validation remains uncached.";
    return true;
}

RegionRefitCacheEvent RegionalSurfaceRefitCache::saveFit(
    const JointBSplineNetworkFitResult& result) const {
    RegionRefitCacheEvent event;
    event.layer = "FitCache";
    event.negativeEntry = !result.success();
    if (!mayWrite()) return disabledEvent("FitCache");
    const auto started = std::chrono::steady_clock::now();
    bool surfaceSaved = true;
    if (result.success()) {
        std::filesystem::create_directories(entryDirectory_);
        std::ofstream output(cacheFile("fit_surfaces.brep"), std::ios::binary | std::ios::trunc);
        if (!output) {
            surfaceSaved = false;
        } else {
            try {
                BRepTools::Write(
                    result.network->inspectionShape,
                    output,
                    Standard_False,
                    Standard_False,
                    TopTools_FormatVersion_CURRENT);
                surfaceSaved = output.good();
            } catch (...) {
                surfaceSaved = false;
            }
        }
    }
    const auto metadataSaved = surfaceSaved && saveDataFile(cacheFile("fit.bin"), [&](QDataStream& stream) {
        stream << kFitMagic << kVersion << qint32(result.status);
        writeString(stream, result.message);
        writeAttempts(stream, result.attempts);
        stream << result.success();
        writeString(stream, result.success() ? result.network->layout.hash : std::string {});
        stream << (result.success() ? result.network->holdoutMaximumDistance : 0.0)
               << (result.success() ? result.network->holdoutMaximumNormalDegrees : 0.0)
               << (result.success() ? result.network->boundaryMaximumDistance : 0.0)
               << (result.success() ? result.network->seamMaximumDistance : 0.0);
    });
    event.writeMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    event.message = metadataSaved
        ? (result.success() ? "Stored positive joint-fit cache." : "Stored negative joint-fit cache.")
        : "Failed to store joint-fit cache.";
    return event;
}

}
