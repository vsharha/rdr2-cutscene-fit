cmake_minimum_required(VERSION 3.21)

project(CutsceneFit VERSION 1.0.3 LANGUAGES C CXX)

option(CUTSCENEFIT_COMPARE "Build screenshot comparison controls into CutsceneFit" OFF)
option(CUTSCENEFIT_RELOAD "Build CutsceneFit as a DLL that tools/reload reloads while the game runs" OFF)

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

# Loads and reloads a reload build of the plugin while the game runs.
if(CUTSCENEFIT_RELOAD)
    add_subdirectory(tools/reload)
endif()

# The release zip holds the plugin, a sample settings file and the licences, at the
# zip root so that it extracts straight into the game folder. The research tools
# have no install rules, and a reload build has no release zip.
if(NOT CUTSCENEFIT_RELOAD)
    install(TARGETS CutsceneFit RUNTIME DESTINATION .)
    install(FILES dist/CutsceneFit.ini LICENSE THIRD-PARTY-NOTICES.txt DESTINATION .)

    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_FILE_NAME CutsceneFit-${PROJECT_VERSION})
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    include(CPack)
endif()
