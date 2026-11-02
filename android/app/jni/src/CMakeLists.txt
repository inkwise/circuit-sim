cmake_minimum_required(VERSION 3.22)

get_filename_component(CL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(IMGUI_DIR "${CL_ROOT}/third_party/imgui")
set(NGSPICE_INCLUDE "${CL_ROOT}/third_party/ngspice-47/src/include")
set(NGSPICE_LIB "${CL_ROOT}/android/app/src/main/jniLibs/${ANDROID_ABI}/libngspice.so")

if(NOT EXISTS "${NGSPICE_LIB}")
    message(FATAL_ERROR
        "libngspice.so not found at ${NGSPICE_LIB}. Run scripts/build-ngspice-android.sh first.")
endif()

add_library(ngspice SHARED IMPORTED)
set_target_properties(ngspice PROPERTIES
    IMPORTED_LOCATION "${NGSPICE_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${NGSPICE_INCLUDE}")

add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(imgui PUBLIC
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends")
target_link_libraries(imgui PUBLIC SDL3::SDL3)
target_compile_options(imgui PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_library(core STATIC
    "${CL_ROOT}/core/src/Circuit.cpp"
    "${CL_ROOT}/core/src/ComponentCatalog.cpp"
    "${CL_ROOT}/core/src/SpiceModels.cpp"
    "${CL_ROOT}/core/src/Netlist.cpp"
    "${CL_ROOT}/core/src/Flow.cpp"
    "${CL_ROOT}/core/src/Engineering.cpp"
    "${CL_ROOT}/core/src/NgspiceEngine.cpp"
    "${CL_ROOT}/core/src/ProjectIO.cpp"
    "${CL_ROOT}/core/src/Examples.cpp"
    "${CL_ROOT}/core/src/Measure.cpp"
    "${CL_ROOT}/core/src/Thermoelectric.cpp"
    "${CL_ROOT}/core/src/Router.cpp")
target_include_directories(core PUBLIC "${CL_ROOT}/core/include")
target_link_libraries(core PUBLIC ngspice)
target_compile_options(core PRIVATE -Wall -Wextra)

add_library(main SHARED
    "${CL_ROOT}/ui/main.cpp"
    "${CL_ROOT}/ui/App.cpp"
    "${CL_ROOT}/ui/EditorUi.cpp"
    "${CL_ROOT}/ui/SchematicView.cpp"
    "${CL_ROOT}/ui/WaveformView.cpp"
    "${CL_ROOT}/ui/Theme.cpp")
target_include_directories(main PRIVATE "${CL_ROOT}/ui")
target_link_libraries(main PRIVATE core imgui SDL3::SDL3 android dl)
target_compile_options(main PRIVATE -Wall -Wextra)
