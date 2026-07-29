#include "reduction/region_refit/RegionalSurfaceRefitCache.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom_BSplineCurve.hxx>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QSaveFile>
#include <QString>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>

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
constexpr quint32 kCurveNetworkMagic = 0x52534643u;
constexpr quint32 kLayoutMagic = 0x5253464cu;
constexpr quint32 kFitMagic = 0x52534646u;
constexpr quint32 kVersion = 14;
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
           << report.maximumBoundaryChordError << report.geometricLimitsPassed
           << report.edgeLengthP01 << report.edgeLengthP50 << report.edgeLengthP99
           << report.edgeLengthP99P01Ratio << report.minimumAngleP05Degrees
           << report.minimumAngleP50Degrees << report.aspectRatioP50
           << report.aspectRatioP95 << report.aspectRatioP99
           << report.normalizedQualityP05 << report.normalizedQualityP50
           << quint64(report.zeroAreaTriangleCount) << quint64(report.nonManifoldEdgeCount)
           << quint64(report.remeshSplitCount) << quint64(report.remeshCollapseCount)
           << quint64(report.remeshFlipCount) << quint64(report.remeshRelocationCount);
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
    quint64 zeroAreaTriangles = 0;
    quint64 nonManifoldEdges = 0;
    quint64 remeshSplits = 0;
    quint64 remeshCollapses = 0;
    quint64 remeshFlips = 0;
    quint64 remeshRelocations = 0;
    stream >> status;
    if (!readString(stream, report.message)) return false;
    stream >> sourceFaces >> vertices >> triangles >> boundarySamples >> endpoints
           >> internalSeams >> weldedSeams >> normalRefinements >> discarded >> discontinuities
           >> report.linearDeflection >> report.angularDeflectionDegrees
           >> report.boundaryChordTolerance >> report.maximumInteriorChordError
           >> report.maximumInteriorNormalErrorDegrees >> maximumNormalFace
           >> maximumNormalTriangle >> report.maximumBoundaryChordError
           >> report.geometricLimitsPassed >> report.edgeLengthP01 >> report.edgeLengthP50
           >> report.edgeLengthP99 >> report.edgeLengthP99P01Ratio
           >> report.minimumAngleP05Degrees >> report.minimumAngleP50Degrees
           >> report.aspectRatioP50 >> report.aspectRatioP95 >> report.aspectRatioP99
           >> report.normalizedQualityP05 >> report.normalizedQualityP50
           >> zeroAreaTriangles >> nonManifoldEdges >> remeshSplits >> remeshCollapses
           >> remeshFlips >> remeshRelocations;
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
    report.zeroAreaTriangleCount = static_cast<std::size_t>(zeroAreaTriangles);
    report.nonManifoldEdgeCount = static_cast<std::size_t>(nonManifoldEdges);
    report.remeshSplitCount = static_cast<std::size_t>(remeshSplits);
    report.remeshCollapseCount = static_cast<std::size_t>(remeshCollapses);
    report.remeshFlipCount = static_cast<std::size_t>(remeshFlips);
    report.remeshRelocationCount = static_cast<std::size_t>(remeshRelocations);
    return true;
}

