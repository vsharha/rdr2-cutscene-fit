add_library(common STATIC
    log.cpp
    mem.cpp
    paths.cpp)

# Headers are included as "common/<name>.h".
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(common PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(common PUBLIC version)

if(MSVC)
    target_compile_options(common PUBLIC /W4 /permissive- /utf-8)
else()
    target_compile_options(common PUBLIC -Wall -Wextra)
endif()
