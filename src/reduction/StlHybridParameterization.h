#pragma once

#include "brep/ShapeDocument.h"
#include "common/GeometryTypes.h"
#include "stl/StlMesh.h"

#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace spo {

enum class SurfaceParameterizationSource {
    None,
    StepHarmonic,
    SupportSurfaceProjection
};

const char* toString(SurfaceParameterizationSource source);

enum class ScanReferenceCoverage {
    None,
    WholeModel,
    SelectionCrop
};

const char* toString(ScanReferenceCoverage coverage);

struct StlHybridProjection {
    bool valid = false;
    double u = 0.0;
    double v = 0.0;
    gp_Pnt referencePoint;
    gp_Vec referenceNormal;
    double distance = 0.0;
};

struct StlHybridParameterizationReport {
    bool success = false;
    SurfaceParameterizationSource source = SurfaceParameterizationSource::None;
    std::string message;
    int sourceTriangleCount = 0;
    int coarseTriangleCount = 0;
    int croppedTriangleCount = 0;
    int vertexCount = 0;
    int boundaryVertexCount = 0;
    int flippedTriangleCount = 0;
    double minimumNormalizedJacobian = 0.0;
};

class StlHybridParameterization {
public:
    static std::shared_ptr<const StlHybridParameterization> buildFromStepSelection(
        const ShapeDocument& document,
        const std::vector<FaceId>& selectedFaces,
        double tolerance3d,
        StlHybridParameterizationReport& report);

    static std::shared_ptr<const StlHybridParameterization> buildFromPatchMesh(
        const StlMesh& patchMesh,
        StlHybridParameterizationReport& report);

    StlHybridProjection project(const gp_Pnt& point) const;
    bool contains(double u, double v, double tolerance = 1.0e-10) const;
    StlHybridProjection projectBruteForceForTesting(const gp_Pnt& point) const;
    bool containsBruteForceForTesting(double u, double v, double tolerance = 1.0e-10) const;

private:
    struct Triangle;
    struct Bounds3d;
    struct BvhNode;

    void buildSpatialIndices();
    std::size_t buildBvhNode(std::size_t begin, std::size_t end);

    std::vector<gp_Pnt> vertices_;
    std::vector<std::array<double, 2>> parameters_;
    std::vector<Triangle> triangles_;
    std::vector<Bounds3d> triangleBounds_;
    std::vector<std::size_t> bvhOrder_;
    std::vector<BvhNode> bvhNodes_;
    int uvGridResolution_ = 64;
    std::vector<std::vector<std::size_t>> uvGrid_;
};

}
