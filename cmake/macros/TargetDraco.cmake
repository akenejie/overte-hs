# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

macro(TARGET_DRACO)
    if (OVERTE_HEADLESS)
        return()
    endif()
    if (OVERTE_USE_SYSTEM_LIBS)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(Draco REQUIRED draco)
        target_include_directories(${TARGET_NAME} SYSTEM PUBLIC ${Draco_INCLUDE_DIRS})
        target_link_libraries(${TARGET_NAME} ${Draco_LINK_LIBRARIES})
    elseif (ANDROID)
        set(INSTALL_DIR ${HIFI_ANDROID_PRECOMPILED}/draco)
        set(DRACO_INCLUDE_DIRS "${INSTALL_DIR}/include" CACHE STRING INTERNAL)
        set(LIB_DIR ${INSTALL_DIR}/lib)
        list(APPEND DRACO_LIBRARIES ${LIB_DIR}/libdraco.a)
        list(APPEND DRACO_LIBRARIES ${LIB_DIR}/libdracodec.a)
        list(APPEND DRACO_LIBRARIES ${LIB_DIR}/libdracoenc.a)
        target_link_libraries(${TARGET_NAME} ${DRACO_LIBRARIES})
    else()
        find_package(draco QUIET REQUIRED)
        target_link_libraries(${TARGET_NAME} draco::draco)
    endif()
endmacro()
