cmake_minimum_required(VERSION 3.21)

project(CutsceneFit LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Build for Windows: pass -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "RDR2 is x64 only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The game loads plugins without the Visual C++ redistributable. MinGW gets its
# static runtime from the toolchain file.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(AddAsi)

add_subdirectory(common)

# Research tools; each builds its own .asi.
add_subdirectory(tools/dump)
add_subdirectory(tools/watch)
add_subdirectory(tools/zoom)

# The plugin itself.
add_subdirectory(src)
