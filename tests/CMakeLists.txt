# Tests that run on the build machine instead of in the game. Configure this directory
# on its own, with the host compiler:
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.21)

project(CutsceneFitTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(root ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(CUTSCENEFIT_DUMP ${root}/dumps/RDR2.dump.exe CACHE FILEPATH
    "Image written by dump.asi from the game version the patterns are for")

# The plugin's own pattern search and resolution, with the Windows parts of the
# common library replaced by signatures.cpp.
add_executable(signatures signatures.cpp ${root}/src/game.cpp ${root}/src/pattern.cpp)
target_include_directories(signatures PRIVATE ${root} ${root}/src)
if(MSVC)
    target_compile_options(signatures PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(signatures PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME signatures COMMAND signatures ${CUTSCENEFIT_DUMP})
# Without the dump the test is reported as skipped, not failed.
set_tests_properties(signatures PROPERTIES SKIP_RETURN_CODE 77)
