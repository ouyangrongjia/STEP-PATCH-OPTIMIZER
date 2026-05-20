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
find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Widgets OpenGLWidgets)

set(SPO_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(spo_core STATIC
    src/app/AppController.cpp
    src/brep/EdgeIndex.cpp
    src/brep/FaceIndex.cpp
    src/brep/ShapeDocument.cpp
    src/brep/TopologyGraph.cpp
    src/command/CommandHistory.cpp
    src/command/DetectFeatureCommand.cpp
    src/command/ExportStepCommand.cpp
    src/command/LoadStepCommand.cpp
    src/command/LockedEdgeRef.cpp
    src/command/LockEdgeCommand.cpp
    src/command/MergePatchCommand.cpp
    src/command/UnlockEdgeCommand.cpp
    src/command/ValidateShapeCommand.cpp
    src/feature/BoundaryClassifier.cpp
    src/feature/CurvatureEstimator.cpp
    src/feature/FeatureEdgeDetector.cpp
    src/feature/UserConstraintSet.cpp
    src/io/ProjectSerializer.cpp
    src/io/StepReader.cpp
    src/io/StepWriter.cpp
    src/merge/MergeCandidate.cpp
    src/merge/MergePlanner.cpp
    src/merge/MergeRegionGrower.cpp
    src/merge/SameDomainUnifier.cpp
    src/merge/SurfaceRefitter.cpp
    src/validate/ErrorMetric.cpp
    src/validate/ReportGenerator.cpp
    src/validate/SewingChecker.cpp
    src/validate/ShapeValidator.cpp
)

target_include_directories(spo_core PUBLIC "${SPO_INCLUDE_DIR}")

target_link_libraries(spo_core
    PUBLIC
        Qt6::Core
        TKDESTEP
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
    )

    target_include_directories(step-patch-optimizer PRIVATE "${SPO_INCLUDE_DIR}")
    target_link_libraries(step-patch-optimizer
        PRIVATE
            spo_core
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
        tests/test_commands.cpp
        tests/test_same_domain_merge.cpp
        tests/test_step_io.cpp
        tests/test_topology_graph.cpp
        tests/test_validation.cpp
    )

    target_link_libraries(spo_tests PRIVATE spo_core)
    add_test(NAME spo_tests COMMAND spo_tests)
endif()
