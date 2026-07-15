#include "feature/FeatureEdgeDetector.h"
#include "io/StepReader.h"
#include "io/StepWriter.h"
#include "io/StlReader.h"
#include "merge/MergePlanner.h"
#include "reduction/SurfaceReductionEngine.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "用法：surface_reduction_probe <输入.step> <输出.step> "
                     "[--source-stl <原始.stl> | --cropped-stl <选区裁剪.stl>] "
                     "[--target-ratio <0到1>] "
                     "(<面编号> <面编号> [...] | --candidate-id <候选编号>)\n";
        return 2;
    }
    const std::filesystem::path input = argv[1];
    const std::filesystem::path output = argv[2];
    const auto read = spo::StepReader().read(input);
    if (!read.status.success()) {
        std::cerr << read.status.message() << '\n';
        return 1;
    }

    spo::SurfaceReductionRequest request;
    int candidateId = -1;
    std::filesystem::path sourceStl;
    bool sourceStlIsSelectionCrop = false;
    double targetRatio = -1.0;
    try {
        for (int index = 3; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--source-stl" || argument == "--cropped-stl") {
                if (++index >= argc || !sourceStl.empty()) {
                    std::cerr << "只能指定一个原始 STL 或选区裁剪 STL 路径。\n";
                    return 2;
                }
                sourceStl = argv[index];
                sourceStlIsSelectionCrop = argument == "--cropped-stl";
            } else if (argument == "--target-ratio") {
                if (++index >= argc || targetRatio > 0.0) {
                    std::cerr << "--target-ratio 必须且只能指定一次。\n";
                    return 2;
                }
                targetRatio = std::stod(argv[index]);
            } else if (argument == "--candidate-id") {
                if (++index >= argc || candidateId >= 0) {
                    std::cerr << "--candidate-id 必须且只能指定一个候选编号。\n";
                    return 2;
                }
                candidateId = std::stoi(argv[index]);
            } else {
                request.selectedFaces.push_back(static_cast<spo::FaceId>(std::stoull(argument)));
            }
        }
    } catch (...) {
        std::cerr << "候选编号和面编号必须是非负整数。\n";
        return 2;
    }
    if (candidateId < -1 || (candidateId >= 0 && !request.selectedFaces.empty())) {
        std::cerr << "候选编号必须是非负整数，且不能与显式面编号同时使用。\n";
        return 2;
    }
    if (candidateId >= 0) {
        spo::MergePlannerOptions plannerOptions;
        plannerOptions.enable_feature_bounded_refit_candidates = true;
        plannerOptions.min_feature_bounded_region_faces = 2;
        const auto features = spo::FeatureEdgeDetector().detect(read.document.topology(), 25.0, 0.0);
        const auto planned = spo::MergePlanner().plan(read.document, features, {}, plannerOptions);
        const auto candidate = std::find_if(
            planned.candidates.begin(), planned.candidates.end(),
            [&](const auto& value) { return value.candidate_id == candidateId; });
        if (candidate == planned.candidates.end()) {
            std::cerr << "没有找到指定候选区域。\n";
            return 1;
        }
        request.selectedFaces = candidate->faces;
    }
    if (request.selectedFaces.size() < 2) {
        std::cerr << "至少需要两个面或一个有效候选区域。\n";
        return 2;
    }
    if (targetRatio > 0.0) {
        if (!(targetRatio < 1.0)) {
            std::cerr << "目标比例必须大于0且小于1。\n";
            return 2;
        }
        request.options.targetOutputRatio = targetRatio;
    }
    if (!sourceStl.empty()) {
        auto stlRead = spo::StlReader().read(sourceStl);
        if (!stlRead.success) {
            std::cerr << stlRead.message << '\n';
            return 1;
        }
        request.referenceStl = std::make_shared<spo::StlMesh>(std::move(stlRead.mesh));
        request.referenceStlIsSelectionCrop = sourceStlIsSelectionCrop;
    }

    std::cout << "candidate_id=" << candidateId << '\n'
              << "selected_faces=";
    for (std::size_t index = 0; index < request.selectedFaces.size(); ++index) {
        std::cout << (index == 0 ? "" : ";") << request.selectedFaces[index];
    }
    std::cout << '\n'
              << "source_stl=" << sourceStl.string() << '\n'
              << "source_stl_is_selection_crop=" << sourceStlIsSelectionCrop << '\n'
              << "target_ratio=" << request.options.targetOutputRatio << '\n'
              << std::flush;

    const auto result = spo::SurfaceReductionEngine().reduce(read.document, request);
    std::cout << "outcome=" << spo::toString(result.report.outcome) << '\n'
              << "input_faces=" << result.report.inputFaceCount << '\n'
              << "output_faces=" << result.report.outputFaceCount << '\n'
              << "reduction_ratio=" << result.report.reductionRatio << '\n'
              << "max_distance=" << result.report.maxDistance << '\n'
              << "max_normal_degrees=" << result.report.maxNormalAngleDegrees << '\n'
              << "max_smooth_seam_degrees=" << result.report.maxSmoothSeamAngleDegrees << '\n'
              << "parameterization_valid=" << result.report.parameterizationValid << '\n'
              << "brep_check_valid=" << result.report.topologyReport.afterBRepCheckValid << '\n'
              << "step_roundtrip_ok=" << result.report.topologyReport.stepRoundtripOk << '\n'
              << "commercial_cad_gate_count=" << result.report.commercialCadReports.size() << '\n'
              << "attempted_merges=" << result.report.attemptedMergeCount << '\n'
              << "accepted_merges=" << result.report.acceptedMergeCount << '\n'
              << "hybrid_parameterization_used=" << result.report.hybridParameterizationUsed << '\n'
              << "hybrid_source_triangles=" << result.report.hybridParameterization.sourceTriangleCount << '\n'
              << "hybrid_coarse_triangles=" << result.report.hybridParameterization.coarseTriangleCount << '\n'
              << "hybrid_cropped_triangles=" << result.report.hybridParameterization.croppedTriangleCount << '\n'
              << "hybrid_parameter_vertices=" << result.report.hybridParameterization.vertexCount << '\n'
              << "hybrid_boundary_vertices=" << result.report.hybridParameterization.boundaryVertexCount << '\n'
              << "hybrid_min_jacobian=" << result.report.hybridParameterization.minimumNormalizedJacobian << '\n'
              << "stl_reference_samples=" << result.report.stlReferenceSampleCount << '\n'
              << "max_step_to_stl_distance=" << result.report.maxStepToStlDistance << '\n'
              << "hybrid_message=" << result.report.hybridParameterization.message << '\n'
              << "rejected_groups=" << result.report.rejectedGroups.size() << '\n'
              << "message=" << result.report.message << '\n';
    std::cout << "topology_failure_reason=" << spo::toString(result.report.topologyReport.failureReason) << '\n'
              << "topology_message=" << result.report.topologyReport.message << '\n'
              << "before_free_edges=" << result.report.topologyReport.beforeFreeEdges << '\n'
              << "after_free_edges=" << result.report.topologyReport.afterFreeEdges << '\n'
              << "before_multiple_edges=" << result.report.topologyReport.beforeMultipleEdges << '\n'
              << "after_multiple_edges=" << result.report.topologyReport.afterMultipleEdges << '\n';
    for (std::size_t index = 0; index < result.report.outputGroups.size(); ++index) {
        if (result.report.outputGroups[index].size() < 2) {
            continue;
        }
        std::cout << "output_group_" << index << "_faces=";
        for (std::size_t faceIndex = 0; faceIndex < result.report.outputGroups[index].size(); ++faceIndex) {
            std::cout << (faceIndex == 0 ? "" : ";") << result.report.outputGroups[index][faceIndex];
        }
        std::cout << '\n';
    }
    for (std::size_t index = 0; index < result.report.rejectedGroups.size(); ++index) {
        const auto& rejected = result.report.rejectedGroups[index];
        std::cout << "rejection_" << index << "_faces=";
        for (std::size_t faceIndex = 0; faceIndex < rejected.faces.size(); ++faceIndex) {
            std::cout << (faceIndex == 0 ? "" : ";") << rejected.faces[faceIndex];
        }
        std::cout << '\n'
                  << "rejection_" << index << "_reason=" << rejected.reason << '\n';
    }
    if (!result.canApply()) {
        return 1;
    }
    const spo::ShapeDocument reduced(result.reducedShape, input);
    const auto write = spo::StepWriter().write(reduced, output);
    if (!write.success()) {
        std::cerr << write.message() << '\n';
        return 1;
    }
    return 0;
}
