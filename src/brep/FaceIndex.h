#pragma once

#include "common/GeometryTypes.h"

#include <TopoDS_Face.hxx>

#include <vector>

namespace spo {

class FaceIndex {
public:
    FaceId addFace(const TopoDS_Face& face);
    std::size_t size() const;
    const TopoDS_Face& face(FaceId id) const;

private:
    std::vector<TopoDS_Face> faces_;
};

}
