#pragma once

#include "common/GeometryTypes.h"
#include "stl/StlMesh.h"

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace spo {

struct RegionReferenceSourceSample {
    FaceId faceId = 0;
    gp_Pnt2d uv;
    gp_Dir normal {0.0, 0.0, 1.0};
    bool normalDefined = false;
    double minimumCurvature = 0.0;
    double maximumCurvature = 0.0;
    bool curvatureDefined = false;
};

struct RegionReferenceVertex {
    gp_Pnt point;
    gp_Dir normal {0.0, 0.0, 1.0};
    std::vector<RegionReferenceSourceSample> sources;
    std::vector<EdgeId> boundaryEdgeIds;
    bool boundary = false;
    bool hardBoundary = false;
    bool boundaryEndpoint = false;
    bool hardCorner = false;
    bool internalSeam = false;
    bool normalDiscontinuity = false;
    double areaWeight = 1.0;
    double boundaryLengthWeight = 1.0;
};

struct RegionReferenceTriangle {
    std::array<std::size_t, 3> vertices {};
    FaceId faceId = 0;
};

struct RegionBoundarySample {
    EdgeId edgeId = 0;
    double parameter = 0.0;
    std::size_t vertexIndex = 0;
    bool endpoint = false;
    bool hardCorner = false;
};

class RegionReferenceMesh {
public:
    bool empty() const;
    StlMesh toStlMesh() const;
    std::string deterministicFingerprint() const;
    void updateIntegrationWeights();

    std::vector<RegionReferenceVertex> vertices;
    std::vector<RegionReferenceTriangle> triangles;
    std::vector<RegionBoundarySample> boundarySamples;
};

}
