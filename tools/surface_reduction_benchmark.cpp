#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "merge/MergePlanner.h"
#include "reduction/SurfaceReductionEngine.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

std::string csvEscape(std::string value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char character : value) {
        if (character == '"') {
            escaped += "\"\"";
        } else if (character == '\r' || character == '\n') {
            escaped += ' ';
        } else {
            escaped += character;
        }
    }
    return '"' + escaped + '"';
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.c_str()), utf8.size()};
}

std::string faceIdsText(const std::vector<spo::FaceId>& faces) {
    std::string output;
    for (const auto face : faces) {
        if (!output.empty()) {
            output += ';';
        }
        output += std::to_string(face);
    }
    return output;
}

std::string outputGroupsText(const std::vector<std::vector<spo::FaceId>>& groups) {
    std::string output;
    for (const auto& group : groups) {
        if (!output.empty()) {
            output += '|';
        }
        output += faceIdsText(group);
    }
    return output;
}

struct BenchmarkCandidate {
    std::vector<spo::FaceId> faces;
    bool exactSameDomain = false;
};

bool isExactlyMergeable(
    const spo::ShapeDocument& document,
    const std::vector<spo::FaceId>& faces) {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const auto face : faces) {
        builder.Add(compound, document.topology().face(face));
    }
    try {
        ShapeUpgrade_UnifySameDomain unifier(
            compound,
            Standard_True,
            Standard_True,
            Standard_True);
        unifier.Build();
        int faceCount = 0;
        TopoDS_Face merged;
        for (TopExp_Explorer explorer(unifier.Shape(), TopAbs_FACE); explorer.More(); explorer.Next()) {
            merged = TopoDS::Face(explorer.Current());
            ++faceCount;
        }
        return faceCount == 1 &&
            !merged.IsNull() &&
            BRepCheck_Analyzer(merged, Standard_True).IsValid();
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "用法：surface_reduction_benchmark <输出目录> <最少区域数> <STEP 文件> [...]\n";
        return 2;
    }
    const std::filesystem::path outputDirectory = argv[1];
    int requiredRegionCount = 0;
    try {
        requiredRegionCount = std::stoi(argv[2]);
    } catch (...) {
        std::cerr << "最少区域数必须是正整数。\n";
        return 2;
    }
    if (requiredRegionCount < 1) {
        std::cerr << "最少区域数必须是正整数。\n";
        return 2;
    }
    std::error_code error;
    std::filesystem::create_directories(outputDirectory, error);
    if (error) {
        std::cerr << "无法创建输出目录。\n";
        return 1;
    }
    std::ofstream report(outputDirectory / "surface_reduction_benchmark.csv", std::ios::binary);
    if (!report) {
        std::cerr << "无法创建基准报告。\n";
        return 1;
    }
    report << "model,region,selected_faces,output_groups,input_faces,output_faces,outcome,reduction_ratio,target_achieved,"
              "max_distance,max_normal_degrees,max_smooth_seam_degrees,parameterization_valid,"
              "brep_check_valid,step_roundtrip_ok,commercial_cad_gate_passed,"
              "rejected_groups,first_rejection,message\n";

    int regionCount = 0;
    int targetAchievedCount = 0;
    int reducedCount = 0;
    int noReductionCount = 0;
    const int inputFileCount = argc - 3;
    const int perModelLimit = std::max(1, (requiredRegionCount + inputFileCount - 1) / inputFileCount);
    for (int argumentIndex = 3;
         argumentIndex < argc && regionCount < requiredRegionCount;
         ++argumentIndex) {
        const std::filesystem::path sourcePath = argv[argumentIndex];
        const auto read = spo::StepReader().read(sourcePath);
        if (!read.status.success()) {
            std::cerr << pathUtf8(sourcePath) << "：" << read.status.message() << '\n';
            continue;
        }
        const auto features = spo::FeatureEdgeDetector().detect(read.document.topology(), 5.0, 0.0);
        spo::MergePlannerOptions plannerOptions;
        plannerOptions.min_feature_bounded_region_faces = 2;
        const auto planned = spo::MergePlanner().plan(read.document, features, {}, plannerOptions);
        std::set<spo::EdgeId> protectedEdges;
        for (const auto& edge : features.edges) {
            protectedEdges.insert(edge.edge);
        }
        std::set<std::vector<spo::FaceId>> discoveredFaces;
        for (spo::EdgeId edge = 0; edge < read.document.topology().edgeCount(); ++edge) {
            if (protectedEdges.contains(edge)) {
                continue;
            }
            const auto* adjacency = read.document.topology().adjacencyForEdge(edge);
            if (adjacency == nullptr || adjacency->faces.size() != 2) {
                continue;
            }
            auto pair = adjacency->faces;
            std::sort(pair.begin(), pair.end());
            discoveredFaces.insert(std::move(pair));
        }
        for (const auto& candidate : planned.candidates) {
            if (candidate.valid && candidate.faces.size() >= 2 && candidate.faces.size() <= 20) {
                auto faces = candidate.faces;
                std::sort(faces.begin(), faces.end());
                discoveredFaces.insert(std::move(faces));
            }
        }
        std::vector<BenchmarkCandidate> candidates;
        candidates.reserve(discoveredFaces.size());
        for (const auto& faces : discoveredFaces) {
            candidates.push_back({faces, isExactlyMergeable(read.document, faces)});
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
            return std::make_tuple(!lhs.exactSameDomain, lhs.faces.size(), lhs.faces) <
                std::make_tuple(!rhs.exactSameDomain, rhs.faces.size(), rhs.faces);
        });

        int modelRegionCount = 0;
        for (const auto& candidate : candidates) {
            if (regionCount >= requiredRegionCount || modelRegionCount >= perModelLimit) {
                break;
            }
            spo::SurfaceReductionRequest request;
            request.selectedFaces = candidate.faces;
            request.options.searchObjective =
                spo::SurfaceReductionSearchObjective::TargetRatio;
            const auto result = spo::SurfaceReductionEngine().reduce(read.document, request);
            ++regionCount;
            ++modelRegionCount;
            targetAchievedCount += result.report.targetAchieved && result.canApply() ? 1 : 0;
            reducedCount += result.canApply() ? 1 : 0;
            noReductionCount += result.report.outcome == spo::SurfaceReductionOutcome::NoAcceptableReduction ? 1 : 0;
            const bool commercialCadGatePassed = !result.report.commercialCadReports.empty() &&
                std::all_of(
                    result.report.commercialCadReports.begin(),
                    result.report.commercialCadReports.end(),
                    [](const auto& gate) { return gate.passed; });
            report << csvEscape(pathUtf8(sourcePath)) << ','
                   << regionCount << ','
                   << csvEscape(faceIdsText(request.selectedFaces)) << ','
                   << csvEscape(outputGroupsText(result.report.outputGroups)) << ','
                   << result.report.inputFaceCount << ','
                   << result.report.outputFaceCount << ','
                   << spo::toString(result.report.outcome) << ','
                   << result.report.reductionRatio << ','
                   << (result.report.targetAchieved ? "true" : "false") << ','
                   << result.report.maxDistance << ','
                   << result.report.maxNormalAngleDegrees << ','
                   << result.report.maxSmoothSeamAngleDegrees << ','
                   << (result.report.parameterizationValid ? "true" : "false") << ','
                   << (result.report.topologyReport.afterBRepCheckValid ? "true" : "false") << ','
                   << (result.report.topologyReport.stepRoundtripOk ? "true" : "false") << ','
                   << (commercialCadGatePassed ? "true" : "false") << ','
                   << result.report.rejectedGroups.size() << ','
                   << csvEscape(result.report.rejectedGroups.empty()
                        ? std::string()
                        : result.report.rejectedGroups.front().reason) << ','
                   << csvEscape(result.report.message) << '\n';
            report.flush();

            if (result.canApply()) {
                auto filename = sourcePath.stem();
                filename += "_surface_reduction_" + std::to_string(regionCount) + ".stp";
                const spo::ShapeDocument reduced(result.reducedShape, sourcePath);
                const auto write = spo::StepWriter().write(reduced, outputDirectory / filename);
                if (!write.success()) {
                    std::cerr << "区域 " << regionCount << " 输出失败：" << write.message() << '\n';
                }
            }
            std::cout << "region=" << regionCount
                      << " model=" << pathUtf8(sourcePath)
                      << " input=" << result.report.inputFaceCount
                      << " output=" << result.report.outputFaceCount
                      << " outcome=" << spo::toString(result.report.outcome) << '\n';
        }
    }

    std::cout << "regions=" << regionCount
              << " reduced=" << reducedCount
              << " target_achieved=" << targetAchievedCount
              << " no_reduction=" << noReductionCount << '\n';
    return regionCount >= requiredRegionCount ? 0 : 3;
}
