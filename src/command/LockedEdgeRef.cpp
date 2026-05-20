#include "command/LockedEdgeRef.h"

#include "brep/TopologyGraph.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopExp.hxx>
#include <TopoDS_Vertex.hxx>

namespace spo {

namespace {

EdgeGeometrySignature signatureForEdge(const TopoDS_Edge& edge) {
    EdgeGeometrySignature signature;
    try {
        BRepAdaptor_Curve curve(edge);
        const auto firstParameter = curve.FirstParameter();
        const auto lastParameter = curve.LastParameter();
        signature.first = curve.Value(firstParameter);
        signature.last = curve.Value(lastParameter);
        signature.middle = curve.Value((firstParameter + lastParameter) * 0.5);
        signature.length = GCPnts_AbscissaPoint::Length(curve, firstParameter, lastParameter);
        signature.valid = true;
    } catch (...) {
        signature = {};
    }

    TopoDS_Vertex firstVertex;
    TopoDS_Vertex lastVertex;
    TopExp::Vertices(edge, firstVertex, lastVertex);
    if (signature.valid && !firstVertex.IsNull() && !lastVertex.IsNull()) {
        signature.first = BRep_Tool::Pnt(firstVertex);
        signature.last = BRep_Tool::Pnt(lastVertex);
    }
    return signature;
}

}

std::optional<LockedEdgeRef> makeLockedEdgeRef(const TopologyGraph& topology, EdgeId edgeId) {
    if (edgeId >= topology.edgeCount()) {
        return std::nullopt;
    }

    auto signature = signatureForEdge(topology.edge(edgeId));
    if (!signature.valid) {
        return std::nullopt;
    }
    return LockedEdgeRef {edgeId, signature};
}

std::set<EdgeId> lockedEdgeIds(const std::vector<LockedEdgeRef>& refs) {
    std::set<EdgeId> ids;
    for (const auto& ref : refs) {
        ids.insert(ref.edgeId);
    }
    return ids;
}

}
