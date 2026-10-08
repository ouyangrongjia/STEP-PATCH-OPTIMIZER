cmake_minimum_required(VERSION 3.25)
project(gui_partition_export LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/utf-8 /FS)
endif()
find_package(OpenCASCADE CONFIG REQUIRED)
find_package(Qt6 CONFIG REQUIRED COMPONENTS Core)
get_filename_component(SPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(gui_partition_export
    export.cpp
    ${SPO_ROOT}/src/io/StepReader.cpp
    ${SPO_ROOT}/src/brep/ShapeDocument.cpp
    ${SPO_ROOT}/src/brep/FaceIndex.cpp
    ${SPO_ROOT}/src/brep/EdgeIndex.cpp
    ${SPO_ROOT}/src/brep/TopologyGraph.cpp
    ${SPO_ROOT}/src/feature/FeatureEdgeDetector.cpp
    ${SPO_ROOT}/src/merge/FeatureBoundedRegionBuilder.cpp
    ${SPO_ROOT}/src/merge/MergeCandidate.cpp
    ${SPO_ROOT}/src/merge/MergePlanner.cpp)
target_include_directories(gui_partition_export PRIVATE ${SPO_ROOT}/src)
target_link_libraries(gui_partition_export PRIVATE Qt6::Core TKDESTEP TKMesh TKXSBase TKernel)
