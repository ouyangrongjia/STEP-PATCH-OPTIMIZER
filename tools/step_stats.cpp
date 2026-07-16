#include "common/Config.h"
#include "patch/PatchImportService.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom_BSplineSurface.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace {

void printCurveOnSurfaceDeviation(const TopoDS_Shape& shape) {
    struct IncidenceDeviation {
        int face = -1;
        int edge = -1;
        double deviation = 0.0;
        double edgeTolerance = 0.0;
    };
    struct SurfaceExtent {
        int face = -1;
        double area = 0.0;
        double faceDiagonal = 0.0;
        double poleDiagonal = 0.0;
        double extentRatio = 0.0;
        int uPoles = 0;
        int vPoles = 0;
    };

    TopTools_IndexedMapOfShape faces;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);

    double maximumDeviation = 0.0;
    int worstFace = -1;
    int worstEdge = -1;
    std::vector<double> deviations;
    std::vector<IncidenceDeviation> incidences;
    std::vector<SurfaceExtent> surfaceExtents;
    for (int faceIndex = 1; faceIndex <= faces.Extent(); ++faceIndex) {
        const auto face = TopoDS::Face(faces(faceIndex));
        BRepAdaptor_Surface surface(face, Standard_True);
        TopLoc_Location location;
        const auto basisSurface = BRep_Tool::Surface(face, location);
        const auto bspline = Handle(Geom_BSplineSurface)::DownCast(basisSurface);
        if (!bspline.IsNull()) {
            Bnd_Box faceBox;
            BRepBndLib::Add(face, faceBox);
            Bnd_Box poleBox;
            for (int u = 1; u <= bspline->NbUPoles(); ++u) {
                for (int v = 1; v <= bspline->NbVPoles(); ++v) {
                    poleBox.Add(bspline->Pole(u, v).Transformed(location.Transformation()));
                }
            }
            auto diagonal = [](const Bnd_Box& box) {
                if (box.IsVoid()) {
                    return 0.0;
                }
                double xmin = 0.0;
                double ymin = 0.0;
                double zmin = 0.0;
                double xmax = 0.0;
                double ymax = 0.0;
                double zmax = 0.0;
                box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                return gp_Pnt(xmin, ymin, zmin).Distance(gp_Pnt(xmax, ymax, zmax));
            };
            GProp_GProps properties;
            BRepGProp::SurfaceProperties(face, properties);
            const double faceDiagonal = diagonal(faceBox);
            const double poleDiagonal = diagonal(poleBox);
            surfaceExtents.push_back({
                faceIndex - 1,
                properties.Mass(),
                faceDiagonal,
                poleDiagonal,
                poleDiagonal / std::max(faceDiagonal, 1.0e-12),
                bspline->NbUPoles(),
                bspline->NbVPoles()});
        }
        for (TopExp_Explorer explorer(face, TopAbs_EDGE); explorer.More(); explorer.Next()) {
            const auto edge = TopoDS::Edge(explorer.Current());
            double first = 0.0;
            double last = 0.0;
            const auto pcurve = BRep_Tool::CurveOnSurface(edge, face, first, last);
            if (pcurve.IsNull() || !std::isfinite(first) || !std::isfinite(last)) {
                continue;
            }
            BRepAdaptor_Curve curve(edge);
            const double curveFirst = curve.FirstParameter();
            const double curveLast = curve.LastParameter();
            double incidenceMaximum = 0.0;
            constexpr int sampleCount = 65;
            for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
                const double fraction = static_cast<double>(sampleIndex) /
                    static_cast<double>(sampleCount - 1);
                const double parameter = first + (last - first) * fraction;
                const double curveParameter = curveFirst + (curveLast - curveFirst) * fraction;
                const auto uv = pcurve->Value(parameter);
                const double deviation = surface.Value(uv.X(), uv.Y()).Distance(
                    curve.Value(curveParameter));
                incidenceMaximum = std::max(incidenceMaximum, deviation);
            }
            deviations.push_back(incidenceMaximum);
            incidences.push_back({
                faceIndex - 1,
                edges.FindIndex(edge) - 1,
                incidenceMaximum,
                BRep_Tool::Tolerance(edge)});
            if (incidenceMaximum >= maximumDeviation) {
                maximumDeviation = incidenceMaximum;
                worstFace = faceIndex - 1;
                worstEdge = edges.FindIndex(edge) - 1;
            }
        }
    }

    std::sort(deviations.begin(), deviations.end());
    const double p95 = deviations.empty()
        ? 0.0
        : deviations[static_cast<std::size_t>(std::floor(
              0.95 * static_cast<double>(deviations.size() - 1)))];
    std::cout << "curve_on_surface_incidence_count: " << deviations.size() << "\n";
    std::cout << "curve_on_surface_max_deviation: " << maximumDeviation << "\n";
    std::cout << "curve_on_surface_p95_deviation: " << p95 << "\n";
    std::cout << "curve_on_surface_worst_face: " << worstFace << "\n";
    std::cout << "curve_on_surface_worst_edge: " << worstEdge << "\n";
    std::cout << "curve_on_surface_over_1e-4: " << std::count_if(
        deviations.begin(), deviations.end(), [](double value) { return value > 1.0e-4; }) << "\n";
    std::cout << "curve_on_surface_over_1e-3: " << std::count_if(
        deviations.begin(), deviations.end(), [](double value) { return value > 1.0e-3; }) << "\n";
    std::sort(incidences.begin(), incidences.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.deviation > rhs.deviation;
    });
    const auto topCount = std::min<std::size_t>(20, incidences.size());
    for (std::size_t index = 0; index < topCount; ++index) {
        const auto& incidence = incidences[index];
        std::cout << "curve_on_surface_top_" << index << ": "
                  << incidence.face << ';' << incidence.edge << ';'
                  << incidence.deviation << ';' << incidence.edgeTolerance << "\n";
    }
    std::sort(surfaceExtents.begin(), surfaceExtents.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.poleDiagonal > rhs.poleDiagonal;
    });
    const auto surfaceCount = std::min<std::size_t>(20, surfaceExtents.size());
    for (std::size_t index = 0; index < surfaceCount; ++index) {
        const auto& extent = surfaceExtents[index];
        std::cout << "bspline_extent_top_" << index << ": "
                  << extent.face << ';' << extent.area << ';'
                  << extent.faceDiagonal << ';' << extent.poleDiagonal << ';'
                  << extent.extentRatio << ';' << extent.uPoles << 'x' << extent.vPoles << "\n";
    }
}

}

int main(int argc, char* argv[]) {
    std::cout << spo::kApplicationName << " STEP stats\n";
    if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--curve-on-surface")) {
        std::cerr << "Usage: step_stats <patch.stp|patch.step|patch.igs|patch.iges> [--curve-on-surface]\n";
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
    if (!result.message.empty()) {
        std::cout << "message: " << result.message << "\n";
    }
    if (!result.errorMessage.empty()) {
        std::cerr << "error: " << result.errorMessage << "\n";
    }
    if (result.success && argc == 3) {
        printCurveOnSurfaceDeviation(result.shape);
    }
    return result.success ? 0 : 1;
}
