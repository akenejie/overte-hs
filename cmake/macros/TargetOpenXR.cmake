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

macro(TARGET_OPENXR)
    if (OVERTE_HEADLESS)
        return()
    endif()
    find_package(OpenXR QUIET REQUIRED)
    target_link_libraries(${TARGET_NAME} OpenXR::openxr_loader)
endmacro()
