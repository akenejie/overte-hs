# Copyright 2014-2018 High Fidelity, Inc.
# Copyright 2021-2023 Overte e.V.
# SPDX-License-Identifier: Apache-2.0

#
# overte-hs modifications:
# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
# (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

set(TARGET_NAME domain-server)

if (UPPER_CMAKE_BUILD_TYPE MATCHES DEBUG AND NOT WIN32)
  set(_SHOULD_SYMLINK_RESOURCES TRUE)
else ()
  set(_SHOULD_SYMLINK_RESOURCES FALSE)
endif ()

# setup the project and link required Qt modules
setup_hifi_project(Network)

# Fix up the rpath so macdeployqt works
if (APPLE)
  set_target_properties(${TARGET_NAME} PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
endif ()

setup_memory_debugger()
setup_thread_debugger()

# copy resources (describe-settings.json) so the standalone binary can load them at runtime
symlink_or_copy_directory_beside_target(${_SHOULD_SYMLINK_RESOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/resources" "resources")

# link the shared hifi libraries
include_hifi_library_headers(gpu)
include_hifi_library_headers(graphics)
include_hifi_library_headers(script-engine)

link_hifi_libraries(networking shared avatars octree)

target_zlib()

add_crashpad()
target_breakpad()

# libcrypto uses dlopen in libdl
if (UNIX)
  target_link_libraries(${TARGET_NAME} ${CMAKE_DL_LIBS})
endif ()

if (WIN32)
  package_libraries_for_deployment()
endif ()

install_beside_console()