void writeMesh(QDataStream& stream, const RegionReferenceMesh& mesh) {
    stream << quint64(mesh.vertices.size());
    for (const auto& vertex : mesh.vertices) {
        stream << vertex.point.X() << vertex.point.Y() << vertex.point.Z()
               << vertex.normal.X() << vertex.normal.Y() << vertex.normal.Z()
               << vertex.boundary << vertex.hardBoundary << vertex.boundaryEndpoint
               << vertex.hardCorner << vertex.internalSeam << vertex.normalDiscontinuity
               << vertex.areaWeight << vertex.boundaryLengthWeight;
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
               >> vertex.hardCorner >> vertex.internalSeam >> vertex.normalDiscontinuity
               >> vertex.areaWeight >> vertex.boundaryLengthWeight;
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
           << report.constructibleParameterization << report.coarseFitPassed
           << report.selectedForFullFit << report.proxyError
           << report.maximumDistortion << report.flippedTriangleCount
           << report.pcurveSelfIntersectionCount
           << report.pcurveNonAdjacentIntersectionCount
           << quint64(report.boundaryChainCount) << report.trimCornerCount
           << report.minimumSignedUvArea
           << report.minimumToMedianUvAreaRatio
           << report.distortionP995
           << report.boundaryRepresentabilityResidual
           << report.maximumSurfaceParameterCarrierBlend
           << quint64(report.logicalSideCandidateCount)
           << quint64(report.matrixRows) << quint64(report.matrixNonZeros)
           << quint64(report.estimatedSolverBytes)
           << quint64(report.estimatedOutputEntities) << report.wallMilliseconds;
    writeString(stream, report.layoutHash);
    writeString(stream, report.rejectionReason);
    writeString(stream, report.domainType);
    writeString(stream, report.selectedLogicalSideHash);
}

bool readCandidateReport(QDataStream& stream, RegionLayoutCandidateReport& report) {
    quint64 rows = 0;
    quint64 nonZeros = 0;
    quint64 bytes = 0;
    quint64 outputEntities = 0;
    quint64 boundaryChains = 0;
    quint64 logicalSideCandidates = 0;
    stream >> report.patchCount >> report.topologyPassed >> report.parameterizationPassed
           >> report.constructibleParameterization >> report.coarseFitPassed
           >> report.selectedForFullFit >> report.proxyError
           >> report.maximumDistortion >> report.flippedTriangleCount
           >> report.pcurveSelfIntersectionCount
           >> report.pcurveNonAdjacentIntersectionCount
           >> boundaryChains >> report.trimCornerCount >> report.minimumSignedUvArea
           >> report.minimumToMedianUvAreaRatio
           >> report.distortionP995
           >> report.boundaryRepresentabilityResidual
           >> report.maximumSurfaceParameterCarrierBlend
           >> logicalSideCandidates
           >> rows >> nonZeros
           >> bytes >> outputEntities >> report.wallMilliseconds;
    if (!readString(stream, report.layoutHash) ||
        !readString(stream, report.rejectionReason) ||
        !readString(stream, report.domainType) ||
        !readString(stream, report.selectedLogicalSideHash)) return false;
    report.boundaryChainCount = static_cast<std::size_t>(boundaryChains);
    report.logicalSideCandidateCount =
        static_cast<std::size_t>(logicalSideCandidates);
    report.matrixRows = static_cast<std::size_t>(rows);
    report.matrixNonZeros = static_cast<std::size_t>(nonZeros);
    report.estimatedSolverBytes = bytes;
    report.estimatedOutputEntities = outputEntities;
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
        stream << quint64(patch.surfaceParameters.size());
        for (const auto& uv : patch.surfaceParameters) {
            stream << uv[0] << uv[1];
        }
        stream << quint64(patch.boundaryLoop.size());
        for (const auto index : patch.boundaryLoop) stream << quint64(index);
        for (const auto& side : patch.boundarySides) {
            stream << quint64(side.size());
            for (const auto index : side) stream << quint64(index);
        }
        for (const auto& side : patch.logicalSides) {
            stream << qint32(side.isoSide)
                   << quint64(side.edgeUseOrders.size());
            for (const auto order : side.edgeUseOrders) {
                stream << quint64(order);
            }
            stream << side.curveLength;
        }
        stream << quint64(patch.edgePcurves.size());
        for (const auto& pcurve : patch.edgePcurves) {
            stream << quint64(pcurve.edgeIndex) << pcurve.forward
                   << pcurve.firstParameter << pcurve.lastParameter
                   << quint64(pcurve.startNode) << quint64(pcurve.endNode)
                   << quint64(pcurve.loopOrder)
                   << qint32(pcurve.isoSide)
                   << pcurve.sideFirstParameter
                   << pcurve.sideLastParameter
                   << quint64(pcurve.parameters.size());
            for (std::size_t index = 0;
                 index < pcurve.parameters.size(); ++index) {
                stream << pcurve.parameters[index]
                       << pcurve.uvPoints[index][0]
                       << pcurve.uvPoints[index][1];
            }
            writeString(stream, pcurve.hash);
        }
        stream << quint64(patch.boundaryChains.size());
        for (const auto& chain : patch.boundaryChains) {
            stream << chain.adjacentPatch << chain.originalBoundary
                   << quint64(chain.vertices.size());
            for (const auto vertex : chain.vertices) stream << quint64(vertex);
            stream << quint64(chain.originalEdgeIds.size());
            for (const auto edgeId : chain.originalEdgeIds) stream << quint64(edgeId);
        }
        stream << quint64(patch.trimCorners.size());
        for (const auto corner : patch.trimCorners) stream << quint64(corner);
        writeString(stream, patch.domainType);
        stream << patch.rectangularIsoBoundary
               << patch.diskTopologyPassed
               << patch.pcurveLoopClosed
               << patch.pcurveOrientationConsistent
               << patch.pcurveSelfIntersectionCount
               << patch.pcurveNonAdjacentIntersectionCount
               << quint64(patch.boundaryChainCount)
               << patch.trimCornerCount;
        stream << quint64(patch.matrixRows) << quint64(patch.matrixNonZeros)
               << patch.flippedTriangleCount << patch.minimumSignedUvArea
               << patch.minimumToMedianUvAreaRatio
               << patch.distortionP995
               << patch.boundaryRepresentabilityResidual
               << patch.surfaceParameterCarrierBlend
               << patch.maximumDistortion
               << quint64(patch.logicalSideCandidateCount);
        writeString(stream, patch.selectedLogicalSideHash);
        writeString(stream, patch.carrierParameterWeightMode);
    }
    stream << quint64(layout.seams.size());
    for (const auto& seam : layout.seams) {
        stream << seam.firstPatch << seam.secondPatch << quint64(seam.meshEdges.size());
        for (const auto& edge : seam.meshEdges) stream << quint64(edge[0]) << quint64(edge[1]);
    }
    stream << layout.proxyError << quint64(layout.estimatedSolverBytes)
           << quint64(layout.estimatedOutputEntities);
    writeString(stream, layout.hash);
    writeString(stream, layout.curveNetworkHash);
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
        patch.surfaceParameters.resize(count);
        for (auto& uv : patch.surfaceParameters) stream >> uv[0] >> uv[1];
        if (!readCount(stream, count)) return false;
        patch.boundaryLoop.resize(count);
        for (auto& index : patch.boundaryLoop) {
            quint64 value = 0;
            stream >> value;
            index = static_cast<std::size_t>(value);
        }
        for (auto& side : patch.boundarySides) {
            if (!readCount(stream, count)) return false;
            side.resize(count);
            for (auto& index : side) {
                quint64 value = 0;
                stream >> value;
                index = static_cast<std::size_t>(value);
            }
        }
        for (auto& side : patch.logicalSides) {
            qint32 isoSide = 0;
            stream >> isoSide;
            side.isoSide = static_cast<RegionPatchIsoSide>(isoSide);
            if (!readCount(stream, count)) return false;
            side.edgeUseOrders.resize(count);
            for (auto& order : side.edgeUseOrders) {
                quint64 value = 0;
                stream >> value;
                order = static_cast<std::size_t>(value);
            }
            stream >> side.curveLength;
        }
        if (!readCount(stream, count)) return false;
        patch.edgePcurves.resize(count);
        for (auto& pcurve : patch.edgePcurves) {
            quint64 edgeIndex = 0;
            quint64 startNode = 0;
            quint64 endNode = 0;
            quint64 loopOrder = 0;
            qint32 isoSide = 0;
            stream >> edgeIndex >> pcurve.forward
                   >> pcurve.firstParameter >> pcurve.lastParameter
                   >> startNode >> endNode >> loopOrder
                   >> isoSide
                   >> pcurve.sideFirstParameter
                   >> pcurve.sideLastParameter;
            pcurve.edgeIndex = static_cast<std::size_t>(edgeIndex);
            pcurve.startNode = static_cast<std::size_t>(startNode);
            pcurve.endNode = static_cast<std::size_t>(endNode);
            pcurve.loopOrder = static_cast<std::size_t>(loopOrder);
            pcurve.isoSide = static_cast<RegionPatchIsoSide>(isoSide);
            if (!readCount(stream, count)) return false;
            pcurve.parameters.resize(count);
            pcurve.uvPoints.resize(count);
            for (std::size_t index = 0; index < count; ++index) {
                stream >> pcurve.parameters[index]
                       >> pcurve.uvPoints[index][0]
                       >> pcurve.uvPoints[index][1];
            }
            if (!readString(stream, pcurve.hash)) return false;
        }
        if (!readCount(stream, count)) return false;
        patch.boundaryChains.resize(count);
        for (auto& chain : patch.boundaryChains) {
            stream >> chain.adjacentPatch >> chain.originalBoundary;
            if (!readCount(stream, count)) return false;
            chain.vertices.resize(count);
            for (auto& vertex : chain.vertices) {
                quint64 value = 0;
                stream >> value;
                vertex = static_cast<std::size_t>(value);
            }
            if (!readCount(stream, count)) return false;
            chain.originalEdgeIds.resize(count);
            for (auto& edgeId : chain.originalEdgeIds) {
                quint64 value = 0;
                stream >> value;
                edgeId = static_cast<EdgeId>(value);
            }
        }
        if (!readCount(stream, count)) return false;
        patch.trimCorners.resize(count);
        for (auto& corner : patch.trimCorners) {
            quint64 value = 0;
            stream >> value;
            corner = static_cast<std::size_t>(value);
        }
        if (!readString(stream, patch.domainType)) return false;
        quint64 boundaryChainCount = 0;
        stream >> patch.rectangularIsoBoundary
               >> patch.diskTopologyPassed
               >> patch.pcurveLoopClosed
               >> patch.pcurveOrientationConsistent
               >> patch.pcurveSelfIntersectionCount
               >> patch.pcurveNonAdjacentIntersectionCount
               >> boundaryChainCount >> patch.trimCornerCount;
        patch.boundaryChainCount = static_cast<std::size_t>(boundaryChainCount);
        quint64 rows = 0;
        quint64 nonZeros = 0;
        stream >> rows >> nonZeros >> patch.flippedTriangleCount
               >> patch.minimumSignedUvArea
               >> patch.minimumToMedianUvAreaRatio
               >> patch.distortionP995
               >> patch.boundaryRepresentabilityResidual
               >> patch.surfaceParameterCarrierBlend
               >> patch.maximumDistortion;
        quint64 logicalSideCandidateCount = 0;
        stream >> logicalSideCandidateCount;
        patch.logicalSideCandidateCount =
            static_cast<std::size_t>(logicalSideCandidateCount);
        if (!readString(stream, patch.selectedLogicalSideHash) ||
            !readString(stream, patch.carrierParameterWeightMode)) {
            return false;
        }
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
    quint64 outputEntities = 0;
    stream >> layout.proxyError >> bytes >> outputEntities;
    layout.estimatedSolverBytes = bytes;
    layout.estimatedOutputEntities = outputEntities;
    return readString(stream, layout.hash) &&
        readString(stream, layout.curveNetworkHash) &&
        stream.status() == QDataStream::Ok;
}

