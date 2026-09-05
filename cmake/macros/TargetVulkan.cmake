# 
#  Created by Bradley Austin Davis on 2016/02/16
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
# 

#
# overte-hs modifications:
# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
# (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

macro(TARGET_VULKAN)
    if (OVERTE_HEADLESS)
        return()
    endif()
    find_package(Vulkan QUIET REQUIRED)
    find_package(VulkanMemoryAllocator QUIET REQUIRED)
    find_package(Qt5 COMPONENTS X11Extras QUIET REQUIRED)
    target_include_directories(${TARGET_NAME} PRIVATE ${VULKAN_INCLUDE_DIR})
    target_link_libraries(${TARGET_NAME} GPUOpen::VulkanMemoryAllocator)
    target_link_libraries(${TARGET_NAME} ${VULKAN_LIBRARY})
    target_link_libraries(${TARGET_NAME} ${Qt5X11Extras_LIBRARIES})
endmacro()
