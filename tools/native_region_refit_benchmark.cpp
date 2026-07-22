#include "reduction/region_refit/JointBSplineNetworkFitter.h"
#include "reduction/region_refit/RegionPatchLayout.h"
#include "reduction/region_refit/RegionRefitResourceMonitor.h"

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string csvEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const auto character : value) {
        if (character == '"') escaped += "\"\"";
        else if (character == '\r' || character == '\n') escaped += ' ';
        else escaped += character;
    }
    return '"' + escaped + '"';
}

spo::RegionReferenceMesh makeWaveMesh(const int cells) {
    spo::RegionReferenceMesh mesh;
    const auto side = static_cast<std::size_t>(cells + 1);
    mesh.vertices.reserve(side * side);
    mesh.triangles.reserve(static_cast<std::size_t>(cells) * static_cast<std::size_t>(cells) * 2);
    const auto index = [side](const int column, const int row) {
        return static_cast<std::size_t>(row) * side + static_cast<std::size_t>(column);
    };
    for (int row = 0; row <= cells; ++row) {
        const auto y = static_cast<double>(row) / static_cast<double>(cells);
        for (int column = 0; column <= cells; ++column) {
            const auto x = static_cast<double>(column) / static_cast<double>(cells);
            const auto z = 0.1 * std::sin(2.0 * kPi * x) * std::sin(2.0 * kPi * y);
            const auto dzdx = 0.2 * kPi * std::cos(2.0 * kPi * x) * std::sin(2.0 * kPi * y);
            const auto dzdy = 0.2 * kPi * std::sin(2.0 * kPi * x) * std::cos(2.0 * kPi * y);
            spo::RegionReferenceVertex vertex;
            vertex.point = gp_Pnt(x, y, z);
            vertex.normal = gp_Dir(-dzdx, -dzdy, 1.0);
            vertex.boundary = row == 0 || row == cells || column == 0 || column == cells;
            vertex.hardBoundary = vertex.boundary;
            vertex.boundaryEndpoint = vertex.boundary &&
                ((row == 0 || row == cells) && (column == 0 || column == cells));
            spo::RegionReferenceSourceSample source;
            source.faceId = 0;
            source.uv = gp_Pnt2d(x, y);
            source.normal = vertex.normal;
            source.normalDefined = true;
            source.minimumCurvature = -0.1;
            source.maximumCurvature = 0.1;
            source.curvatureDefined = true;
            vertex.sources.push_back(source);
            mesh.vertices.push_back(std::move(vertex));
        }
    }
    for (int row = 0; row < cells; ++row) {
        for (int column = 0; column < cells; ++column) {
            const auto lowerLeft = index(column, row);
            const auto lowerRight = index(column + 1, row);
            const auto upperLeft = index(column, row + 1);
            const auto upperRight = index(column + 1, row + 1);
            mesh.triangles.push_back({{lowerLeft, lowerRight, upperRight}, 0});
            mesh.triangles.push_back({{lowerLeft, upperRight, upperLeft}, 0});
        }
    }
    return mesh;
}

