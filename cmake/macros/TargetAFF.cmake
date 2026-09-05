# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

macro(TARGET_AFF)
    if (OVERTE_HEADLESS)
        return()
    endif()
    if(OVERTE_USE_SYSTEM_LIBS)
        find_path(AFF_INCLUDE_DIRS "artery-font/artery-font.h")
        target_include_directories(${TARGET_NAME} SYSTEM PRIVATE ${AFF_INCLUDE_DIRS})
    else()
        find_package(artery-font-format QUIET REQUIRED)
        target_link_libraries(${TARGET_NAME} artery-font-format::artery-font-format)
    endif()
endmacro()
