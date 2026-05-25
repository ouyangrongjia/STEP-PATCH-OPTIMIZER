#include "brep/ShapeDocument.h"

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>

namespace spo {

ShapeDocument::ShapeDocument(TopoDS_Shape shape, std::filesystem::path sourcePath)
    : shape_(std::move(shape)), sourcePath_(std::move(sourcePath)), stats_(computeStats(shape_)) {
    topology_.build(shape_);
}

bool ShapeDocument::hasShape() const {
    return !shape_.IsNull();
}

const TopoDS_Shape& ShapeDocument::shape() const {
    return shape_;
}

const std::filesystem::path& ShapeDocument::sourcePath() const {
    return sourcePath_;
}

const ShapeStats& ShapeDocument::stats() const {
    return stats_;
}

const TopologyGraph& ShapeDocument::topology() const {
    return topology_;
}

std::string ShapeDocument::displayName() const {
    if (sourcePath_.empty()) {
        return "未命名";
    }
    const auto utf8Name = sourcePath_.filename().u8string();
    return {reinterpret_cast<const char*>(utf8Name.c_str()), utf8Name.size()};
}

ShapeStats ShapeDocument::computeStats(const TopoDS_Shape& shape) {
    ShapeStats stats;
    if (shape.IsNull()) {
        return stats;
    }

    for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
        ++stats.solids;
    }
    for (TopExp_Explorer explorer(shape, TopAbs_SHELL); explorer.More(); explorer.Next()) {
        ++stats.shells;
    }
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        ++stats.faces;
    }
    for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        ++stats.edges;
    }
    for (TopExp_Explorer explorer(shape, TopAbs_VERTEX); explorer.More(); explorer.Next()) {
        ++stats.vertices;
    }

    return stats;
}

}
