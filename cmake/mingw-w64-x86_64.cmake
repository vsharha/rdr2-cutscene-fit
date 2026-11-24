set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
# Debian-based distributions ship the compilers per thread model, as
# x86_64-w64-mingw32-g++-posix and -win32. Only the posix one has std::thread and
# std::mutex; pass -DCUTSCENEFIT_MINGW_SUFFIX=-posix for those.
set(CUTSCENEFIT_MINGW_SUFFIX "" CACHE STRING "Suffix of the MinGW-w64 compiler names")
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc${CUTSCENEFIT_MINGW_SUFFIX})
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++${CUTSCENEFIT_MINGW_SUFFIX})
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# The game loads the plugin without the MinGW runtime DLLs next to it.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
