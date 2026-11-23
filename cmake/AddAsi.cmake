# add_asi(<name> <sources...>)
#
# A shared library named <name>.asi that links the common library. It needs no
# exports: the ASI loader only calls LoadLibrary on it.
function(add_asi name)
    add_library(${name} SHARED ${ARGN})
    set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME ${name})
    target_link_libraries(${name} PRIVATE common)

    if(NOT MSVC)
        # A static MinGW link otherwise exports every symbol of libgcc and libstdc++.
        target_link_options(${name} PRIVATE -Wl,--exclude-all-symbols
            $<$<CONFIG:Release,MinSizeRel>:-s>)
    endif()
endfunction()