const RegionBoundaryEdgeConstraint* cachedConstraintFor(
    const RegionTopologyConstraints& constraints,
    const EdgeId edgeId) {
    const auto found = std::find_if(
        constraints.boundaryEdges.begin(),
        constraints.boundaryEdges.end(),
        [&](const auto& constraint) { return constraint.edgeId == edgeId; });
    return found == constraints.boundaryEdges.end() ? nullptr : &*found;
}

void writeCurveNetwork(
    QDataStream& stream,
    const RegionCurveNetwork& network) {
    stream << network.patchCount << quint64(network.trianglePatches.size());
    for (const auto patch : network.trianglePatches) stream << patch;
    stream << quint64(network.nodes.size());
    for (const auto& node : network.nodes) {
        stream << quint64(node.id) << quint64(node.meshVertex)
               << node.point.X() << node.point.Y() << node.point.Z()
               << node.originalBoundary << quint64(node.incidentEdges.size());
        for (const auto edge : node.incidentEdges) stream << quint64(edge);
    }
    stream << quint64(network.edges.size());
    for (const auto& edge : network.edges) {
        stream << quint64(edge.id) << quint64(edge.startNode)
               << quint64(edge.endNode) << edge.firstPatch << edge.secondPatch
               << qint32(edge.requestedContinuity)
               << qint32(edge.continuity)
               << qint32(edge.constructionMode)
               << edge.productQualityPassed
               << edge.validationSampleCount
               << edge.controlPointCount
               << edge.maximumSampleTurnDegrees
               << edge.bendingEnergy
               << edge.selfIntersectionCount
               << edge.originalBoundary
               << edge.hasOriginalEdge << quint64(edge.originalEdgeId)
               << edge.firstParameter << edge.lastParameter
               << edge.reusesOriginalEdge << edge.splitsOriginalEdge
               << edge.frozen << edge.maximumProjectionError
               << quint64(edge.meshVertices.size());
        for (const auto vertex : edge.meshVertices) stream << quint64(vertex);
        stream << quint64(edge.meshParameters.size());
        for (const auto parameter : edge.meshParameters) stream << parameter;
        const auto bspline =
            Handle(Geom_BSplineCurve)::DownCast(edge.curve);
        if (edge.originalBoundary || bspline.IsNull()) {
            stream << quint64(0);
            continue;
        }
        stream << quint64(bspline->NbPoles())
               << quint64(bspline->NbKnots())
               << qint32(bspline->Degree())
               << bspline->IsPeriodic();
        for (int pole = 1; pole <= bspline->NbPoles(); ++pole) {
            const auto point = bspline->Pole(pole);
            stream << point.X() << point.Y() << point.Z()
                   << bspline->Weight(pole);
        }
        for (int knot = 1; knot <= bspline->NbKnots(); ++knot) {
            stream << bspline->Knot(knot)
                   << qint32(bspline->Multiplicity(knot));
        }
    }
    stream << quint64(network.patchLoops.size());
    for (const auto& loop : network.patchLoops) {
        stream << loop.patch << quint64(loop.edges.size());
        for (const auto& use : loop.edges) {
            stream << quint64(use.edgeIndex) << use.forward;
        }
    }
    writeString(stream, network.partitionLayoutHash);
    writeString(stream, network.hash);
}

