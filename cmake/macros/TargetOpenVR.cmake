#
#  Created by Bradley Austin Davis on 2018/10/24
#  Copyright 2013-2018 High Fidelity, Inc.
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

macro(TARGET_OPENVR)
    if (OVERTE_HEADLESS)
        return()
    endif()
    if(OVERTE_USE_SYSTEM_LIBS)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(OpenVR REQUIRED openvr)
        target_include_directories(${TARGET_NAME} SYSTEM PRIVATE ${OpenVR_INCLUDE_DIRS})
        target_link_libraries(${TARGET_NAME} ${OpenVR_LINK_LIBRARIES})
    else()
        find_package(OpenVR QUIET REQUIRED)
        target_link_libraries(${TARGET_NAME} openvr::openvr)
    endif()
endmacro()
