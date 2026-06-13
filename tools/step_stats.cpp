#include "common/Config.h"
#include "brep/ShapeDocument.h"
#include "patch/PatchImportService.h"
#include "validate/ShapeValidator.h"

#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

#include <iostream>

namespace {

bool is_closed_seam_on_single_face(
    const spo::ShapeDocument& document,
    spo::EdgeId edgeId,
    const spo::EdgeAdjacency& adjacency) {
    return adjacency.faces.size() == 1 &&
        BRep_Tool::IsClosed(document.topology().edge(edgeId), document.topology().face(adjacency.faces.front()));
}

bool is_free_edge_for_validation(
    const spo::ShapeDocument& document,
    spo::EdgeId edgeId,
    const spo::EdgeAdjacency& adjacency) {
    return adjacency.use_count == 1 &&
        !is_closed_seam_on_single_face(document, edgeId, adjacency) &&
        !BRep_Tool::Degenerated(document.topology().edge(edgeId));
}

}

int main(int argc, char* argv[]) {
    std::cout << spo::kApplicationName << " STEP stats\n";
    if (argc != 2) {
        std::cerr << "Usage: step_stats <patch.stp|patch.step|patch.igs|patch.iges>\n";
        return 2;
    }

    const auto result = spo::PatchImportService().importPatch(argv[1]);
    std::cout << "path: " << argv[1] << "\n";
    std::cout << "success: " << result.success << "\n";
    std::cout << "faces: " << result.faceCount << "\n";
    std::cout << "edges: " << result.edgeCount << "\n";
    std::cout << "shells: " << result.shellCount << "\n";
    std::cout << "solids: " << result.solidCount << "\n";
    std::cout << "bbox_valid: " << result.bboxValid << "\n";
    std::cout << "brep_check_valid: " << result.brepCheckValid << "\n";
    if (!result.shape.IsNull()) {
        const spo::ShapeDocument document(result.shape, {});
        const auto validation = spo::ShapeValidator().validate(document);
        std::cout << "free_edges: " << validation.free_edges << "\n";
        std::cout << "multiple_edges: " << validation.multiple_edges << "\n";
        int printed = 0;
        for (spo::EdgeId edgeId = 0; edgeId < document.topology().edgeCount() && printed < 8; ++edgeId) {
            const auto* adjacency = document.topology().adjacencyForEdge(edgeId);
            if (adjacency == nullptr || !is_free_edge_for_validation(document, edgeId, *adjacency)) {
                continue;
            }
            const auto& edge = document.topology().edge(edgeId);
            GProp_GProps props;
            BRepGProp::LinearProperties(edge, props);
            double first = 0.0;
            double last = 0.0;
            const auto curve = BRep_Tool::Curve(edge, first, last);
            std::cout << "free_edge[" << printed << "] id=" << edgeId
                      << " length=" << props.Mass()
                      << " use_count=" << adjacency->use_count
                      << " unique_faces=" << adjacency->faces.size()
                      << " face=" << adjacency->faces.front()
                      << " closed=" << edge.Closed()
                      << " degenerated=" << BRep_Tool::Degenerated(edge)
                      << " seam_on_face=" << BRep_Tool::IsClosed(edge, document.topology().face(adjacency->faces.front()));
            if (!curve.IsNull()) {
                const auto p0 = curve->Value(first);
                const auto p1 = curve->Value(last);
                std::cout << " p0=(" << p0.X() << "," << p0.Y() << "," << p0.Z() << ")"
                          << " p1=(" << p1.X() << "," << p1.Y() << "," << p1.Z() << ")";
            }
            TopoDS_Vertex firstVertex;
            TopoDS_Vertex lastVertex;
            TopExp::Vertices(edge, firstVertex, lastVertex);
            if (!firstVertex.IsNull() && !lastVertex.IsNull()) {
                const auto p0 = BRep_Tool::Pnt(firstVertex);
                const auto p1 = BRep_Tool::Pnt(lastVertex);
                std::cout << " v0=(" << p0.X() << "," << p0.Y() << "," << p0.Z() << ")"
                          << " v1=(" << p1.X() << "," << p1.Y() << "," << p1.Z() << ")";
            }
            std::cout << "\n";
            ++printed;
        }
    }
    if (!result.message.empty()) {
        std::cout << "message: " << result.message << "\n";
    }
    if (!result.errorMessage.empty()) {
        std::cerr << "error: " << result.errorMessage << "\n";
    }
    return result.success ? 0 : 1;
}
