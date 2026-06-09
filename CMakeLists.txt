cmake_minimum_required(VERSION 3.25)

project(step_patch_optimizer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

option(SPO_BUILD_APP "Build the Qt desktop application" ON)
option(SPO_BUILD_TOOLS "Build command-line tools" ON)
option(SPO_BUILD_TESTS "Build tests" ON)

find_package(OpenCASCADE CONFIG REQUIRED)
find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Widgets OpenGLWidgets Concurrent)

set(SPO_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(spo_core STATIC
    src/app/AppController.cpp
    src/app/ProcessStatus.cpp
    src/brep/BoundaryWireBuilder.cpp
    src/brep/EdgeIndex.cpp
    src/brep/FaceIndex.cpp
    src/brep/ShapeDocument.cpp
    src/brep/SurfaceTypeProbe.cpp
    src/brep/TopologyGraph.cpp
    src/command/CommandHistory.cpp
    src/command/DetectFeatureCommand.cpp
    src/command/ExportStepCommand.cpp
    src/command/LoadStepCommand.cpp
    src/command/LockedEdgeRef.cpp
    src/command/LockEdgeCommand.cpp
    src/command/MergePatchCommand.cpp
    src/command/PatchReplacementCommand.cpp
    src/command/PlaneRegionBatchMergeCommand.cpp
    src/command/PlaneRegionMergeCommand.cpp
    src/command/SphereRegionBatchMergeCommand.cpp
    src/command/SphereRegionMergeCommand.cpp
    src/command/UnlockEdgeCommand.cpp
    src/command/ValidateShapeCommand.cpp
    src/feature/BoundaryClassifier.cpp
    src/feature/CurvatureEstimator.cpp
    src/feature/FeatureEdgeDetector.cpp
    src/feature/UserConstraintSet.cpp
    src/external/geomagic/GeomagicOutputPathResolver.cpp
    src/external/geomagic/GeomagicAutoSurfaceConfig.cpp
    src/external/geomagic/GeomagicAutoSurfaceResult.cpp
    src/external/geomagic/GeomagicAutoSurfaceBackend.cpp
    src/io/ProjectSerializer.cpp
    src/io/StepReader.cpp
    src/io/StepWriter.cpp
    src/io/StlReader.cpp
    src/io/StlWriter.cpp
    src/merge/FaceInspectInfo.cpp
    src/merge/FaceInspector.cpp
    src/merge/FeatureBoundedRegionBuilder.cpp
    src/merge/CandidateFilters.cpp
    src/merge/ConeRegionMerger.cpp
    src/merge/CylinderRegionMerger.cpp
    src/merge/MergeCandidate.cpp
    src/merge/MergePlanner.cpp
    src/merge/MergeRegionGrower.cpp
    src/merge/PlaneRegionMerger.cpp
    src/merge/RegionBoundaryAnalyzer.cpp
    src/merge/RegionMergeStub.cpp
    src/merge/SameDomainUnifier.cpp
    src/merge/SphereRegionMerger.cpp
    src/stl/StlCropReport.cpp
    src/stl/StlMesh.cpp
    src/stl/StlRegionExtractor.cpp
    src/merge/SurfaceRefitter.cpp
    src/merge/TorusRegionMerger.cpp
    src/patch/PatchImportService.cpp
    src/patch/PatchArtifactLocator.cpp
    src/patch/PatchPreviewReport.cpp
    src/patch/PatchApplyState.cpp
    src/patch/MultiFacePatchAnalyzer.cpp
    src/patch/BoundaryConstrainedPatchBuilder.cpp
    src/patch/BoundaryConstrainedMultiSurfaceShellBuilder.cpp
    src/patch/BoundaryConstrainedSurfaceRetrim.cpp
    src/patch/CropBoundaryDiagnostics.cpp
    src/patch/PatchReplacementRepair.cpp
    src/validate/ErrorMetric.cpp
    src/validate/ReportGenerator.cpp
    src/validate/SewingChecker.cpp
    src/validate/ShapeValidator.cpp
    src/validate/StrictTopologyGate.cpp
)

target_include_directories(spo_core PUBLIC "${SPO_INCLUDE_DIR}")

target_link_libraries(spo_core
    PUBLIC
        Qt6::Core
        TKDESTEP
        TKDEIGES
        TKXSBase
        TKernel
)

if(SPO_BUILD_APP)
    qt_standard_project_setup()

    qt_add_executable(step-patch-optimizer
        src/app/main.cpp
        src/app/MainWindow.cpp
        src/gui/InspectPanel.cpp
        src/gui/LogPanel.cpp
        src/gui/ModelTreePanel.cpp
        src/gui/OccViewWidget.cpp
        src/gui/ParameterPanel.cpp
        src/gui/ProcessStatusPanel.cpp
    )

    target_include_directories(step-patch-optimizer PRIVATE "${SPO_INCLUDE_DIR}")
    target_link_libraries(step-patch-optimizer
        PRIVATE
            spo_core
            Qt6::Concurrent
            Qt6::Widgets
            Qt6::OpenGLWidgets
            TKV3d
            TKOpenGl
            TKService
    )

    if(WIN32)
        set(SPO_QT_BIN_DIR "${CMAKE_TOOLCHAIN_FILE}/../../../installed/${VCPKG_TARGET_TRIPLET}/tools/Qt6/bin")
        get_filename_component(SPO_QT_BIN_DIR "${SPO_QT_BIN_DIR}" ABSOLUTE)

        add_custom_command(TARGET step-patch-optimizer POST_BUILD
            COMMAND "$<IF:$<CONFIG:Debug>,${SPO_QT_BIN_DIR}/windeployqt.debug.bat,${SPO_QT_BIN_DIR}/windeployqt.exe>"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-compiler-runtime
                    "$<TARGET_FILE:step-patch-optimizer>"
            COMMENT "Deploying Qt runtime plugins for step-patch-optimizer"
            VERBATIM
        )
    endif()
endif()

if(SPO_BUILD_TOOLS)
    add_executable(batch_optimize tools/batch_optimize.cpp)
    target_link_libraries(batch_optimize PRIVATE spo_core)

    add_executable(step_stats tools/step_stats.cpp)
    target_link_libraries(step_stats PRIVATE spo_core)
endif()

if(SPO_BUILD_TESTS)
    enable_testing()

    add_executable(spo_tests
        tests/test_feature_edges.cpp
        tests/test_feature_bounded_region_builder.cpp
        tests/test_face_inspect.cpp
        tests/test_analytic_candidate_detection.cpp
        tests/test_boundary_wire_builder.cpp
        tests/test_candidate_type_statistics.cpp
        tests/test_commands.cpp
        tests/test_merge_planner.cpp
        tests/test_plane_region_merge_command.cpp
        tests/test_plane_region_merger.cpp
        tests/test_region_boundary_analyzer.cpp
        tests/test_region_merge_stubs.cpp
        tests/test_sphere_region_merge_command.cpp
        tests/test_sphere_region_merger.cpp
        tests/test_same_domain_merge.cpp
        tests/test_step_io.cpp
        tests/test_stl_io.cpp
        tests/test_stl_mesh.cpp
        tests/test_stl_region_extractor.cpp
        tests/test_geomagic_output_path_resolver.cpp
        tests/test_geomagic_autosurface_config.cpp
        tests/test_geomagic_backend_mock.cpp
        tests/test_geomagic_pipeline_script.cpp
        tests/test_patch_import_service.cpp
        tests/test_patch_import_service_real.cpp
        tests/test_patch_artifact_locator.cpp
        tests/test_patch_preview_report.cpp
        tests/test_patch_apply_state.cpp
        tests/test_multiface_patch_analyzer.cpp
        tests/test_strict_topology_gate.cpp
        tests/test_boundary_constrained_patch_builder.cpp
        tests/test_crop_boundary_diagnostics.cpp
        tests/test_patch_replacement_repair.cpp
        tests/test_patch_replacement_command.cpp
        tests/test_topology_graph.cpp
        tests/test_validation.cpp
    )

    target_link_libraries(spo_tests PRIVATE spo_core)
    target_compile_definitions(spo_tests PRIVATE SPO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME spo_tests COMMAND spo_tests)
    set_tests_properties(spo_tests PROPERTIES TIMEOUT 120)
endif()
