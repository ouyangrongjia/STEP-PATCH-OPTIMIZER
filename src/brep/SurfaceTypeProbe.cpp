#include "brep/SurfaceTypeProbe.h"

#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>

namespace spo {

std::string surfaceTypeName(const TopoDS_Face& face) {
    if (face.IsNull()) {
        return "Unknown";
    }

    try {
        BRepAdaptor_Surface surface(face);
        switch (surface.GetType()) {
        case GeomAbs_Plane:
            return "Plane";
        case GeomAbs_Cylinder:
            return "Cylinder";
        case GeomAbs_Cone:
            return "Cone";
        case GeomAbs_Sphere:
            return "Sphere";
        case GeomAbs_Torus:
            return "Torus";
        case GeomAbs_BezierSurface:
            return "Bezier";
        case GeomAbs_BSplineSurface:
            return "BSpline";
        case GeomAbs_SurfaceOfRevolution:
            return "SurfaceOfRevolution";
        case GeomAbs_SurfaceOfExtrusion:
            return "SurfaceOfExtrusion";
        case GeomAbs_OffsetSurface:
            return "OffsetSurface";
        case GeomAbs_OtherSurface:
            return "Other";
        }
    } catch (...) {
        return "Unknown";
    }

    return "Unknown";
}

}
