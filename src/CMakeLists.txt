include(FetchContent)

# MinHook v1.3.4, pinned by commit. Its own CMakeLists names the library after the
# platform and adds install rules, so only the sources are fetched (SOURCE_SUBDIR
# points at a directory without a CMakeLists.txt) and built below.
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
    SOURCE_SUBDIR sources-only
)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook SYSTEM PUBLIC ${minhook_SOURCE_DIR}/include)
target_include_directories(minhook PRIVATE ${minhook_SOURCE_DIR}/src ${minhook_SOURCE_DIR}/src/hde)
# Third-party code: its warnings are not ours to fix. Nothing here applies to targets
# that link it.
if(MSVC)
    target_compile_options(minhook PRIVATE /W0)
else()
    target_compile_options(minhook PRIVATE -w)
endif()

add_asi(CutsceneFit
    bars.cpp
    dllmain.cpp
    game.cpp
    pattern.cpp
    screen.cpp
    settings.cpp
    state.cpp
    zoom.cpp)
if(CUTSCENEFIT_COMPARE)
    target_sources(CutsceneFit PRIVATE compare.cpp)
    target_compile_definitions(CutsceneFit PRIVATE CUTSCENEFIT_COMPARE)
endif()
if(CUTSCENEFIT_RELOAD)
    target_compile_definitions(CutsceneFit PRIVATE CUTSCENEFIT_RELOAD)
    # Not .asi, so that an ASI loader never loads it on its own; tools/reload does.
    set_target_properties(CutsceneFit PROPERTIES SUFFIX ".dll")
endif()
target_link_libraries(CutsceneFit PRIVATE minhook)
