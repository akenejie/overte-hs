# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

set(TARGET_NAME networking)
setup_hifi_library(Network WebSockets)
link_hifi_libraries(shared platform)

target_openssl()
target_tbb()
add_crashpad()
target_breakpad()

if (OVERTE_HEADLESS)
    # Link the Rust UDP library
    if (WIN32)
        set(RUST_UDP_LIB "${CMAKE_SOURCE_DIR}/rust-udp/target/release/overte_udp.lib")
    else()
        set(RUST_UDP_LIB "${CMAKE_SOURCE_DIR}/rust-udp/target/release/liboverte_udp.a")
    endif()
    if (EXISTS ${RUST_UDP_LIB})
        target_link_libraries(${TARGET_NAME} ${RUST_UDP_LIB})
    else()
        message(WARNING "Rust UDP library not found at ${RUST_UDP_LIB}. Build it with: cd rust-udp && cargo build --release")
    endif()
    if (WIN32)
        target_link_libraries(${TARGET_NAME} ws2_32.lib)
    else()
        target_link_libraries(${TARGET_NAME} pthread dl m)
    endif()
else()

if (WIN32 OR (UNIX AND NOT APPLE))
    if (NOT DISABLE_WEBRTC )
        target_webrtc()
    else()
        message(WARNING "WebRTC is supported on this platform but has been disabled for this build (likely memory debugging)")
    endif()
endif ()

if (WIN32)
    # we need ws2_32.lib on windows, but it's static so we don't bubble it up
    # Libraries needed for WebRTC: security.lib winmm.lib
    target_link_libraries(${TARGET_NAME} ws2_32.lib security.lib winmm.lib)
elseif(APPLE)
    # IOKit is needed for getting machine fingerprint
    find_library(FRAMEWORK_IOKIT IOKit)
    find_library(CORE_FOUNDATION CoreFoundation)
    target_link_libraries(${TARGET_NAME} ${FRAMEWORK_IOKIT} ${CORE_FOUNDATION})
elseif(UNIX)
    # libcrypto uses dlopen in libdl
    target_link_libraries(${TARGET_NAME} ${CMAKE_DL_LIBS})
endif ()
endif() # OVERTE_HEADLESS
