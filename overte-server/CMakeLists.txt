# Copyright 2026
# SPDX-License-Identifier: Apache-2.0
#
# overte-server: a multicall single-binary entry point that embeds the
# domain-server and assignment-client applets (audio/avatar/entity/
# entity-script/assets/messages mixers and servers).
# `overte-server domain|audio|avatar|entity|entity-script|assets|messages
# <args...>` dispatches to the corresponding applet; the flag mode
# (`--domain/--audio/--avatar/--entity/--entity-script/--assets/--messages
# <port>` in any combination) runs the full VR room stack under one
# supervisor process.

#
# overte-hs modifications:
# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
# (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

set(TARGET_NAME overte-server)

project(${TARGET_NAME})

# Embed the default empty world (default-data/entities/models.json.gz) as a C
# byte array so a freshly created --data dir can be seeded at first run
# without any runtime data files or Qt resource dependency.
set(DEFAULT_DATA_HPP "${CMAKE_BINARY_DIR}/includes/default_data.hpp")
add_custom_command(
  OUTPUT "${DEFAULT_DATA_HPP}"
  COMMAND ${CMAKE_COMMAND}
          -DSRC=${CMAKE_SOURCE_DIR}/default-data/entities/models.json.gz
          -DOUT=${DEFAULT_DATA_HPP}
          -P ${CMAKE_SOURCE_DIR}/cmake/GenerateDefaultData.cmake
  DEPENDS "${CMAKE_SOURCE_DIR}/default-data/entities/models.json.gz"
          "${CMAKE_SOURCE_DIR}/cmake/GenerateDefaultData.cmake"
  COMMENT "Generating ${DEFAULT_DATA_HPP} (embedded default empty world)"
  VERBATIM
)
add_custom_target(overte_server_default_data DEPENDS "${DEFAULT_DATA_HPP}")

# Embed the release version as a C macro so `--version` reports the same number
# the release pipeline uses for tags and binary names. version.txt at the
# repository root is the single source of truth - bump it in one place and the
# binary, the GitHub Actions tags/binaries and this header all follow.
set(OVERTE_HS_VERSION_HPP "${CMAKE_BINARY_DIR}/includes/overte_hs_version.hpp")
add_custom_command(
  OUTPUT "${OVERTE_HS_VERSION_HPP}"
  COMMAND ${CMAKE_COMMAND}
          -DSRC=${CMAKE_SOURCE_DIR}/version.txt
          -DOUT=${OVERTE_HS_VERSION_HPP}
          -P ${CMAKE_SOURCE_DIR}/cmake/GenerateVersionHeader.cmake
  DEPENDS "${CMAKE_SOURCE_DIR}/version.txt"
          "${CMAKE_SOURCE_DIR}/cmake/GenerateVersionHeader.cmake"
  COMMENT "Generating ${OVERTE_HS_VERSION_HPP} (overte-hs version)"
  VERBATIM
)
add_custom_target(overte_server_version DEPENDS "${OVERTE_HS_VERSION_HPP}")

option(OVERTE_SERVER_STRIP "Strip debug symbols from the overte-server binary to shrink it" OFF)

set(OVERTE_SERVER_OWN_SRCS src/main.cpp)

# gather the domain-server applet sources
file(GLOB OVERTE_SERVER_DS_SRCS "${CMAKE_SOURCE_DIR}/domain-server/src/*")
file(GLOB OVERTE_SERVER_DS_SUBDIRS RELATIVE "${CMAKE_SOURCE_DIR}/domain-server/src" "${CMAKE_SOURCE_DIR}/domain-server/src/*")
foreach(OVERTE_SERVER_DS_SUBDIR ${OVERTE_SERVER_DS_SUBDIRS})
  if (IS_DIRECTORY "${CMAKE_SOURCE_DIR}/domain-server/src/${OVERTE_SERVER_DS_SUBDIR}")
    file(GLOB OVERTE_SERVER_DS_SUBDIR_CONTENTS "${CMAKE_SOURCE_DIR}/domain-server/src/${OVERTE_SERVER_DS_SUBDIR}/*")
    list(APPEND OVERTE_SERVER_DS_SRCS ${OVERTE_SERVER_DS_SUBDIR_CONTENTS})
  endif ()
endforeach()

# gather the assignment-client applet sources
file(GLOB OVERTE_SERVER_AC_SRCS "${CMAKE_SOURCE_DIR}/assignment-client/src/*")
file(GLOB OVERTE_SERVER_AC_SUBDIRS RELATIVE "${CMAKE_SOURCE_DIR}/assignment-client/src" "${CMAKE_SOURCE_DIR}/assignment-client/src/*")
foreach(OVERTE_SERVER_AC_SUBDIR ${OVERTE_SERVER_AC_SUBDIRS})
  if (IS_DIRECTORY "${CMAKE_SOURCE_DIR}/assignment-client/src/${OVERTE_SERVER_AC_SUBDIR}")
    file(GLOB OVERTE_SERVER_AC_SUBDIR_CONTENTS "${CMAKE_SOURCE_DIR}/assignment-client/src/${OVERTE_SERVER_AC_SUBDIR}/*")
    list(APPEND OVERTE_SERVER_AC_SRCS ${OVERTE_SERVER_AC_SUBDIR_CONTENTS})
  endif ()
endforeach()

add_executable(${TARGET_NAME} ${OVERTE_SERVER_OWN_SRCS} ${OVERTE_SERVER_DS_SRCS} ${OVERTE_SERVER_AC_SRCS} resources.qrc)
add_dependencies(${TARGET_NAME} overte_server_default_data overte_server_version)

# the multicall applet mains are renamed from `main` via this define
target_compile_definitions(${TARGET_NAME} PRIVATE OVERTE_MULTICALL_APPLET)

target_compile_definitions(${TARGET_NAME} PRIVATE OVERTE_HEADLESS QT_CORE_LIB QT_GUI_LIB)
find_package(Qt5 COMPONENTS Core QUIET)
target_link_libraries(${TARGET_NAME} Qt5::Core)

# generated app version header
target_include_directories(${TARGET_NAME} PRIVATE
  "${CMAKE_BINARY_DIR}/includes"
  "${CMAKE_SOURCE_DIR}/domain-server/src"
  "${CMAKE_SOURCE_DIR}/assignment-client/src"
)

# union of the hifi libraries linked by both applets
link_hifi_libraries(
  networking shared physics octree avatars entities
  audio animation recording audio-codec plugins script-engine image graphics
  model-networking model-serializers gpu ktx shaders render material-networking procedural
  hfm model-baker workload task
)
include_hifi_library_headers(procedural)
include_hifi_library_headers(entities)

target_glm()
target_zlib()

# shrink the deliverable: `-s` removes debug/annotation symbols at link time
if (OVERTE_SERVER_STRIP)
  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(${TARGET_NAME} PRIVATE -s)
  endif ()
endif ()

# libcrypto uses dlopen in libdl
if (UNIX)
  target_link_libraries(${TARGET_NAME} ${CMAKE_DL_LIBS})
endif ()

target_link_libraries(${TARGET_NAME} ${CMAKE_THREAD_LIBS_INIT})

add_crashpad()
target_breakpad()

install_beside_console()
