cmake_minimum_required(VERSION 3.22)

project(CircuitLabTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(CL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(NGSPICE_INCLUDE "${CL_ROOT}/third_party/ngspice-47/src/include")
set(NGSPICE_LIB "${CL_ROOT}/android/app/src/main/jniLibs/${ANDROID_ABI}/libngspice.so")

if(NOT EXISTS "${NGSPICE_LIB}")
    message(FATAL_ERROR "libngspice.so not found at ${NGSPICE_LIB}")
endif()

add_library(ngspice SHARED IMPORTED)
set_target_properties(ngspice PROPERTIES
    IMPORTED_LOCATION "${NGSPICE_LIB}")

add_executable(engine_tests
    engine_tests.cpp
    "${CL_ROOT}/core/src/Circuit.cpp"
    "${CL_ROOT}/core/src/ComponentCatalog.cpp"
    "${CL_ROOT}/core/src/SpiceModels.cpp"
    "${CL_ROOT}/core/src/Netlist.cpp"
    "${CL_ROOT}/core/src/Flow.cpp"
    "${CL_ROOT}/core/src/Engineering.cpp"
    "${CL_ROOT}/core/src/Examples.cpp"
    "${CL_ROOT}/core/src/Measure.cpp"
    "${CL_ROOT}/core/src/NgspiceEngine.cpp"
    "${CL_ROOT}/core/src/ProjectIO.cpp")

target_include_directories(engine_tests PRIVATE
    "${CL_ROOT}/core/include"
    "${NGSPICE_INCLUDE}")
target_link_libraries(engine_tests PRIVATE ngspice)
target_link_options(engine_tests PRIVATE -Wl,-z,max-page-size=16384)
target_compile_options(engine_tests PRIVATE -Wall -Wextra)
