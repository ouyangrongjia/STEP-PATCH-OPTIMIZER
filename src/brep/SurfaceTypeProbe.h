#pragma once

#include <TopoDS_Face.hxx>

#include <string>

namespace spo {

std::string surfaceTypeName(const TopoDS_Face& face);

}
