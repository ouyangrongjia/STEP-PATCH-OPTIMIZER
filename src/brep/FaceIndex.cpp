#include "brep/FaceIndex.h"

namespace spo {

FaceId FaceIndex::addFace(const TopoDS_Face& face) {
    const auto id = faces_.size();
    faces_.push_back(face);
    return id;
}

std::size_t FaceIndex::size() const {
    return faces_.size();
}

const TopoDS_Face& FaceIndex::face(FaceId id) const {
    return faces_.at(id);
}

}
