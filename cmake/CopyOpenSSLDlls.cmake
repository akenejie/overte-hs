#
#  CopyOpenSSLDlls.cmake
#  cmake
#
#  Copyright 2026 Overte e.V.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#
#  Build-time script that finds and copies OpenSSL DLLs from the conanlibs
#  directory into the target output directory.  Invoked by add_custom_command
#  (POST_BUILD) so the glob happens at BUILD time — not at configure time —
#  which avoids stale-file issues with file(GLOB) on multi-config generators.
#
#  Parameters (passed via -D):
#    CONANLIB_DIR  – absolute path to conanlibs/<config>/
#    DEST_DIR      – absolute path to the target's output directory

if (NOT CONANLIB_DIR OR NOT DEST_DIR)
    message(FATAL_ERROR "CopyOpenSSLDlls.cmake: CONANLIB_DIR and DEST_DIR must be set")
endif ()

file(GLOB _crypto_dlls "${CONANLIB_DIR}/libcrypto*.dll")
file(GLOB _ssl_dlls    "${CONANLIB_DIR}/libssl*.dll")

set(_all_dlls ${_crypto_dlls} ${_ssl_dlls})

if (NOT _all_dlls)
    message(STATUS "CopyOpenSSLDlls: no OpenSSL DLLs found in ${CONANLIB_DIR} — skipping")
    return ()
endif ()

foreach(_dll IN LISTS _all_dlls)
    get_filename_component(_name "${_dll}" NAME)
    message(STATUS "CopyOpenSSLDlls: ${_name} -> ${DEST_DIR}")
    file(COPY "${_dll}" DESTINATION "${DEST_DIR}")
endforeach ()
