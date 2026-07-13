#include "merge/SurfaceRefitter.h"

#include "merge/RegionBoundaryAnalyzer.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepFill_Filling.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_UniformAbscissa.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GeomAdaptor_Surface.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_State.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <utility>

namespace spo {

namespace {

constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
constexpr int kMaxTrainingSampleCount = 256;
constexpr int kMaxValidationSampleCount = 2048;
constexpr FaceId kInvalidFaceId = std::numeric_limits<FaceId>::max();
constexpr EdgeId kInvalidEdgeId = std::numeric_limits<EdgeId>::max();

struct SourceTriangle {
    FaceId face_id = -1;
    TopoDS_Face face;
    Handle(Geom_Surface) surface;
    gp_Pnt2d uv0;
    gp_Pnt2d uv1;
    gp_Pnt2d uv2;
    double area = 0.0;
};

struct SourceSample {
    gp_Pnt point;
    gp_Vec normal;
    FaceId face_id = -1;
};

struct DistanceSample {
    gp_Pnt point;
    FaceId face_id = -1;
    EdgeId edge_id = -1;
};

struct SurfaceProjection {
    bool success = false;
    bool normal_defined = false;
    double distance = 0.0;
    double u = 0.0;
    double v = 0.0;
    gp_Pnt point;
    gp_Vec normal;
};

struct SurfaceDomain {
    TopoDS_Face face;
    Handle(Geom_Surface) surface;
    double u_min = 0.0;
    double u_max = 0.0;
    double v_min = 0.0;
    double v_max = 0.0;
    bool valid = false;
};

void fail(
    SurfaceRefitReport& report,
    const SurfaceRefitFailureReason reason,
    std::string message) {
    report.success = false;
    report.failure_reason = reason;
    report.message = std::move(message);
}

bool valid_options(const SurfaceRefitOptions& options) {
    return options.tolerance2d > 0.0 &&
        options.tolerance3d > 0.0 &&
        options.tolerance_angle > 0.0 &&
        options.tolerance_curvature > 0.0 &&
        options.normal_tolerance_degrees > 0.0 &&
        options.mesh_deflection > 0.0 &&
        options.mesh_angular_deflection > 0.0 &&
        options.sample_deduplication_tolerance > 0.0 &&
        options.minimum_normalized_jacobian > 0.0 &&
        options.plate_degree > 0 &&
        options.points_on_curve >= 2 &&
        options.iterations > 0 &&
        options.max_degree > 0 &&
        options.max_segments > 0 &&
        options.training_sample_count > 0 &&
        options.training_sample_count <= kMaxTrainingSampleCount &&
        options.validation_sample_count > 0 &&
        options.validation_sample_count <= kMaxValidationSampleCount &&
        options.boundary_samples_per_edge >= 2 &&
        options.jacobian_grid_size >= 2 &&
        options.max_surface_degree > 0 &&
        options.max_surface_pole_count > 0 &&
        options.max_surface_knot_count > 0 &&
        options.max_surface_segment_count > 0;
}

double radical_inverse(std::uint64_t value, const std::uint64_t base) {
    double result = 0.0;
    double factor = 1.0 / static_cast<double>(base);
    while (value > 0) {
        result += factor * static_cast<double>(value % base);
        value /= base;
        factor /= static_cast<double>(base);
    }
    return result;
}

bool contains_point(
    const std::vector<SourceSample>& samples,
    const gp_Pnt& point,
    const double tolerance_squared) {
    return std::any_of(samples.begin(), samples.end(), [&](const SourceSample& sample) {
        return sample.point.SquareDistance(point) <= tolerance_squared;
    });
}

std::vector<SourceTriangle> build_source_triangles(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const SurfaceRefitOptions& options,
    std::string& message) {
    std::vector<SourceTriangle> triangles;
    const auto& topology = document.topology();

    for (const auto face_id : candidate.faces) {
        if (face_id < 0 || static_cast<std::size_t>(face_id) >= topology.faceCount()) {
            message = "Candidate references a missing source face.";
            return {};
        }

        const auto& face = topology.face(face_id);
        const auto surface = BRep_Tool::Surface(face);
        if (surface.IsNull()) {
            message = "Candidate source face has no geometric surface.";
            return {};
        }

        try {
            BRepMesh_IncrementalMesh mesher(
                face,
                options.mesh_deflection,
                Standard_False,
                options.mesh_angular_deflection,
                Standard_True);
            if (!mesher.IsDone()) {
                message = "OCCT could not triangulate a candidate source face.";
                return {};
            }
        } catch (const Standard_Failure& error) {
            const auto* text = error.GetMessageString();
            message = std::string("Candidate source face triangulation threw: ") +
                (text != nullptr ? text : "unknown OCCT error");
            return {};
        }

        TopLoc_Location location;
        const auto triangulation = BRep_Tool::Triangulation(face, location);
        if (triangulation.IsNull() || !triangulation->HasUVNodes()) {
            message = "Candidate source face triangulation has no UV nodes.";
            return {};
        }

        for (int triangle_index = 1; triangle_index <= triangulation->NbTriangles(); ++triangle_index) {
            int node0 = 0;
            int node1 = 0;
            int node2 = 0;
            triangulation->Triangle(triangle_index).Get(node0, node1, node2);
            const auto uv0 = triangulation->UVNode(node0);
            const auto uv1 = triangulation->UVNode(node1);
            const auto uv2 = triangulation->UVNode(node2);

            try {
                const auto point0 = surface->Value(uv0.X(), uv0.Y());
                const auto point1 = surface->Value(uv1.X(), uv1.Y());
                const auto point2 = surface->Value(uv2.X(), uv2.Y());
                const auto area = 0.5 * gp_Vec(point0, point1).Crossed(gp_Vec(point0, point2)).Magnitude();
                if (!std::isfinite(area) || area <= Precision::SquareConfusion()) {
                    continue;
                }
                triangles.push_back(SourceTriangle {face_id, face, surface, uv0, uv1, uv2, area});
            } catch (const Standard_Failure&) {
                continue;
            }
        }
    }

    if (triangles.empty()) {
        message = "Candidate source faces produced no usable area-weighted sampling triangles.";
    }
    return triangles;
}

std::vector<SourceSample> generate_samples(
    const std::vector<SourceTriangle>& triangles,
    const int requested_count,
    const std::uint64_t sequence_offset,
    const std::vector<SourceSample>& excluded,
    const SurfaceRefitOptions& options) {
    std::vector<SourceSample> samples;
    samples.reserve(static_cast<std::size_t>(requested_count));

    std::vector<double> cumulative_area;
    cumulative_area.reserve(triangles.size());
    double total_area = 0.0;
    for (const auto& triangle : triangles) {
        total_area += triangle.area;
        cumulative_area.push_back(total_area);
    }
    if (!(total_area > 0.0)) {
        return samples;
    }

    const auto tolerance_squared =
        options.sample_deduplication_tolerance * options.sample_deduplication_tolerance;
    const int attempt_limit = requested_count * 64 + 128;
    for (int attempt = 0; attempt < attempt_limit && static_cast<int>(samples.size()) < requested_count; ++attempt) {
        const auto sequence_index = sequence_offset + static_cast<std::uint64_t>(attempt) + 1;
        const auto area_position = radical_inverse(sequence_index, 2) * total_area;
        const auto found = std::upper_bound(cumulative_area.begin(), cumulative_area.end(), area_position);
        const auto triangle_index = std::min<std::size_t>(
            static_cast<std::size_t>(std::distance(cumulative_area.begin(), found)),
            triangles.size() - 1);
        const auto& triangle = triangles[triangle_index];

        const auto first = radical_inverse(sequence_index, 3);
        const auto second = radical_inverse(sequence_index, 5);
        const auto root = std::sqrt(first);
        const auto weight0 = 1.0 - root;
        const auto weight1 = root * (1.0 - second);
        const auto weight2 = root * second;
        const gp_Pnt2d uv(
            weight0 * triangle.uv0.X() + weight1 * triangle.uv1.X() + weight2 * triangle.uv2.X(),
            weight0 * triangle.uv0.Y() + weight1 * triangle.uv1.Y() + weight2 * triangle.uv2.Y());

        BRepClass_FaceClassifier classifier(
            triangle.face,
            uv,
            std::max(options.tolerance2d, Precision::PConfusion()),
            Standard_True);
        if (classifier.State() == TopAbs_OUT) {
            continue;
        }

        gp_Pnt point;
        gp_Vec du;
        gp_Vec dv;
        try {
            triangle.surface->D1(uv.X(), uv.Y(), point, du, dv);
        } catch (const Standard_Failure&) {
            continue;
        }
        auto normal = du.Crossed(dv);
        if (normal.SquareMagnitude() <= Precision::SquareConfusion()) {
            continue;
        }
        normal.Normalize();
        if (triangle.face.Orientation() == TopAbs_REVERSED) {
            normal.Reverse();
        }
        if (contains_point(samples, point, tolerance_squared) ||
            contains_point(excluded, point, tolerance_squared)) {
            continue;
        }
        samples.push_back(SourceSample {point, normal, triangle.face_id});
    }
    return samples;
}

std::vector<DistanceSample> sample_boundary(
    const ShapeDocument& document,
    const std::vector<EdgeId>& ordered_edges,
    const int samples_per_edge) {
    std::vector<DistanceSample> samples;
    const auto& topology = document.topology();
    for (const auto edge_id : ordered_edges) {
        if (edge_id < 0 || static_cast<std::size_t>(edge_id) >= topology.edgeCount()) {
            continue;
        }
        const auto& edge = topology.edge(edge_id);
        BRepAdaptor_Curve adaptor(edge);
        GCPnts_UniformAbscissa uniform(adaptor, samples_per_edge);
        if (uniform.IsDone() && uniform.NbPoints() == samples_per_edge) {
            for (int index = 1; index <= uniform.NbPoints(); ++index) {
                samples.push_back(DistanceSample {adaptor.Value(uniform.Parameter(index)), kInvalidFaceId, edge_id});
            }
            continue;
        }

        const auto first = adaptor.FirstParameter();
        const auto last = adaptor.LastParameter();
        for (int index = 0; index < samples_per_edge; ++index) {
            const auto ratio = static_cast<double>(index) / static_cast<double>(samples_per_edge - 1);
            samples.push_back(DistanceSample {adaptor.Value(first + (last - first) * ratio), kInvalidFaceId, edge_id});
        }
    }
    return samples;
}

std::vector<DistanceSample> sample_corners(
    const ShapeDocument& document,
    const std::vector<EdgeId>& ordered_edges,
    const double tolerance) {
    std::vector<DistanceSample> corners;
    const auto tolerance_squared = tolerance * tolerance;
    const auto& topology = document.topology();
    for (const auto edge_id : ordered_edges) {
        if (edge_id < 0 || static_cast<std::size_t>(edge_id) >= topology.edgeCount()) {
            continue;
        }
        TopoDS_Vertex first;
        TopoDS_Vertex second;
        TopExp::Vertices(topology.edge(edge_id), first, second);
        for (const auto& vertex : {first, second}) {
            if (vertex.IsNull()) {
                continue;
            }
            const auto point = BRep_Tool::Pnt(vertex);
            const auto duplicate = std::any_of(corners.begin(), corners.end(), [&](const DistanceSample& existing) {
                return existing.point.SquareDistance(point) <= tolerance_squared;
            });
            if (!duplicate) {
                corners.push_back(DistanceSample {point, kInvalidFaceId, edge_id});
            }
        }
    }
    return corners;
}

SurfaceDomain surface_domain(const TopoDS_Face& face) {
    SurfaceDomain domain;
    domain.face = face;
    domain.surface = BRep_Tool::Surface(face);
    if (domain.surface.IsNull()) {
        return domain;
    }
    BRepTools::UVBounds(face, domain.u_min, domain.u_max, domain.v_min, domain.v_max);
    domain.valid = std::isfinite(domain.u_min) && std::isfinite(domain.u_max) &&
        std::isfinite(domain.v_min) && std::isfinite(domain.v_max) &&
        domain.u_min < domain.u_max && domain.v_min < domain.v_max;
    return domain;
}

SurfaceProjection project_to_surface(
    const SurfaceDomain& domain,
    const gp_Pnt& point,
    const SurfaceRefitOptions& options,
    const bool require_inside_face) {
    SurfaceProjection result;
    if (!domain.valid) {
        return result;
    }

    try {
        GeomAPI_ProjectPointOnSurf projection(
            point,
            domain.surface,
            domain.u_min,
            domain.u_max,
            domain.v_min,
            domain.v_max,
            std::max(options.tolerance2d, Precision::Confusion()));
        if (!projection.IsDone() || projection.NbPoints() == 0) {
            return result;
        }
        for (int index = 1; index <= projection.NbPoints(); ++index) {
            double u = 0.0;
            double v = 0.0;
            projection.Parameters(index, u, v);
            if (require_inside_face) {
                BRepClass_FaceClassifier classifier(
                    domain.face,
                    gp_Pnt2d(u, v),
                    std::max(options.tolerance2d, Precision::PConfusion()),
                    Standard_True);
                if (classifier.State() == TopAbs_OUT) {
                    continue;
                }
            }
            const auto distance = projection.Distance(index);
            if (!result.success || distance < result.distance) {
                result.success = true;
                result.distance = distance;
                result.point = projection.Point(index);
                result.u = u;
                result.v = v;
            }
        }
        if (!result.success) {
            return result;
        }

        gp_Pnt evaluated;
        gp_Vec du;
        gp_Vec dv;
        domain.surface->D1(result.u, result.v, evaluated, du, dv);
        result.normal = du.Crossed(dv);
        if (result.normal.SquareMagnitude() > Precision::SquareConfusion()) {
            result.normal.Normalize();
            if (domain.face.Orientation() == TopAbs_REVERSED) {
                result.normal.Reverse();
            }
            result.normal_defined = true;
        }
        result.success = std::isfinite(result.distance);
    } catch (const Standard_Failure&) {
        return {};
    }
    return result;
}

double percentile95(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

SurfaceRefitErrorStatistics evaluate_distances(
    const std::vector<DistanceSample>& samples,
    const SurfaceDomain& domain,
    const SurfaceRefitOptions& options,
    const bool require_inside_face) {
    SurfaceRefitErrorStatistics statistics;
    statistics.sample_count = static_cast<int>(samples.size());
    std::vector<double> distances;
    distances.reserve(samples.size());
    double squared_sum = 0.0;

    for (const auto& sample : samples) {
        const auto projection = project_to_surface(domain, sample.point, options, require_inside_face);
        if (!projection.success) {
            ++statistics.projection_failure_count;
            ++statistics.over_tolerance_count;
            continue;
        }
        distances.push_back(projection.distance);
        squared_sum += projection.distance * projection.distance;
        if (projection.distance > options.tolerance3d) {
            ++statistics.over_tolerance_count;
        }
        if (distances.size() == 1 || projection.distance > statistics.max) {
            statistics.max = projection.distance;
            statistics.worst_point = sample.point;
            statistics.worst_face_id = sample.face_id;
            statistics.worst_edge_id = sample.edge_id;
        }
    }

    statistics.p95 = percentile95(distances);
    if (!distances.empty()) {
        statistics.rms = std::sqrt(squared_sum / static_cast<double>(distances.size()));
    }
    return statistics;
}

SurfaceRefitErrorStatistics evaluate_interior_distances(
    const std::vector<SourceSample>& samples,
    const SurfaceDomain& domain,
    const SurfaceRefitOptions& options) {
    std::vector<DistanceSample> distance_samples;
    distance_samples.reserve(samples.size());
    for (const auto& sample : samples) {
        distance_samples.push_back(DistanceSample {sample.point, sample.face_id, kInvalidEdgeId});
    }
    return evaluate_distances(distance_samples, domain, options, true);
}

SurfaceRefitNormalStatistics evaluate_normals(
    const std::vector<SourceSample>& samples,
    const SurfaceDomain& domain,
    const SurfaceRefitOptions& options) {
    SurfaceRefitNormalStatistics statistics;
    statistics.sample_count = static_cast<int>(samples.size());
    std::vector<double> angles;
    angles.reserve(samples.size());
    double squared_sum = 0.0;

    for (const auto& sample : samples) {
        const auto projection = project_to_surface(domain, sample.point, options, true);
        if (!projection.success || !projection.normal_defined ||
            sample.normal.SquareMagnitude() <= Precision::SquareConfusion()) {
            ++statistics.undefined_count;
            ++statistics.over_tolerance_count;
            continue;
        }
        const auto cosine = std::clamp(sample.normal.Dot(projection.normal), -1.0, 1.0);
        const auto angle = std::acos(cosine) * kRadiansToDegrees;
        angles.push_back(angle);
        squared_sum += angle * angle;
        if (angle > options.normal_tolerance_degrees) {
            ++statistics.over_tolerance_count;
        }
        if (angles.size() == 1 || angle > statistics.max_angle_degrees) {
            statistics.max_angle_degrees = angle;
            statistics.worst_point = sample.point;
            statistics.worst_face_id = sample.face_id;
        }
    }

    statistics.p95_angle_degrees = percentile95(angles);
    if (!angles.empty()) {
        statistics.rms_angle_degrees = std::sqrt(squared_sum / static_cast<double>(angles.size()));
    }
    return statistics;
}

bool should_reverse_output_orientation(
    const std::vector<SourceSample>& validation,
    const SurfaceDomain& domain,
    const SurfaceRefitOptions& options) {
    double signed_alignment = 0.0;
    int comparison_count = 0;
    for (const auto& sample : validation) {
        const auto projection = project_to_surface(domain, sample.point, options, true);
        if (!projection.success || !projection.normal_defined) {
            continue;
        }
        signed_alignment += sample.normal.Dot(projection.normal);
        ++comparison_count;
    }
    return comparison_count > 0 && signed_alignment < 0.0;
}

SurfaceRefitJacobianReport inspect_jacobian(
    const TopoDS_Face& face,
    const SurfaceDomain& domain,
    const SurfaceRefitOptions& options) {
    SurfaceRefitJacobianReport report;
    report.min_magnitude = std::numeric_limits<double>::infinity();
    report.min_normalized = std::numeric_limits<double>::infinity();

    struct GridNormal {
        bool valid = false;
        gp_Vec normal;
    };
    const int grid_size = options.jacobian_grid_size;
    std::vector<GridNormal> grid(static_cast<std::size_t>(grid_size * grid_size));

    for (int u_index = 0; u_index < grid_size; ++u_index) {
        const auto u_ratio = (static_cast<double>(u_index) + 0.5) / static_cast<double>(grid_size);
        const auto u = domain.u_min + (domain.u_max - domain.u_min) * u_ratio;
        for (int v_index = 0; v_index < grid_size; ++v_index) {
            const auto v_ratio = (static_cast<double>(v_index) + 0.5) / static_cast<double>(grid_size);
            const auto v = domain.v_min + (domain.v_max - domain.v_min) * v_ratio;
            BRepClass_FaceClassifier classifier(
                face,
                gp_Pnt2d(u, v),
                std::max(options.tolerance2d, Precision::PConfusion()),
                Standard_True);
            if (classifier.State() == TopAbs_OUT) {
                continue;
            }

            gp_Pnt point;
            gp_Vec du;
            gp_Vec dv;
            try {
                domain.surface->D1(u, v, point, du, dv);
            } catch (const Standard_Failure&) {
                ++report.near_singular_count;
                continue;
            }
            auto cross = du.Crossed(dv);
            const auto magnitude = cross.Magnitude();
            const auto denominator = du.Magnitude() * dv.Magnitude();
            const auto normalized = denominator > Precision::SquareConfusion()
                ? magnitude / denominator
                : 0.0;
            ++report.sample_count;
            report.max_magnitude = std::max(report.max_magnitude, magnitude);
            if (magnitude < report.min_magnitude) {
                report.min_magnitude = magnitude;
                report.worst_u = u;
                report.worst_v = v;
            }
            report.min_normalized = std::min(report.min_normalized, normalized);
            if (magnitude <= Precision::Confusion() ||
                normalized < options.minimum_normalized_jacobian) {
                ++report.near_singular_count;
                continue;
            }
            cross.Normalize();
            grid[static_cast<std::size_t>(u_index * grid_size + v_index)] = GridNormal {true, cross};
        }
    }

    auto compare = [&](const int first_u, const int first_v, const int second_u, const int second_v) {
        const auto& first = grid[static_cast<std::size_t>(first_u * grid_size + first_v)];
        const auto& second = grid[static_cast<std::size_t>(second_u * grid_size + second_v)];
        if (first.valid && second.valid && first.normal.Dot(second.normal) < 0.0) {
            ++report.flipped_pair_count;
        }
    };
    for (int u_index = 0; u_index < grid_size; ++u_index) {
        for (int v_index = 0; v_index < grid_size; ++v_index) {
            if (u_index + 1 < grid_size) {
                compare(u_index, v_index, u_index + 1, v_index);
            }
            if (v_index + 1 < grid_size) {
                compare(u_index, v_index, u_index, v_index + 1);
            }
        }
    }

    if (!std::isfinite(report.min_magnitude)) {
        report.min_magnitude = 0.0;
    }
    if (!std::isfinite(report.min_normalized)) {
        report.min_normalized = 0.0;
    }
    return report;
}

const char* surface_type_name(const GeomAbs_SurfaceType type) {
    switch (type) {
    case GeomAbs_Plane: return "Plane";
    case GeomAbs_Cylinder: return "Cylinder";
    case GeomAbs_Cone: return "Cone";
    case GeomAbs_Sphere: return "Sphere";
    case GeomAbs_Torus: return "Torus";
    case GeomAbs_BezierSurface: return "Bezier";
    case GeomAbs_BSplineSurface: return "BSpline";
    case GeomAbs_SurfaceOfRevolution: return "SurfaceOfRevolution";
    case GeomAbs_SurfaceOfExtrusion: return "SurfaceOfExtrusion";
    case GeomAbs_OffsetSurface: return "Offset";
    case GeomAbs_OtherSurface: return "Other";
    }
    return "Unknown";
}

SurfaceRefitComplexityReport inspect_complexity(const Handle(Geom_Surface)& surface) {
    SurfaceRefitComplexityReport report;
    if (surface.IsNull()) {
        report.surface_type = "Null";
        return report;
    }

    try {
        GeomAdaptor_Surface adaptor(surface);
        const auto type = adaptor.GetType();
        report.surface_type = surface_type_name(type);
        if (type == GeomAbs_BSplineSurface) {
            report.u_degree = adaptor.UDegree();
            report.v_degree = adaptor.VDegree();
            report.u_poles = adaptor.NbUPoles();
            report.v_poles = adaptor.NbVPoles();
            report.u_knots = adaptor.NbUKnots();
            report.v_knots = adaptor.NbVKnots();
        } else if (type == GeomAbs_BezierSurface) {
            report.u_degree = adaptor.UDegree();
            report.v_degree = adaptor.VDegree();
            report.u_poles = adaptor.NbUPoles();
            report.v_poles = adaptor.NbVPoles();
            report.u_knots = 2;
            report.v_knots = 2;
        }
    } catch (const Standard_Failure&) {
        report.surface_type = "Unknown";
    }

    report.total_poles = report.u_poles * report.v_poles;
    report.u_segments = std::max(0, report.u_knots - 1);
    report.v_segments = std::max(0, report.v_knots - 1);
    report.total_segments = report.u_segments * report.v_segments;
    return report;
}

void inspect_boundary_representation(const TopoDS_Face& face, SurfaceRefitReport& report) {
    for (TopExp_Explorer explorer(face, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        const auto edge = TopoDS::Edge(explorer.Current());
        ++report.output_edge_count;
        double first = 0.0;
        double last = 0.0;
        const auto pcurve = BRep_Tool::CurveOnSurface(edge, face, first, last);
        if (pcurve.IsNull()) {
            ++report.missing_pcurve_count;
        }
        if (!BRep_Tool::SameParameter(edge)) {
            ++report.same_parameter_failure_count;
        }
    }
}

bool complexity_exceeded(
    const SurfaceRefitComplexityReport& complexity,
    const SurfaceRefitOptions& options) {
    return complexity.u_degree > options.max_surface_degree ||
        complexity.v_degree > options.max_surface_degree ||
        complexity.total_poles > options.max_surface_pole_count ||
        complexity.u_knots + complexity.v_knots > options.max_surface_knot_count ||
        complexity.total_segments > options.max_surface_segment_count;
}

int overlap_count(
    const std::vector<SourceSample>& training,
    const std::vector<SourceSample>& validation,
    const double tolerance) {
    const auto tolerance_squared = tolerance * tolerance;
    int count = 0;
    for (const auto& validation_sample : validation) {
        if (contains_point(training, validation_sample.point, tolerance_squared)) {
            ++count;
        }
    }
    return count;
}

} // namespace

const char* surfaceRefitFailureReasonToString(const SurfaceRefitFailureReason reason) {
    switch (reason) {
    case SurfaceRefitFailureReason::None: return "None";
    case SurfaceRefitFailureReason::InvalidInput: return "InvalidInput";
    case SurfaceRefitFailureReason::UnsupportedCandidateType: return "UnsupportedCandidateType";
    case SurfaceRefitFailureReason::BoundaryInvalid: return "BoundaryInvalid";
    case SurfaceRefitFailureReason::SampleGenerationFailed: return "SampleGenerationFailed";
    case SurfaceRefitFailureReason::FillingFailed: return "FillingFailed";
    case SurfaceRefitFailureReason::NullFace: return "NullFace";
    case SurfaceRefitFailureReason::BoundaryRepresentationInvalid: return "BoundaryRepresentationInvalid";
    case SurfaceRefitFailureReason::BRepCheckFailed: return "BRepCheckFailed";
    case SurfaceRefitFailureReason::BoundaryErrorExceeded: return "BoundaryErrorExceeded";
    case SurfaceRefitFailureReason::CornerErrorExceeded: return "CornerErrorExceeded";
    case SurfaceRefitFailureReason::InteriorErrorExceeded: return "InteriorErrorExceeded";
    case SurfaceRefitFailureReason::NormalErrorExceeded: return "NormalErrorExceeded";
    case SurfaceRefitFailureReason::JacobianInvalid: return "JacobianInvalid";
    case SurfaceRefitFailureReason::ComplexityExceeded: return "ComplexityExceeded";
    }
    return "Unknown";
}

SurfaceRefitResult SurfaceRefitter::fitSingleFace(
    const ShapeDocument& document,
    const MergeCandidate& candidate,
    const SurfaceRefitOptions& options) const {
    SurfaceRefitResult result;
    auto& report = result.report;
    report.candidate_id = candidate.candidate_id;
    report.candidate_face_count = static_cast<int>(candidate.faces.size());
    report.requested_training_sample_count = options.training_sample_count;
    report.requested_validation_sample_count = options.validation_sample_count;

    if (!document.hasShape() || candidate.faces.empty() || !valid_options(options)) {
        fail(report, SurfaceRefitFailureReason::InvalidInput, "Single-face refit requires a loaded shape, source faces, and valid options.");
        return result;
    }
    if (candidate.candidate_type != MergeCandidateType::FeatureBoundedRefit) {
        fail(report, SurfaceRefitFailureReason::UnsupportedCandidateType, "Single-face refit only supports FeatureBoundedRefit candidates.");
        return result;
    }

    const auto boundary = RegionBoundaryAnalyzer().analyze(document, candidate);
    report.boundary_valid = boundary.valid;
    report.ordered_boundary_edges = boundary.ordered_boundary_edges;
    if (!boundary.valid || boundary.ordered_boundary_edges.empty()) {
        std::ostringstream message;
        message << "Single-face refit rejected candidate boundary: " << boundary.message;
        fail(report, SurfaceRefitFailureReason::BoundaryInvalid, message.str());
        return result;
    }

    std::string sample_message;
    const auto triangles = build_source_triangles(document, candidate, options, sample_message);
    if (triangles.empty()) {
        fail(report, SurfaceRefitFailureReason::SampleGenerationFailed, sample_message);
        return result;
    }

    const auto training = generate_samples(triangles, options.training_sample_count, 0, {}, options);
    const auto validation = generate_samples(triangles, options.validation_sample_count, 104729, training, options);
    report.training_sample_count = static_cast<int>(training.size());
    report.validation_sample_count = static_cast<int>(validation.size());
    report.training_validation_overlap_count = overlap_count(
        training,
        validation,
        options.sample_deduplication_tolerance);
    if (report.training_sample_count != options.training_sample_count ||
        report.validation_sample_count != options.validation_sample_count ||
        report.training_validation_overlap_count != 0) {
        fail(
            report,
            SurfaceRefitFailureReason::SampleGenerationFailed,
            "Could not build complete, disjoint area-weighted training and validation sample sets.");
        return result;
    }

    BRepFill_Filling filling(
        options.plate_degree,
        options.points_on_curve,
        options.iterations,
        Standard_False,
        options.tolerance2d,
        options.tolerance3d,
        options.tolerance_angle,
        options.tolerance_curvature,
        options.max_degree,
        options.max_segments);
    filling.SetConstrParam(
        options.tolerance2d,
        options.tolerance3d,
        options.tolerance_angle,
        options.tolerance_curvature);

    const auto& topology = document.topology();
    try {
        for (const auto edge_id : boundary.ordered_boundary_edges) {
            if (edge_id < 0 || static_cast<std::size_t>(edge_id) >= topology.edgeCount()) {
                fail(report, SurfaceRefitFailureReason::BoundaryInvalid, "Ordered boundary references a missing edge.");
                return result;
            }
            filling.Add(topology.edge(edge_id), GeomAbs_C0, Standard_True);
        }
        for (const auto& sample : training) {
            filling.Add(sample.point);
        }
        filling.Build();
    } catch (const Standard_Failure& error) {
        const auto* text = error.GetMessageString();
        fail(
            report,
            SurfaceRefitFailureReason::FillingFailed,
            std::string("BRepFill_Filling threw: ") + (text != nullptr ? text : "unknown OCCT error"));
        return result;
    }

    report.filling_done = filling.IsDone();
    if (!report.filling_done) {
        fail(report, SurfaceRefitFailureReason::FillingFailed, "BRepFill_Filling did not finish.");
        return result;
    }
    report.g0_error = filling.G0Error();
    report.g1_error = filling.G1Error();
    report.g2_error = filling.G2Error();

    result.face = filling.Face();
    if (result.face.IsNull()) {
        fail(report, SurfaceRefitFailureReason::NullFace, "BRepFill_Filling produced a null face.");
        return result;
    }

    inspect_boundary_representation(result.face, report);
    report.brep_check_valid = BRepCheck_Analyzer(result.face).IsValid();
    auto domain = surface_domain(result.face);
    if (!domain.valid) {
        fail(report, SurfaceRefitFailureReason::NullFace, "Fitted face has no finite surface domain.");
        return result;
    }
    if (should_reverse_output_orientation(validation, domain, options)) {
        result.face.Reverse();
        report.output_orientation_reversed = true;
        domain = surface_domain(result.face);
    }

    const auto boundary_samples = sample_boundary(
        document,
        boundary.ordered_boundary_edges,
        options.boundary_samples_per_edge);
    const auto corner_samples = sample_corners(
        document,
        boundary.ordered_boundary_edges,
        options.sample_deduplication_tolerance);
    report.boundary_error = evaluate_distances(boundary_samples, domain, options, false);
    report.corner_error = evaluate_distances(corner_samples, domain, options, false);
    report.interior_error = evaluate_interior_distances(validation, domain, options);
    report.normal_error = evaluate_normals(validation, domain, options);
    report.jacobian = inspect_jacobian(result.face, domain, options);
    report.complexity = inspect_complexity(domain.surface);

    if (report.missing_pcurve_count > 0 || report.same_parameter_failure_count > 0) {
        fail(report, SurfaceRefitFailureReason::BoundaryRepresentationInvalid, "Fitted face has missing pcurves or non-SameParameter boundary edges.");
        return result;
    }
    if (!report.brep_check_valid) {
        fail(report, SurfaceRefitFailureReason::BRepCheckFailed, "Fitted face failed BRepCheck.");
        return result;
    }
    if (report.g0_error > options.tolerance3d ||
        report.boundary_error.projection_failure_count > 0 ||
        report.boundary_error.over_tolerance_count > 0) {
        fail(report, SurfaceRefitFailureReason::BoundaryErrorExceeded, "Fitted face exceeds the original-boundary G0 tolerance.");
        return result;
    }
    if (report.corner_error.projection_failure_count > 0 ||
        report.corner_error.over_tolerance_count > 0) {
        fail(report, SurfaceRefitFailureReason::CornerErrorExceeded, "Fitted face exceeds the original corner tolerance.");
        return result;
    }
    if (report.interior_error.projection_failure_count > 0 ||
        report.interior_error.over_tolerance_count > 0) {
        fail(report, SurfaceRefitFailureReason::InteriorErrorExceeded, "Fitted face exceeds the independent interior validation tolerance.");
        return result;
    }
    if (report.normal_error.undefined_count > 0 ||
        report.normal_error.over_tolerance_count > 0) {
        fail(report, SurfaceRefitFailureReason::NormalErrorExceeded, "Fitted face exceeds the independent normal validation tolerance.");
        return result;
    }
    if (report.jacobian.sample_count == 0 ||
        report.jacobian.near_singular_count > 0 ||
        report.jacobian.flipped_pair_count > 0) {
        fail(report, SurfaceRefitFailureReason::JacobianInvalid, "Fitted face has a near-singular or locally flipped Jacobian sample.");
        return result;
    }
    if (complexity_exceeded(report.complexity, options)) {
        fail(report, SurfaceRefitFailureReason::ComplexityExceeded, "Fitted surface complexity exceeds the configured NSR-1 limits.");
        return result;
    }

    report.success = true;
    report.failure_reason = SurfaceRefitFailureReason::None;
    report.message = "Built one G0 original-boundary filling face with disjoint STEP-derived training and validation samples.";
    return result;
}

} // namespace spo