bool readCurveNetwork(
    QDataStream& stream,
    const RegionTopologyConstraints& constraints,
    RegionCurveNetwork& network) {
    std::size_t count = 0;
    stream >> network.patchCount;
    if (!readCount(stream, count)) return false;
    network.trianglePatches.resize(count);
    for (auto& patch : network.trianglePatches) stream >> patch;
    if (!readCount(stream, count)) return false;
    network.nodes.resize(count);
    for (auto& node : network.nodes) {
        quint64 id = 0;
        quint64 meshVertex = 0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        stream >> id >> meshVertex >> x >> y >> z >> node.originalBoundary;
        node.id = static_cast<std::size_t>(id);
        node.meshVertex = static_cast<std::size_t>(meshVertex);
        node.point = gp_Pnt(x, y, z);
        if (!readCount(stream, count)) return false;
        node.incidentEdges.resize(count);
        for (auto& edge : node.incidentEdges) {
            quint64 value = 0;
            stream >> value;
            edge = static_cast<std::size_t>(value);
        }
    }
    if (!readCount(stream, count)) return false;
    network.edges.resize(count);
    for (auto& edge : network.edges) {
        quint64 id = 0;
        quint64 startNode = 0;
        quint64 endNode = 0;
        qint32 requestedContinuity = 0;
        qint32 continuity = 0;
        qint32 constructionMode = 0;
        quint64 originalEdgeId = 0;
        stream >> id >> startNode >> endNode
               >> edge.firstPatch >> edge.secondPatch
               >> requestedContinuity >> continuity >> constructionMode
               >> edge.productQualityPassed
               >> edge.validationSampleCount
               >> edge.controlPointCount
               >> edge.maximumSampleTurnDegrees
               >> edge.bendingEnergy
               >> edge.selfIntersectionCount
               >> edge.originalBoundary >> edge.hasOriginalEdge
               >> originalEdgeId >> edge.firstParameter >> edge.lastParameter
               >> edge.reusesOriginalEdge >> edge.splitsOriginalEdge
               >> edge.frozen >> edge.maximumProjectionError;
        edge.id = static_cast<std::size_t>(id);
        edge.startNode = static_cast<std::size_t>(startNode);
        edge.endNode = static_cast<std::size_t>(endNode);
        edge.requestedContinuity =
            static_cast<RegionCurveContinuity>(requestedContinuity);
        edge.continuity = static_cast<RegionCurveContinuity>(continuity);
        edge.constructionMode =
            static_cast<RegionInternalCurveConstructionMode>(
                constructionMode);
        edge.originalEdgeId = static_cast<EdgeId>(originalEdgeId);
        if (!readCount(stream, count)) return false;
        edge.meshVertices.resize(count);
        for (auto& vertex : edge.meshVertices) {
            quint64 value = 0;
            stream >> value;
            vertex = static_cast<std::size_t>(value);
        }
        if (!readCount(stream, count)) return false;
        edge.meshParameters.resize(count);
        for (auto& parameter : edge.meshParameters) stream >> parameter;
        std::size_t poleCount = 0;
        if (!readCount(stream, poleCount)) return false;
        if (edge.originalBoundary) {
            if (poleCount != 0) return false;
            const auto* constraint = cachedConstraintFor(
                constraints, edge.originalEdgeId);
            if (constraint == nullptr || constraint->originalEdge.IsNull()) {
                return false;
            }
            edge.originalEdge = constraint->originalEdge;
            double first = 0.0;
            double last = 0.0;
            edge.curve = BRep_Tool::Curve(
                edge.originalEdge, first, last);
        } else {
            std::size_t knotCount = 0;
            qint32 degree = 0;
            bool periodic = false;
            if (poleCount < 2 || !readCount(stream, knotCount) ||
                knotCount < 2) {
                return false;
            }
            stream >> degree >> periodic;
            if (degree < 1 ||
                poleCount > static_cast<std::size_t>(
                    std::numeric_limits<int>::max()) ||
                knotCount > static_cast<std::size_t>(
                    std::numeric_limits<int>::max())) {
                return false;
            }
            TColgp_Array1OfPnt poles(
                1, static_cast<int>(poleCount));
            TColStd_Array1OfReal weights(
                1, static_cast<int>(poleCount));
            for (std::size_t index = 0; index < poleCount; ++index) {
                double x = 0.0;
                double y = 0.0;
                double z = 0.0;
                double weight = 1.0;
                stream >> x >> y >> z >> weight;
                poles.SetValue(
                    static_cast<int>(index + 1), gp_Pnt(x, y, z));
                weights.SetValue(
                    static_cast<int>(index + 1), weight);
            }
            TColStd_Array1OfReal knots(
                1, static_cast<int>(knotCount));
            TColStd_Array1OfInteger multiplicities(
                1, static_cast<int>(knotCount));
            for (std::size_t index = 0; index < knotCount; ++index) {
                double knot = 0.0;
                qint32 multiplicity = 0;
                stream >> knot >> multiplicity;
                knots.SetValue(
                    static_cast<int>(index + 1), knot);
                multiplicities.SetValue(
                    static_cast<int>(index + 1), multiplicity);
            }
            try {
                edge.curve = new Geom_BSplineCurve(
                    poles,
                    weights,
                    knots,
                    multiplicities,
                    degree,
                    periodic,
                    Standard_True);
            } catch (...) {
                return false;
            }
        }
        if (edge.curve.IsNull()) return false;
    }
    if (!readCount(stream, count)) return false;
    network.patchLoops.resize(count);
    for (auto& loop : network.patchLoops) {
        stream >> loop.patch;
        if (!readCount(stream, count)) return false;
        loop.edges.resize(count);
        for (auto& use : loop.edges) {
            quint64 edgeIndex = 0;
            stream >> edgeIndex >> use.forward;
            use.edgeIndex = static_cast<std::size_t>(edgeIndex);
        }
    }
    std::string storedHash;
    if (!readString(stream, network.partitionLayoutHash) ||
        !readString(stream, storedHash)) {
        return false;
    }
    std::string message;
    if (!validateRegionCurveNetwork(
            network,
            std::numeric_limits<double>::infinity(),
            97,
            message)) {
        return false;
    }
    return network.hash == storedHash &&
        stream.status() == QDataStream::Ok;
}

