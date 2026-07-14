#include "merge/SurfaceRefitDiagnostics.h"

#include <fstream>
#include <iomanip>
#include <limits>
#include <system_error>

namespace spo {

namespace {

template <typename Id>
void write_optional_id(std::ostream& output, const Id id) {
    if (id != std::numeric_limits<Id>::max()) {
        output << id;
    }
}

}

SurfaceRefitDiagnosticArtifacts writeSurfaceRefitDiagnosticArtifacts(
    const SurfaceRefitReport& report,
    const std::filesystem::path& output_directory) {
    SurfaceRefitDiagnosticArtifacts artifacts;
    artifacts.csv_path = output_directory / "surface_error_samples.csv";
    artifacts.obj_path = output_directory / "surface_error_vectors.obj";
    artifacts.normal_csv_path = output_directory / "surface_normal_error_samples.csv";

    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) {
        artifacts.message = "Could not create native surface diagnostics directory: " + error.message();
        return artifacts;
    }

    std::ofstream csv(artifacts.csv_path, std::ios::binary);
    if (!csv.good()) {
        artifacts.message = "Could not open surface_error_samples.csv for writing.";
        return artifacts;
    }
    csv << std::setprecision(17);
    csv << "sample_set,category,source_x,source_y,source_z,projected_x,projected_y,projected_z,"
           "distance,projection_success,over_tolerance,face_id,edge_id,nearest_training_distance\n";
    auto write_distance_sample = [&](const SurfaceRefitSampleDiagnostic& sample) {
        csv << surfaceRefitSampleSetToString(sample.sample_set) << ','
            << surfaceRefitSampleCategoryToString(sample.category) << ','
            << sample.source_point.X() << ','
            << sample.source_point.Y() << ','
            << sample.source_point.Z() << ',';
        if (sample.projection_success) {
            csv << sample.projected_point.X() << ','
                << sample.projected_point.Y() << ','
                << sample.projected_point.Z() << ','
                << sample.distance;
        } else {
            csv << ",,,";
        }
        csv << ',' << (sample.projection_success ? "true" : "false")
            << ',' << (sample.over_tolerance ? "true" : "false") << ',';
        write_optional_id(csv, sample.face_id);
        csv << ',';
        write_optional_id(csv, sample.edge_id);
        csv << ',';
        if (sample.nearest_training_distance_defined) {
            csv << sample.nearest_training_distance;
        }
        csv << '\n';
    };
    for (const auto& sample : report.surface_error_samples) {
        write_distance_sample(sample);
    }
    for (const auto& sample : report.selection_error_samples) {
        write_distance_sample(sample);
    }
    csv.flush();
    artifacts.csv_written = csv.good();
    csv.close();
    if (!artifacts.csv_written) {
        artifacts.message = "Could not finish writing surface_error_samples.csv.";
        return artifacts;
    }

    std::ofstream obj(artifacts.obj_path, std::ios::binary);
    if (!obj.good()) {
        artifacts.message = "Could not open surface_error_vectors.obj for writing.";
        return artifacts;
    }
    obj << std::setprecision(17);
    obj << "# NSR-1.3 over-tolerance source-to-projection error vectors\n";
    int vertex_index = 1;
    auto write_distance_vector = [&](const SurfaceRefitSampleDiagnostic& sample) {
        if (!sample.over_tolerance || !sample.projection_success) {
            return;
        }
        obj << "# sample_set=" << surfaceRefitSampleSetToString(sample.sample_set)
            << " category=" << surfaceRefitSampleCategoryToString(sample.category)
            << " distance=" << sample.distance << " face_id=";
        write_optional_id(obj, sample.face_id);
        obj << " edge_id=";
        write_optional_id(obj, sample.edge_id);
        obj << '\n';
        obj << "v " << sample.source_point.X() << ' '
            << sample.source_point.Y() << ' '
            << sample.source_point.Z() << '\n';
        obj << "v " << sample.projected_point.X() << ' '
            << sample.projected_point.Y() << ' '
            << sample.projected_point.Z() << '\n';
        obj << "l " << vertex_index << ' ' << vertex_index + 1 << '\n';
        vertex_index += 2;
    };
    for (const auto& sample : report.surface_error_samples) {
        write_distance_vector(sample);
    }
    for (const auto& sample : report.selection_error_samples) {
        write_distance_vector(sample);
    }
    obj.flush();
    artifacts.obj_written = obj.good();
    if (!artifacts.obj_written) {
        artifacts.message = "Could not finish writing surface_error_vectors.obj.";
        return artifacts;
    }

    std::ofstream normal_csv(artifacts.normal_csv_path, std::ios::binary);
    if (!normal_csv.good()) {
        artifacts.message = "Could not open surface_normal_error_samples.csv for writing.";
        return artifacts;
    }
    normal_csv << std::setprecision(17);
    normal_csv << "sample_set,source_x,source_y,source_z,projected_x,projected_y,projected_z,"
                  "source_normal_x,source_normal_y,source_normal_z,"
                  "fitted_normal_x,fitted_normal_y,fitted_normal_z,"
                  "angle_degrees,u,v,face_id,projection_success,fitted_normal_defined,over_tolerance\n";
    auto write_normal_sample = [&](const SurfaceRefitNormalSampleDiagnostic& sample) {
        if (!sample.over_tolerance) {
            return;
        }
        normal_csv << surfaceRefitSampleSetToString(sample.sample_set) << ','
                   << sample.source_point.X() << ','
                   << sample.source_point.Y() << ','
                   << sample.source_point.Z() << ',';
        if (sample.projection_success) {
            normal_csv << sample.projected_point.X() << ','
                       << sample.projected_point.Y() << ','
                       << sample.projected_point.Z();
        } else {
            normal_csv << ",,";
        }
        normal_csv << ',' << sample.source_normal.X()
                   << ',' << sample.source_normal.Y()
                   << ',' << sample.source_normal.Z() << ',';
        if (sample.fitted_normal_defined) {
            normal_csv << sample.fitted_normal.X() << ','
                       << sample.fitted_normal.Y() << ','
                       << sample.fitted_normal.Z() << ','
                       << sample.angle_degrees << ','
                       << sample.u << ','
                       << sample.v;
        } else {
            normal_csv << ",,,,,";
        }
        normal_csv << ',';
        write_optional_id(normal_csv, sample.face_id);
        normal_csv << ',' << (sample.projection_success ? "true" : "false")
                   << ',' << (sample.fitted_normal_defined ? "true" : "false")
                   << ',' << (sample.over_tolerance ? "true" : "false")
                   << '\n';
    };
    for (const auto& sample : report.normal_error_samples) {
        write_normal_sample(sample);
    }
    for (const auto& sample : report.selection_normal_error_samples) {
        write_normal_sample(sample);
    }
    normal_csv.flush();
    artifacts.normal_csv_written = normal_csv.good();
    if (!artifacts.normal_csv_written) {
        artifacts.message = "Could not finish writing surface_normal_error_samples.csv.";
        return artifacts;
    }

    artifacts.message = "Wrote selection/holdout distance CSV/OBJ diagnostics and over-tolerance normal CSV diagnostics.";
    return artifacts;
}

}
