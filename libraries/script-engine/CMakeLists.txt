# Copyright 2013-2019, High Fidelity, Inc.
# Copyright 2021-2023 Overte e.V.
# SPDX-License-Identifier: Apache-2.0

#
# overte-hs modifications:
# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
# (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

set(TARGET_NAME script-engine)

# FIXME Move undo scripting interface to application and remove Widgets
setup_hifi_library(Network)

# quickjs.c uses C99 designated initializers (e.g., .u = { .func = { ... } })
# and function designators as constant expressions in static initializers.
# MSVC defaults to C89; enable C11 or later for this target's C sources.
# Use target_compile_options (after setup_hifi_library creates the target)
# so the flag is reliably applied to all C compilations in this target.
if (MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:/std:c11>
    )
endif()
if (OVERTE_HEADLESS)
    target_compile_definitions(${TARGET_NAME} PRIVATE OVERTE_NO_QTWEBSOCKET)
    # QtWebSockets is not built in the headless Qt. The WebSocket implementations
    # (WebSocketClass.cpp / WebSocketServerClass.cpp) were removed in this
    # headless fork; their headers stay because ScriptManager.cpp references
    # them (the sources are already guarded by OVERTE_NO_QTWEBSOCKET). Keep
    # AUTOMOC off the remaining headers.
    set_source_files_properties(
        src/WebSocketClass.h
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