void writeCurveNetworkCandidate(
    QDataStream& stream,
    const RegionCurveNetworkCandidate& candidate) {
    writeCurveNetwork(stream, candidate.network);
    writeLayout(stream, candidate.partitionLayout);
}

bool readCurveNetworkCandidate(
    QDataStream& stream,
    const RegionTopologyConstraints& constraints,
    RegionCurveNetworkCandidate& candidate) {
    return readCurveNetwork(stream, constraints, candidate.network) &&
        readLayout(stream, candidate.partitionLayout);
}

void writeIteration(QDataStream& stream, const RegionFitIterationReport& report) {
    stream << report.iteration << report.uControlCount << report.vControlCount
           << quint64(report.matrixRows) << quint64(report.matrixColumns)
           << quint64(report.matrixNonZeros) << quint64(report.estimatedSolverBytes)
           << report.symbolicFactorizationReuseCount << quint64(report.projectionCount)
           << quint64(report.uvIterationCount) << quint64(report.projectionFallbackCount)
           << quint64(report.boundaryHardConstraintCount)
           << quint64(report.seamG0ConstraintCount)
           << quint64(report.seamG1SoftConstraintCount)
           << quint64(report.seamG2SoftConstraintCount)
           << quint64(report.adjacentNormalSoftConstraintCount)
           << quint64(report.constraintRowsBefore)
           << quint64(report.constraintRowsAfter)
           << quint64(report.constraintEstimatedRank)
           << quint64(report.nonZeroBoundaryRhsCount);
    for (const auto value : report.boundaryTargetMinimum) stream << value;
    for (const auto value : report.boundaryTargetMaximum) stream << value;
    stream << report.maximumHardConstraintResidual
           << report.minimumFullHardConstraintResidual
           << report.boundaryFeasibilityRefinementRounds
           << report.boundaryFeasibilityResidual
           << report.hardConstraintFeasible
           << report.initialControlPriorScale
           << report.constraintCorrectionAnchorScale
           << report.affineBaselineTrainingMaximumDistance
           << report.affineBaselineBoundaryMaximumDistance
           << report.initialSupportBlendAlpha
           << report.constraintCorrectionBlendAlpha
           << report.initialTrainingMaximumDistance
           << report.initialBoundaryMaximumDistance
           << report.correctedTrainingMaximumDistance
           << report.correctedBoundaryMaximumDistance
           << report.trainingMaximumDistance
           << report.selectionMaximumDistance << report.holdoutMaximumDistance
           << report.holdoutMaximumNormalDegrees << report.holdout95Distance
           << report.holdout95NormalDegrees << quint64(report.holdoutWorstDistanceVertex)
           << quint64(report.holdoutWorstNormalVertex) << report.holdoutWorstDistancePatch
           << report.holdoutWorstNormalPatch << report.holdoutWorstDistanceInternalSeam
           << report.holdoutWorstNormalInternalSeam << report.boundaryMaximumDistance
           << report.seamMaximumDistance
           << report.seamMaximumCurveDeviation
           << report.seamMaximumNormalDegrees
           << report.seamMaximumCurvatureDifference
           << report.seamContinuityGatePassed
           << quint64(report.sameFaceBarycentricMappingCount)
           << quint64(report.sameFaceMappingFallbackCount)
           << quint64(report.crossFaceMappingFailureCount)
           << report.actualTotalPoleCount
           << report.actualMaximumPoleCountPerSide
           << report.actualTotalKnotCount
           << report.fixedBoundaryControlPointCount
           << report.interiorControlPointCount
           << report.reducedSpdSystemUsed
           << report.fairnessEnergy
           << report.controlNetSecondDifferenceEnergy
           << report.physicalSurfaceArea
           << report.areaWeightedNormalGradientEnergy
           << report.areaWeightedMeanCurvatureVariation
           << report.maximumNormalVariationDegrees
           << report.curvatureOscillation
           << report.minimumNormalizedJacobian
           << report.conditionNumberP50
           << report.conditionNumberP95
           << report.conditionNumberP99
           << report.surfaceQualityHardFailure
           << report.surfaceUvFlippedTriangleCount
           << report.surfaceUvDegenerateTriangleCount
           << report.surfaceJacobianDegenerateSampleCount
           << report.surfaceJacobianOrientationFlipCount
           << report.wallMilliseconds;
    writeString(stream, report.hardConstraintStatus);
    writeString(stream, report.surfaceLegalityStatus);
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
    quint64 boundaryConstraints = 0;
    quint64 seamG0 = 0;
    quint64 seamG1 = 0;
    quint64 seamG2 = 0;
    quint64 adjacentNormal = 0;
    quint64 constraintRowsBefore = 0;
    quint64 constraintRowsAfter = 0;
    quint64 constraintEstimatedRank = 0;
    quint64 nonZeroBoundaryRhs = 0;
    quint64 sameFaceBarycentricMappings = 0;
    quint64 sameFaceMappingFallbacks = 0;
    quint64 crossFaceMappingFailures = 0;
    stream >> report.iteration >> report.uControlCount >> report.vControlCount
           >> rows >> columns >> nonZeros >> bytes >> report.symbolicFactorizationReuseCount
           >> projectionCount >> uvIterations >> projectionFallbacks
           >> boundaryConstraints >> seamG0 >> seamG1 >> seamG2 >> adjacentNormal
           >> constraintRowsBefore >> constraintRowsAfter >> constraintEstimatedRank
           >> nonZeroBoundaryRhs;
    for (auto& value : report.boundaryTargetMinimum) stream >> value;
    for (auto& value : report.boundaryTargetMaximum) stream >> value;
    stream >> report.maximumHardConstraintResidual
           >> report.minimumFullHardConstraintResidual
           >> report.boundaryFeasibilityRefinementRounds
           >> report.boundaryFeasibilityResidual
           >> report.hardConstraintFeasible
           >> report.initialControlPriorScale
           >> report.constraintCorrectionAnchorScale
           >> report.affineBaselineTrainingMaximumDistance
           >> report.affineBaselineBoundaryMaximumDistance
           >> report.initialSupportBlendAlpha
           >> report.constraintCorrectionBlendAlpha
           >> report.initialTrainingMaximumDistance
           >> report.initialBoundaryMaximumDistance
           >> report.correctedTrainingMaximumDistance
           >> report.correctedBoundaryMaximumDistance
           >> report.trainingMaximumDistance >> report.selectionMaximumDistance
           >> report.holdoutMaximumDistance >> report.holdoutMaximumNormalDegrees
           >> report.holdout95Distance >> report.holdout95NormalDegrees
           >> worstDistance >> worstNormal >> report.holdoutWorstDistancePatch
           >> report.holdoutWorstNormalPatch >> report.holdoutWorstDistanceInternalSeam
           >> report.holdoutWorstNormalInternalSeam >> report.boundaryMaximumDistance
           >> report.seamMaximumDistance
           >> report.seamMaximumCurveDeviation
           >> report.seamMaximumNormalDegrees
           >> report.seamMaximumCurvatureDifference
           >> report.seamContinuityGatePassed
           >> sameFaceBarycentricMappings
           >> sameFaceMappingFallbacks
           >> crossFaceMappingFailures
           >> report.actualTotalPoleCount
           >> report.actualMaximumPoleCountPerSide
           >> report.actualTotalKnotCount
           >> report.fixedBoundaryControlPointCount
           >> report.interiorControlPointCount
           >> report.reducedSpdSystemUsed
           >> report.fairnessEnergy
           >> report.controlNetSecondDifferenceEnergy
           >> report.physicalSurfaceArea
           >> report.areaWeightedNormalGradientEnergy
           >> report.areaWeightedMeanCurvatureVariation
           >> report.maximumNormalVariationDegrees
           >> report.curvatureOscillation
           >> report.minimumNormalizedJacobian
           >> report.conditionNumberP50
           >> report.conditionNumberP95
           >> report.conditionNumberP99
           >> report.surfaceQualityHardFailure
           >> report.surfaceUvFlippedTriangleCount
           >> report.surfaceUvDegenerateTriangleCount
           >> report.surfaceJacobianDegenerateSampleCount
           >> report.surfaceJacobianOrientationFlipCount
           >> report.wallMilliseconds;
    report.matrixRows = static_cast<std::size_t>(rows);
    report.matrixColumns = static_cast<std::size_t>(columns);
    report.matrixNonZeros = static_cast<std::size_t>(nonZeros);
    report.estimatedSolverBytes = bytes;
    report.projectionCount = static_cast<std::size_t>(projectionCount);
    report.uvIterationCount = static_cast<std::size_t>(uvIterations);
    report.projectionFallbackCount = static_cast<std::size_t>(projectionFallbacks);
    report.boundaryHardConstraintCount = static_cast<std::size_t>(boundaryConstraints);
    report.seamG0ConstraintCount = static_cast<std::size_t>(seamG0);
    report.seamG1SoftConstraintCount = static_cast<std::size_t>(seamG1);
    report.seamG2SoftConstraintCount = static_cast<std::size_t>(seamG2);
    report.adjacentNormalSoftConstraintCount = static_cast<std::size_t>(adjacentNormal);
    report.constraintRowsBefore = static_cast<std::size_t>(constraintRowsBefore);
    report.constraintRowsAfter = static_cast<std::size_t>(constraintRowsAfter);
    report.constraintEstimatedRank = static_cast<std::size_t>(constraintEstimatedRank);
    report.nonZeroBoundaryRhsCount = static_cast<std::size_t>(nonZeroBoundaryRhs);
    report.sameFaceBarycentricMappingCount =
        static_cast<std::size_t>(sameFaceBarycentricMappings);
    report.sameFaceMappingFallbackCount =
        static_cast<std::size_t>(sameFaceMappingFallbacks);
    report.crossFaceMappingFailureCount =
        static_cast<std::size_t>(crossFaceMappingFailures);
    report.holdoutWorstDistanceVertex = static_cast<std::size_t>(worstDistance);
    report.holdoutWorstNormalVertex = static_cast<std::size_t>(worstNormal);
    return readString(stream, report.hardConstraintStatus) &&
        readString(stream, report.surfaceLegalityStatus) &&
        readString(stream, report.stopReason) &&
        stream.status() == QDataStream::Ok;
}

