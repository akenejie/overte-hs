# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

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
