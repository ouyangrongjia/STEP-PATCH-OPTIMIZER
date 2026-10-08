// Generator only: export exactly the GUI candidate labels, plus unselected
// singleton faces to retain complete model coverage. No fitting occurs here.
#include "io/StepReader.h"
#include "feature/FeatureEdgeDetector.h"
#include "merge/MergePlanner.h"
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 6) {
        std::cerr << "input.step output_directory angle_degrees min_edge_length mesh_deflection_or_zero\n";
        return 2;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        const std::filesystem::path output(args[2].toStdWString());
        if (std::filesystem::exists(output)) throw std::runtime_error("Output already exists");
        std::filesystem::create_directories(output);
        const auto read = spo::StepReader().read(std::filesystem::path(args[1].toStdWString()));
        if (!read.status.success()) throw std::runtime_error(read.status.message());
        const auto& topology = read.document.topology();
        const auto features = spo::FeatureEdgeDetector().detect(topology, args[3].toDouble(), args[4].toDouble());
        spo::MergePlannerOptions options;
        options.min_feature_bounded_region_faces = 2;
        options.enable_feature_bounded_refit_candidates = true;
        const auto planned = spo::MergePlanner().plan(read.document, features, {}, options);
        std::vector<std::int32_t> labels(topology.faceCount(), -1);
        QJsonArray regions;
        for (const auto& candidate : planned.candidates) {
            QJsonArray faces, boundary;
            for (const auto face : candidate.faces) {
                if (labels[face] >= 0) throw std::runtime_error("Overlapping GUI candidates");
                labels[face] = candidate.candidate_id; faces.append(int(face));
            }
            for (const auto edge : candidate.boundary_edges) boundary.append(int(edge));
            regions.append(QJsonObject{{"region_id", candidate.candidate_id}, {"gui_candidate_id", candidate.candidate_id},
                                       {"face_ids", faces}, {"boundary_edge_ids", boundary}});
        }
        int singletonCount = 0;
        for (std::size_t face = 0; face < labels.size(); ++face) {
            if (labels[face] < 0) {
                labels[face] = int(regions.size()); ++singletonCount;
                regions.append(QJsonObject{{"region_id", labels[face]}, {"gui_candidate_id", QJsonValue::Null},
                                           {"face_ids", QJsonArray{int(face)}}, {"role", "unselected_singleton_retained"}});
            }
        }
        QJsonObject report{{"source", args[1]}, {"source_faces", int(labels.size())},
                           {"gui_candidates", int(planned.candidates.size())}, {"retained_singletons", singletonCount},
                           {"angle_degrees", args[3].toDouble()}, {"min_edge_length", args[4].toDouble()},
                           {"min_candidate_faces", 2}, {"locked_edges", QJsonArray{}}, {"regions", regions},
                           {"detector", "unmodified FeatureEdgeDetector + MergePlanner + FeatureBoundedRegionBuilder"}};
        const double deflection = args[5].toDouble();
        std::uint64_t count = 0;
        if (deflection > 0.) {
            BRepMesh_IncrementalMesh mesh(read.document.shape(), deflection, false, .1, true);
            mesh.Perform();
            std::ofstream raw(output / "triangles.bin", std::ios::binary);
            for (std::size_t fi = 0; fi < labels.size(); ++fi) {
                const auto& face = topology.face(fi); TopLoc_Location location;
                const auto triangulation = BRep_Tool::Triangulation(face, location);
                if (triangulation.IsNull()) throw std::runtime_error("A source face has no triangulation");
                for (int ti = 1; ti <= triangulation->NbTriangles(); ++ti) {
                    int a, b, c; triangulation->Triangle(ti).Get(a, b, c);
                    if (face.Orientation() == TopAbs_REVERSED) std::swap(b, c);
                    std::array<double, 9> xyz;
                    int k = 0;
                    for (const int index : {a, b, c}) {
                        const auto p = triangulation->Node(index).Transformed(location.Transformation());
                        xyz[k++] = p.X(); xyz[k++] = p.Y(); xyz[k++] = p.Z();
                    }
                    const std::int32_t faceId = std::int32_t(fi);
                    raw.write(reinterpret_cast<const char*>(xyz.data()), sizeof(xyz));
                    raw.write(reinterpret_cast<const char*>(&labels[fi]), sizeof(labels[fi]));
                    raw.write(reinterpret_cast<const char*>(&faceId), sizeof(faceId));
                    ++count;
                }
            }
            if (!raw) throw std::runtime_error("Triangle export failed");
        }
        report["triangles"] = double(count); report["mesh_deflection_mm"] = deflection;
        report["angular_deflection_radians"] = .1;
        report["generator_uses_CAD"] = true;
        report["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        const auto bytes = QJsonDocument(report).toJson();
        std::ofstream manifest(output / "partition.json", std::ios::binary);
        manifest.write(bytes.constData(), bytes.size());
        if (!manifest) throw std::runtime_error("Manifest write failed");
        std::cout << planned.candidates.size() << " GUI candidates, " << singletonCount << " retained singleton faces, " << count << " triangles\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
