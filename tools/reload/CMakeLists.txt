add_asi(CutsceneFitReload dllmain.cpp)

# The default payload is the plugin this build writes. Under Wine, and so Proton, the
# Z: drive maps the Linux root directory.
if(CMAKE_HOST_UNIX)
    set(payload_drive "Z:")
endif()
target_compile_definitions(CutsceneFitReload PRIVATE
    "CUTSCENEFIT_RELOAD_PAYLOAD=\"${payload_drive}$<TARGET_FILE:CutsceneFit>\"")
add_dependencies(CutsceneFitReload CutsceneFit)
