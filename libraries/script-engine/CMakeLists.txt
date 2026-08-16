# Copyright 2013-2019, High Fidelity, Inc.
# Copyright 2021-2023 Overte e.V.
# SPDX-License-Identifier: Apache-2.0

set(TARGET_NAME script-engine)


# FIXME Move undo scripting interface to application and remove Widgets
setup_hifi_library(Network)
if (OVERTE_HEADLESS)
    target_compile_definitions(${TARGET_NAME} PRIVATE OVERTE_NO_QTWEBSOCKET)
    # QtWebSockets is not built in the headless Qt. Exclude the WebSocket
    # classes entirely (the sources are already guarded by OVERTE_NO_QTWEBSOCKET)
    # and stop AUTOMOC from processing their headers.
    set_source_files_properties(
        src/WebSocketClass.cpp
        src/WebSocketClass.h
        src/WebSocketServerClass.cpp
        src/WebSocketServerClass.h
        PROPERTIES HEADER_FILE_ONLY TRUE SKIP_AUTOMOC TRUE
    )
endif()

link_hifi_libraries(networking)
include_hifi_library_headers(octree)
include_hifi_library_headers(shared)

target_zlib()
if (NOT ANDROID)
  target_quazip()
endif ()
