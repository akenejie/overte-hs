# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

macro(TARGET_VHACD)
    if (OVERTE_HEADLESS)
        return()
    endif()
    find_package(v-hacd QUIET REQUIRED)
    target_link_libraries(${TARGET_NAME} v-hacd::v-hacd)
endmacro()