double millisecondsSince(const std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "用法：native_region_refit_benchmark <输出 CSV> [单次拟合预算毫秒]\n";
        return 2;
    }
    const std::filesystem::path outputPath = argv[1];
    std::int64_t fitBudgetMilliseconds = 180000;
    if (argc == 3) {
        try {
            fitBudgetMilliseconds = std::stoll(argv[2]);
        } catch (...) {
            std::cerr << "单次拟合预算必须是正整数。\n";
            return 2;
        }
    }
    if (fitBudgetMilliseconds <= 0) {
        std::cerr << "单次拟合预算必须是正整数。\n";
        return 2;
    }
    std::error_code error;
    std::filesystem::create_directories(outputPath.parent_path(), error);
    if (error) {
        std::cerr << "无法创建基准输出目录。\n";
        return 1;
    }
    std::ofstream report(outputPath, std::ios::binary);
    if (!report) {
        std::cerr << "无法创建基准报告。\n";
        return 1;
    }
    report << "target_triangles,triangles,vertices,patch_count,layout_status,layout_wall_ms,layout_hash,"
              "proxy_error,layout_estimated_solver_bytes,fit_status,fit_wall_ms,fit_iterations,matrix_rows,"
              "matrix_columns,matrix_nonzeros,fit_estimated_solver_bytes,projection_count,uv_iterations,"
              "projection_fallbacks,u_controls,v_controls,holdout_max_distance,holdout_max_normal_degrees,"
              "boundary_max_distance,seam_max_distance,working_set_bytes,peak_working_set_bytes,private_bytes,"
              "peak_private_bytes,process_cpu_ms,stop_reason\n";

    const std::array<std::pair<int, int>, 3> scales {{{10000, 71}, {50000, 158}, {100000, 224}}};
    const std::array<int, 3> patchCounts {2, 4, 8};
    for (const auto& [targetTriangles, cells] : scales) {
        const auto mesh = makeWaveMesh(cells);
        for (const auto patchCount : patchCounts) {
            spo::RegionRefitOptions options;
            options.fixedPatchCount = patchCount;
            options.maximumParameterDistortion = 1000.0;
            options.maximumFitIterations = 1;
            options.maximumFullFitCount = 1;
            options.layoutBudgetMilliseconds = 300000;
            options.singleFitBudgetMilliseconds = fitBudgetMilliseconds;
            options.solverMemoryPreflightMiB = 7168;

            const auto before = spo::captureRegionProcessResources();
            const auto layoutStarted = std::chrono::steady_clock::now();
            const auto layouts = spo::RegionPatchLayoutGenerator().generate(
                mesh,
                spo::RegionRefitLayoutMode::FixedPatchCount,
                options);
            const auto layoutWall = millisecondsSince(layoutStarted);

            spo::JointBSplineNetworkFitResult fitted;
            double fitWall = 0.0;
            if (layouts.success()) {
                const auto fitStarted = std::chrono::steady_clock::now();
                fitted = spo::JointBSplineNetworkFitter().fit(
                    mesh,
                    {},
                    layouts.selectedCandidates,
                    options,
                    fitStarted + std::chrono::milliseconds(fitBudgetMilliseconds));
                fitWall = millisecondsSince(fitStarted);
            }
            const auto after = spo::captureRegionProcessResources();
            const auto* layout = layouts.selectedCandidates.empty()
                ? nullptr
                : &layouts.selectedCandidates.front();
            const spo::RegionFitAttemptReport* attempt = fitted.attempts.empty()
                ? nullptr
                : &fitted.attempts.front();
            const spo::RegionFitIterationReport* iteration =
                attempt == nullptr || attempt->iterations.empty()
                    ? nullptr
                    : &attempt->iterations.back();
            const auto stopReason = attempt != nullptr
                ? attempt->stopReason
                : (layouts.success() ? fitted.message : layouts.message);

            report << targetTriangles << ','
                   << mesh.triangles.size() << ','
                   << mesh.vertices.size() << ','
                   << patchCount << ','
                   << spo::toString(layouts.status) << ','
                   << layoutWall << ','
                   << csvEscape(layout == nullptr ? std::string() : layout->hash) << ','
                   << (layout == nullptr ? 0.0 : layout->proxyError) << ','
                   << (layout == nullptr ? 0 : layout->estimatedSolverBytes) << ','
                   << spo::toString(fitted.status) << ','
                   << fitWall << ','
                   << (attempt == nullptr ? 0 : attempt->iterations.size()) << ','
                   << (iteration == nullptr ? 0 : iteration->matrixRows) << ','
                   << (iteration == nullptr ? 0 : iteration->matrixColumns) << ','
                   << (iteration == nullptr ? 0 : iteration->matrixNonZeros) << ','
                   << (iteration == nullptr ? 0 : iteration->estimatedSolverBytes) << ','
                   << (iteration == nullptr ? 0 : iteration->projectionCount) << ','
                   << (iteration == nullptr ? 0 : iteration->uvIterationCount) << ','
                   << (iteration == nullptr ? 0 : iteration->projectionFallbackCount) << ','
                   << (iteration == nullptr ? 0 : iteration->uControlCount) << ','
                   << (iteration == nullptr ? 0 : iteration->vControlCount) << ','
                   << (iteration == nullptr ? 0.0 : iteration->holdoutMaximumDistance) << ','
                   << (iteration == nullptr ? 0.0 : iteration->holdoutMaximumNormalDegrees) << ','
                   << (iteration == nullptr ? 0.0 : iteration->boundaryMaximumDistance) << ','
                   << (iteration == nullptr ? 0.0 : iteration->seamMaximumDistance) << ','
                   << after.workingSetBytes << ','
                   << after.peakWorkingSetBytes << ','
                   << after.privateBytes << ','
                   << after.peakPrivateBytes << ','
                   << (after.processCpuMilliseconds - before.processCpuMilliseconds) << ','
                   << csvEscape(stopReason) << '\n';
            report.flush();
            std::cout << "triangles=" << mesh.triangles.size()
                      << " K=" << patchCount
                      << " layout_ms=" << layoutWall
                      << " fit_ms=" << fitWall
                      << " status=" << spo::toString(fitted.status) << '\n';
        }
    }
    return 0;
}
