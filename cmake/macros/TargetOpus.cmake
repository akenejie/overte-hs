#
#  Created by Michael Bailey on 12/20/2019
#  Copyright 2019 Michael Bailey
#  Copyright 2026 Overte e.V.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#

#
# overte-hs modifications:
# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
# (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

macro(TARGET_opus)
    if (OVERTE_HEADLESS)
        return()
    endif()
    if(OVERTE_USE_SYSTEM_LIBS)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(Opus REQUIRED opus)
        target_include_directories(${TARGET_NAME} SYSTEM PRIVATE ${Opus_INCLUDE_DIRS})
        target_link_libraries(${TARGET_NAME} ${Opus_LINK_LIBRARIES})
    else()
        find_package(Opus QUIET REQUIRED)
        target_link_libraries(${TARGET_NAME} Opus::opus)
    endif()
endmacro()
