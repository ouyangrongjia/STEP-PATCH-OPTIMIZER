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
    csv << "category,source_x,source_y,source_z,projected_x,projected_y,projected_z,"
           "distance,projection_success,over_tolerance,face_id,edge_id\n";
    for (const auto& sample : report.surface_error_samples) {
        csv << surfaceRefitSampleCategoryToString(sample.category) << ','
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
        csv << '\n';
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
    obj << "# NSR-1.1 over-tolerance source-to-projection error vectors\n";
    int vertex_index = 1;
    for (const auto& sample : report.surface_error_samples) {
        if (!sample.over_tolerance || !sample.projection_success) {
            continue;
        }
        obj << "# category=" << surfaceRefitSampleCategoryToString(sample.category)
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
    }
    obj.flush();
    artifacts.obj_written = obj.good();
    if (!artifacts.obj_written) {
        artifacts.message = "Could not finish writing surface_error_vectors.obj.";
        return artifacts;
    }

    artifacts.message = "Wrote numeric CSV diagnostics and over-tolerance OBJ vectors.";
    return artifacts;
}

}
