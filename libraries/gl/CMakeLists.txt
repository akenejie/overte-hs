set(TARGET_NAME gl)

if (OVERTE_HEADLESS)
    # Minimal stub for headless builds - no OpenGL
    add_library(${TARGET_NAME} empty.cpp)
    set_target_properties(${TARGET_NAME} PROPERTIES LINKER_LANGUAGE CXX)
    return()
endif()

setup_hifi_library(Gui Widgets)
link_hifi_libraries(shared)

set(OpenGL_GL_PREFERENCE "GLVND")
target_opengl()
target_glad()

# support EGL on Linux for Wayland
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_egl()
endif()
