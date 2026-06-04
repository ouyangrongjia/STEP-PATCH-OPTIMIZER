#pragma once

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <string>
#include <vector>

namespace spo {

struct ImportedPatchInfo;

struct MultiFacePatchAnalysis {
    bool success = false;
    std::string message;

    int faceCount = 0;
    int edgeCount = 0;
    int shellCount = 0;
    int solidCount = 0;

    std::vector<TopoDS_Face> faces;
    std::vector<TopoDS_Edge> edges;
    std::vector<TopoDS_Edge> outerEdges;
    std::vector<TopoDS_Edge> internalEdges;

    bool bboxValid = false;
    bool brepCheckValid = false;
    bool hasAtLeastOneFace = false;
    bool isSingleFace = false;
    bool isMultiFace = false;
};

class MultiFacePatchAnalyzer {
public:
    MultiFacePatchAnalysis analyze(const ImportedPatchInfo& importedPatch) const;
};

}
