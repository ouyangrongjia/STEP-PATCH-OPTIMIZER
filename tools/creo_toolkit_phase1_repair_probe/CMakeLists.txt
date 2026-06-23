cmake_minimum_required(VERSION 3.25)

project(creo_toolkit_phase1_repair_probe LANGUAGES CXX)

if(NOT DEFINED CREO_ROOT OR CREO_ROOT STREQUAL "")
    message(FATAL_ERROR "Pass -DCREO_ROOT=<Creo install root>, for example E:/Proe/Creo 11.0.0.0")
endif()

file(TO_CMAKE_PATH "${CREO_ROOT}" CREO_ROOT_CMAKE)
set(CREO_COMMON_FILES "${CREO_ROOT_CMAKE}/Common Files")
set(CREO_TOOLKIT_DIR "${CREO_COMMON_FILES}/protoolkit")
set(CREO_TOOLKIT_INCLUDE_DIR "${CREO_TOOLKIT_DIR}/includes")
set(CREO_TOOLKIT_LIB_DIR "${CREO_TOOLKIT_DIR}/x86e_win64/obj")

if(NOT EXISTS "${CREO_TOOLKIT_INCLUDE_DIR}/ProToolkit.h")
    message(FATAL_ERROR "ProToolkit.h was not found under ${CREO_TOOLKIT_INCLUDE_DIR}")
endif()

set(CREO_TOOLKIT_LIBRARY "${CREO_TOOLKIT_LIB_DIR}/protkmd_NU.lib")
if(NOT EXISTS "${CREO_TOOLKIT_LIBRARY}")
    set(CREO_TOOLKIT_LIBRARY "${CREO_TOOLKIT_LIB_DIR}/protk_dllmd_NU.lib")
endif()
if(NOT EXISTS "${CREO_TOOLKIT_LIBRARY}")
    set(CREO_TOOLKIT_LIBRARY "${CREO_TOOLKIT_LIB_DIR}/protoolkit_NU.lib")
endif()
if(NOT EXISTS "${CREO_TOOLKIT_LIBRARY}")
    message(FATAL_ERROR "No supported Pro/TOOLKIT import library was found under ${CREO_TOOLKIT_LIB_DIR}")
endif()

add_executable(creo_toolkit_phase1_repair_probe creo_toolkit_phase1_repair_probe.cpp)
target_compile_features(creo_toolkit_phase1_repair_probe PRIVATE cxx_std_17)
target_include_directories(creo_toolkit_phase1_repair_probe PRIVATE "${CREO_TOOLKIT_INCLUDE_DIR}")
target_link_libraries(
    creo_toolkit_phase1_repair_probe
    PRIVATE
        "${CREO_TOOLKIT_LIBRARY}"
        "${CREO_TOOLKIT_LIB_DIR}/ucore.lib"
        "${CREO_TOOLKIT_LIB_DIR}/udata.lib"
        ws2_32
        mpr
        netapi32)

if(MSVC)
    set_property(TARGET creo_toolkit_phase1_repair_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    target_compile_definitions(creo_toolkit_phase1_repair_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
