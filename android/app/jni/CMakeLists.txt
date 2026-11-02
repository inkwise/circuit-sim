cmake_minimum_required(VERSION 3.22)

project(CircuitLab LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(CL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# Android 15 / Play requirement: every shared object must use 16 KB ELF
# segment alignment. NDK r29 defaults to this, but be explicit.
add_link_options(-Wl,-z,max-page-size=16384)

# Build SDL3 as a shared library. The Java half (org.libsdl.app) is compiled by
# Gradle from app/src/main/java; no SDL3.jar or test programs are needed.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_ANDROID_JAR OFF CACHE BOOL "" FORCE)
add_subdirectory("${CL_ROOT}/third_party/SDL3" "${CMAKE_BINARY_DIR}/SDL3")

add_subdirectory(src)
