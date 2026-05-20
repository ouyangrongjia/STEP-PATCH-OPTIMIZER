#pragma once

#include "brep/TopologyGraph.h"

#include <TopoDS_Shape.hxx>

#include <filesystem>
#include <string>

namespace spo {

struct ShapeStats {
    int solids = 0;
    int shells = 0;
    int faces = 0;
    int edges = 0;
    int vertices = 0;
};

class ShapeDocument {
public:
    ShapeDocument() = default;
    ShapeDocument(TopoDS_Shape shape, std::filesystem::path sourcePath);

    bool hasShape() const;
    const TopoDS_Shape& shape() const;
    const std::filesystem::path& sourcePath() const;
    const ShapeStats& stats() const;
    const TopologyGraph& topology() const;
    std::string displayName() const;

private:
    static ShapeStats computeStats(const TopoDS_Shape& shape);

    TopoDS_Shape shape_;
    std::filesystem::path sourcePath_;
    ShapeStats stats_;
    TopologyGraph topology_;
};

}