void writeAttempts(QDataStream& stream, const std::vector<RegionFitAttemptReport>& attempts) {
    stream << quint64(attempts.size());
    for (const auto& attempt : attempts) {
        stream << attempt.patchCount;
        writeString(stream, attempt.layoutHash);
        stream << quint64(attempt.sameFaceBarycentricMappingCount)
               << quint64(attempt.sameFaceMappingFallbackCount)
               << quint64(attempt.crossFaceMappingFailureCount);
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
        quint64 sameFaceBarycentricMappings = 0;
        quint64 sameFaceMappingFallbacks = 0;
        quint64 crossFaceMappingFailures = 0;
        stream >> sameFaceBarycentricMappings
               >> sameFaceMappingFallbacks
               >> crossFaceMappingFailures;
        attempt.sameFaceBarycentricMappingCount =
            static_cast<std::size_t>(sameFaceBarycentricMappings);
        attempt.sameFaceMappingFallbackCount =
            static_cast<std::size_t>(sameFaceMappingFallbacks);
        attempt.crossFaceMappingFailureCount =
            static_cast<std::size_t>(crossFaceMappingFailures);
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
         << "native-regional-refit-cache-v2|" << request.options.inputStepSha256 << '|'
         << (request.options.algorithmFingerprint.empty()
                ? "native-regional-refit-v43-parameter-quality-adaptive-support"
                : request.options.algorithmFingerprint)
         << '|' << static_cast<int>(request.layoutMode)
         << '|' << static_cast<int>(request.options.fittingMode)
         << '|' << request.forceDiagnosticSewing << '|';
    for (const auto face : faces) text << face << ',';
    text << '|';
    for (const auto edge : edges) text << edge << ',';
    text << '|' << request.options.tolerance3d
         << '|' << request.options.normalToleranceDegrees
         << '|' << request.options.curveOnSurfaceTolerance
         << '|' << request.options.curveProjectionTolerance
         << '|' << request.options.curveValidationSampleCount
         << '|' << request.options.curveFairingIterations
         << '|' << request.options.curveFairingWeight
         << '|' << request.options.maximumInternalCurveSampleTurnDegrees
         << '|' << request.options.g1ToleranceDegrees
         << '|' << request.options.g2CurvatureTolerance
         << '|' << request.options.enableInternalSeamG2
         << '|' << request.options.seamG1RelativeWeight
         << '|' << request.options.seamG2RelativeWeight
         << '|' << request.options.maximumSampleTriangles
         << '|' << static_cast<int>(request.options.samplingMode)
         << '|' << static_cast<int>(request.options.layoutCarrierSamplingMode)
         << '|' << request.options.targetSampleTriangles
         << '|' << request.options.targetLayoutCarrierTriangles
         << '|' << request.options.maximumRemeshSweeps
         << '|' << request.options.maximumLocalSizeRatio
         << '|' << request.options.fixedPatchCount
         << '|' << request.options.requestedPatchCount
         << '|' << request.options.patchCountSearchRadius
         << '|' << static_cast<int>(request.options.adaptiveObjective)
         << '|' << request.options.maximumAdaptivePatchCount
         << '|' << request.options.maximumFullFitCount
         << '|' << request.options.maximumFitIterations
         << '|' << request.options.singleFitBudgetMilliseconds
         << '|' << request.options.totalFitBudgetMilliseconds
         << '|' << request.options.solverMemoryPreflightMiB
         << '|' << request.options.maximumParameterDistortion
         << '|' << request.options.maximumControlPointsPerSide
         << '|' << request.options.initialControlPriorRelativeWeight
         << '|' << request.options.constraintCorrectionAnchorRelativeWeight;
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

bool RegionalSurfaceRefitCache::loadCurveNetworks(
    const RegionTopologyConstraints& constraints,
    RegionCurveNetworkGenerationResult& result,
    RegionRefitCacheEvent& event) const {
    event.layer = "CurveNetworkCache";
    if (!mayRead()) {
        event = disabledEvent("CurveNetworkCache");
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto loaded = openDataFile(
        cacheFile("curve_networks.bin"),
        [&](QDataStream& stream) {
            quint32 magic = 0;
            quint32 version = 0;
            qint32 status = 0;
            stream >> magic >> version >> status;
            if (magic != kCurveNetworkMagic || version != kVersion ||
                !readString(stream, result.message)) {
                return false;
            }
            result.status = static_cast<RegionRefitStatus>(status);
            std::size_t count = 0;
            if (!readCount(stream, count)) return false;
            result.layoutReports.resize(count);
            for (auto& report : result.layoutReports) {
                if (!readCandidateReport(stream, report)) return false;
            }
            if (!readCount(stream, count)) return false;
            result.constructibleCandidates.resize(count);
            for (auto& candidate : result.constructibleCandidates) {
                if (!readCurveNetworkCandidate(
                        stream, constraints, candidate)) {
                    return false;
                }
            }
            if (!readCount(stream, count)) return false;
            result.selectedCandidates.resize(count);
            for (auto& candidate : result.selectedCandidates) {
                if (!readCurveNetworkCandidate(
                        stream, constraints, candidate)) {
                    return false;
                }
            }
            return result.success();
        });
    event.readMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    event.hit = loaded;
    event.message = loaded
        ? "Frozen curve-network cache hit."
        : "Frozen curve-network cache miss or incompatible binary version.";
    return loaded;
}

RegionRefitCacheEvent RegionalSurfaceRefitCache::saveCurveNetworks(
    const RegionCurveNetworkGenerationResult& result) const {
    RegionRefitCacheEvent event;
    event.layer = "CurveNetworkCache";
    if (!mayWrite()) return disabledEvent("CurveNetworkCache");
    const auto started = std::chrono::steady_clock::now();
    const auto saved = saveDataFile(
        cacheFile("curve_networks.bin"),
        [&](QDataStream& stream) {
            stream << kCurveNetworkMagic << kVersion
                   << qint32(result.status);
            writeString(stream, result.message);
            stream << quint64(result.layoutReports.size());
            for (const auto& report : result.layoutReports) {
                writeCandidateReport(stream, report);
            }
            stream << quint64(result.constructibleCandidates.size());
            for (const auto& candidate : result.constructibleCandidates) {
                writeCurveNetworkCandidate(stream, candidate);
            }
            stream << quint64(result.selectedCandidates.size());
            for (const auto& candidate : result.selectedCandidates) {
                writeCurveNetworkCandidate(stream, candidate);
            }
        });
    event.writeMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    event.message = saved
        ? "Stored frozen curve-network cache."
        : "Failed to store frozen curve-network cache.";
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
        result.constructibleCandidates.resize(count);
        for (auto& layout : result.constructibleCandidates) {
            if (!readLayout(stream, layout)) return false;
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
        stream << quint64(result.constructibleCandidates.size());
        for (const auto& layout : result.constructibleCandidates) writeLayout(stream, layout);
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
    double seamCurveDeviation = 0.0;
    double seamNormalDegrees = 0.0;
    double seamCurvatureDifference = 0.0;
    bool seamContinuityGatePassed = false;
    bool boundaryLocked = false;
    bool constructibleForBRep = false;
    double normalizedViolationScore = 0.0;
    RegionSurfaceQualityMetrics quality;
    std::string fitOutcome;
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
        stream >> holdoutDistance >> holdoutNormal >> boundaryDistance
               >> seamDistance >> seamCurveDeviation >> seamNormalDegrees
               >> seamCurvatureDifference >> seamContinuityGatePassed
               >> boundaryLocked >> constructibleForBRep
               >> normalizedViolationScore
               >> quality.totalPoleCount
               >> quality.maximumPoleCountPerSide
               >> quality.totalKnotCount
               >> quality.fairnessEnergy
               >> quality.controlNetSecondDifferenceEnergy
               >> quality.physicalSurfaceArea
               >> quality.areaWeightedNormalGradientEnergy
               >> quality.areaWeightedMeanCurvatureVariation
               >> quality.maximumNormalVariationDegrees
               >> quality.curvatureOscillation
               >> quality.minimumNormalizedJacobian
               >> quality.conditionNumberP50
               >> quality.conditionNumberP95
               >> quality.conditionNumberP99
               >> quality.hardFailure
               >> quality.legality.finiteControlNet
               >> quality.legality.positiveWeights
               >> quality.legality.validDegreesAndPoles
               >> quality.legality.strictlyIncreasingKnots
               >> quality.legality.uvFlippedTriangleCount
               >> quality.legality.uvDegenerateTriangleCount
               >> quality.legality.jacobianDegenerateSampleCount
               >> quality.legality.jacobianOrientationFlipCount
               >> quality.legality.minimumNormalizedJacobian
               >> quality.legality.passed;
        if (!readString(stream, fitOutcome)) return false;
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
    network->seamMaximumCurveDeviation = seamCurveDeviation;
    network->seamMaximumNormalDegrees = seamNormalDegrees;
    network->seamMaximumCurvatureDifference = seamCurvatureDifference;
    network->seamContinuityGatePassed = seamContinuityGatePassed;
    network->boundaryLocked = boundaryLocked;
    network->constructibleForBRep = constructibleForBRep;
    network->normalizedViolationScore = normalizedViolationScore;
    network->quality = quality;
    network->quality.surfaceComplexities.clear();
    for (std::size_t patch = 0;
         patch < network->surfaces.size();
         ++patch) {
        const auto& surface = network->surfaces[patch];
        network->quality.surfaceComplexities.push_back({
            static_cast<int>(patch),
            surface->NbUPoles(),
            surface->NbVPoles(),
            surface->NbUKnots(),
            surface->NbVKnots()});
    }
    network->fitOutcome = fitOutcome;
    network->fitGatePassed = true;
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
               << (result.success() ? result.network->seamMaximumDistance : 0.0)
               << (result.success() ? result.network->seamMaximumCurveDeviation : 0.0)
               << (result.success() ? result.network->seamMaximumNormalDegrees : 0.0)
               << (result.success() ? result.network->seamMaximumCurvatureDifference : 0.0)
               << (result.success() && result.network->seamContinuityGatePassed)
               << (result.success() && result.network->boundaryLocked)
               << (result.success() && result.network->constructibleForBRep)
               << (result.success()
                    ? result.network->normalizedViolationScore : 0.0)
               << (result.success()
                    ? result.network->quality.totalPoleCount : 0)
               << (result.success()
                    ? result.network->quality.maximumPoleCountPerSide : 0)
               << (result.success()
                    ? result.network->quality.totalKnotCount : 0)
               << (result.success()
                    ? result.network->quality.fairnessEnergy : 0.0)
               << (result.success()
                    ? result.network->quality
                        .controlNetSecondDifferenceEnergy : 0.0)
               << (result.success()
                    ? result.network->quality.physicalSurfaceArea : 0.0)
               << (result.success()
                    ? result.network->quality
                        .areaWeightedNormalGradientEnergy : 0.0)
               << (result.success()
                    ? result.network->quality
                        .areaWeightedMeanCurvatureVariation : 0.0)
               << (result.success()
                    ? result.network->quality.maximumNormalVariationDegrees
                    : 0.0)
               << (result.success()
                    ? result.network->quality.curvatureOscillation : 0.0)
               << (result.success()
                    ? result.network->quality.minimumNormalizedJacobian : 0.0)
               << (result.success()
                    ? result.network->quality.conditionNumberP50 : 0.0)
               << (result.success()
                    ? result.network->quality.conditionNumberP95 : 0.0)
               << (result.success()
                    ? result.network->quality.conditionNumberP99 : 0.0)
               << (result.success() &&
                    result.network->quality.hardFailure)
               << (!result.success() ||
                    result.network->quality.legality.finiteControlNet)
               << (!result.success() ||
                    result.network->quality.legality.positiveWeights)
               << (!result.success() ||
                    result.network->quality.legality.validDegreesAndPoles)
               << (!result.success() ||
                    result.network->quality.legality
                        .strictlyIncreasingKnots)
               << (result.success()
                    ? result.network->quality.legality
                        .uvFlippedTriangleCount : 0)
               << (result.success()
                    ? result.network->quality.legality
                        .uvDegenerateTriangleCount : 0)
               << (result.success()
                    ? result.network->quality.legality
                        .jacobianDegenerateSampleCount : 0)
               << (result.success()
                    ? result.network->quality.legality
                        .jacobianOrientationFlipCount : 0)
               << (result.success()
                    ? result.network->quality.legality
                        .minimumNormalizedJacobian : 0.0)
               << (result.success() &&
                    result.network->quality.legality.passed);
        writeString(
            stream,
            result.success() ? result.network->fitOutcome : std::string {});
    });
    event.writeMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    event.message = metadataSaved
        ? (result.success() ? "Stored positive joint-fit cache." : "Stored negative joint-fit cache.")
        : "Failed to store joint-fit cache.";
    return event;
}

}
