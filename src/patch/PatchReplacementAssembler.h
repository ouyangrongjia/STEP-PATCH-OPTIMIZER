#pragma once

#include <TopoDS_Shape.hxx>

#include <string>

namespace spo {

class ShapeDocument;
struct BoundaryConstrainedPatchBuildResult;
struct RegionBoundaryAnalysis;

struct PatchReplacementAssemblyResult {
    bool success = false;
    TopoDS_Shape shape;
    std::string message;
    std::string warning;
};

class PatchReplacementAssembler {
public:
    PatchReplacementAssemblyResult assemble(
        const ShapeDocument& beforeDocument,
        const RegionBoundaryAnalysis& boundary,
        const BoundaryConstrainedPatchBuildResult& buildResult,
        bool forceFaceCompoundAssembly) const;
};

}
