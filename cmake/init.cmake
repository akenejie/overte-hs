# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMakeTargets")
# Hide automoc folders (for IDEs)
set(AUTOGEN_TARGETS_FOLDER "hidden/generated")
# Find includes in corresponding build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if (CMAKE_BUILD_TYPE)
  string(TOUPPER ${CMAKE_BUILD_TYPE} UPPER_CMAKE_BUILD_TYPE)
else ()
  set(UPPER_CMAKE_BUILD_TYPE DEBUG)
endif ()

# CMAKE_CURRENT_SOURCE_DIR is the parent folder here
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/")

set(HF_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(MACRO_DIR "${HF_CMAKE_DIR}/macros")
set(EXTERNAL_PROJECT_DIR "${HF_CMAKE_DIR}/externals")

file(GLOB HIFI_CUSTOM_MACROS "cmake/macros/*.cmake")
foreach(CUSTOM_MACRO ${HIFI_CUSTOM_MACROS})
  include(${CUSTOM_MACRO})
endforeach()
unset(HIFI_CUSTOM_MACROS)

if (ANDROID)
    set(BUILD_SHARED_LIBS ON)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

    string(REGEX REPLACE "\\\\" "/" ANDROID_NDK ${ANDROID_NDK})
    string(REGEX REPLACE "\\\\" "/" CMAKE_TOOLCHAIN_FILE ${CMAKE_TOOLCHAIN_FILE})
    string(REGEX REPLACE "\\\\" "/" ANDROID_TOOLCHAIN ${ANDROID_TOOLCHAIN})
    string(REGEX REPLACE "\\\\" "/" CMAKE_MAKE_PROGRAM ${CMAKE_MAKE_PROGRAM})
    list(APPEND EXTERNAL_ARGS -DANDROID_ABI=${ANDROID_ABI})
    list(APPEND EXTERNAL_ARGS -DANDROID_NDK=${ANDROID_NDK})
    list(APPEND EXTERNAL_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
    list(APPEND EXTERNAL_ARGS -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
    list(APPEND EXTERNAL_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
    list(APPEND EXTERNAL_ARGS -DHIFI_ANDROID=${HIFI_ANDROID})
    list(APPEND EXTERNAL_ARGS -DANDROID_PLATFORM=${ANDROID_PLATFORM})
    list(APPEND EXTERNAL_ARGS -DANDROID_TOOLCHAIN=${ANDROID_TOOLCHAIN})
    list(APPEND EXTERNAL_ARGS -DANDROID_STL=${ANDROID_STL})
endif ()

if (APPLE)
  set(CMAKE_XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--deep")
  set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
  set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
endif()

if (UNIX)
  # Static libs result in duplicated constructor and destructor calls on Linux
  # and crashes on exit, and perhaps loss of global state on plugin loads.
  #
  # This will need to be looked at closely before Linux can have a static build.
  #
  # Opt-in to a static build by passing -DBUILD_SHARED_LIBS=OFF.
  if (NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS ON)
  endif ()
endif ()

find_package(Python3 REQUIRED)
