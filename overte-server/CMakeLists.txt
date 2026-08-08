# Copyright 2026
# SPDX-License-Identifier: Apache-2.0
#
# overte-server: a multicall single-binary entry point that embeds the
# domain-server and assignment-client applets (audio/avatar/entity mixers).
# `overte-server domain|audio|avatar|entity|start ...` dispatches to the
# corresponding applet; `start` runs the full 4-process VR room stack under
# one supervisor process.

set(TARGET_NAME overte-server)

project(${TARGET_NAME})

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

# the multicall applet mains are renamed from `main` via this define
target_compile_definitions(${TARGET_NAME} PRIVATE OVERTE_MULTICALL_APPLET)

# AssetsBackupHandler methods are compiled out in headless builds; matching the
# domain-server target, do not generate moc for it or its vtable will reference
# symbols that do not exist.
if (OVERTE_HEADLESS)
  set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/domain-server/src/AssetsBackupHandler.h"
    PROPERTIES SKIP_AUTOMOC ON
  )
endif()

if (OVERTE_HEADLESS)
  target_compile_definitions(${TARGET_NAME} PRIVATE OVERTE_HEADLESS QT_CORE_LIB QT_GUI_LIB)
  find_package(Qt5 COMPONENTS Core QUIET)
  target_link_libraries(${TARGET_NAME} Qt5::Core)
else()
  find_package(Qt5 COMPONENTS Core Gui Network Quick WebSockets QUIET REQUIRED)
  target_link_libraries(${TARGET_NAME} Qt5::Core Qt5::Gui Qt5::Network Qt5::Quick Qt5::WebSockets)
endif()

# generated app version header and the compat HTTPManager stub
target_include_directories(${TARGET_NAME} PRIVATE
  "${CMAKE_BINARY_DIR}/includes"
  "${CMAKE_SOURCE_DIR}/compat"
  "${CMAKE_SOURCE_DIR}/domain-server/src"
  "${CMAKE_SOURCE_DIR}/assignment-client/src"
)

# union of the hifi libraries linked by both applets
link_hifi_libraries(
  networking shared physics octree avatars entities
  audio animation recording plugins script-engine image graphics
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

target_quazip()

add_crashpad()
target_breakpad()

install_beside_console()
