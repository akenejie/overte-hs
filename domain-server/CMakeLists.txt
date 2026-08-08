# Copyright 2014-2018 High Fidelity, Inc.
# Copyright 2021-2023 Overte e.V.
# SPDX-License-Identifier: Apache-2.0

set(TARGET_NAME domain-server)

if (UPPER_CMAKE_BUILD_TYPE MATCHES DEBUG AND NOT WIN32)
  set(_SHOULD_SYMLINK_RESOURCES TRUE)
else ()
  set(_SHOULD_SYMLINK_RESOURCES FALSE)
endif ()

# setup the project and link required Qt modules
if (OVERTE_HEADLESS)
    setup_hifi_project(Network)
    target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/compat")
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AssetsBackupHandler.h"
    PROPERTIES SKIP_AUTOMOC ON
    )
else()
  setup_hifi_project(Network)
endif()

# Fix up the rpath so macdeployqt works
if (APPLE)
  set_target_properties(${TARGET_NAME} PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
endif ()

setup_memory_debugger()
setup_thread_debugger()

# TODO: find a solution that will handle web file changes in resources on windows without a re-build.
symlink_or_copy_directory_beside_target(${_SHOULD_SYMLINK_RESOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/resources" "resources")

# link the shared hifi libraries
include_hifi_library_headers(gpu)
include_hifi_library_headers(graphics)
include_hifi_library_headers(script-engine)

if (OVERTE_HEADLESS)
  link_hifi_libraries(networking shared avatars octree)
else()
  link_hifi_libraries(embedded-webserver networking shared avatars octree)
endif()

target_zlib()
target_quazip()

if (NOT OVERTE_HEADLESS)
  target_openssl()
endif()

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
