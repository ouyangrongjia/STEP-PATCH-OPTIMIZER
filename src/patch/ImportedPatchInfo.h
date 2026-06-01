#pragma once

#include <TopoDS_Shape.hxx>

#include <filesystem>
#include <string>

namespace spo {

struct ImportedPatchInfo {
    bool success = false;
    bool brepCheckValid = false;

    std::string message;
    std::string errorMessage;

    std::filesystem::path sourcePath;
    std::filesystem::path attemptedStepPath;
    std::filesystem::path attemptedIgesPath;

    TopoDS_Shape shape;

    int faceCount = 0;
    int edgeCount = 0;
    int solidCount = 0;
    int shellCount = 0;

    bool bboxValid = false;
    double bboxMinX = 0.0;
    double bboxMinY = 0.0;
    double bboxMinZ = 0.0;
    double bboxMaxX = 0.0;
    double bboxMaxY = 0.0;
    double bboxMaxZ = 0.0;
};

}
