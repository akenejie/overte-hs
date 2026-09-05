#
#  ManuallyInstallOpenSSLforQt.cmake
#
#  Created by Stephen Birarda on 1/15/16.
#  Copyright 2014 High Fidelity, Inc.
#  Copyright 2020 Vircadia contributors.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#

#
# overte-hs modifications:
# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
# (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

macro(manually_install_openssl_for_qt)

  # Qt dynamically links OpenSSL if it can find it on the user's machine
  # We want to avoid it being found somewhere random and have it not being a compatible version
  # So even though we don't need the dynamic version of OpenSSL for our direct-use purposes
  # we use this macro to include the two SSL DLLs with the targets using QtNetwork
  if (WIN32)
    # Use a build-time script so the glob runs at BUILD time — not at configure
    # time — which correctly handles multi-config generators (MSVC) and avoids
    # stale-file / missing-directory issues with file(GLOB).
    add_custom_command(
      TARGET ${TARGET_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        -DCONANLIB_DIR="${CMAKE_BINARY_DIR}/conanlibs/$<CONFIG>"
        -DDEST_DIR="$<TARGET_FILE_DIR:${TARGET_NAME}>"
        -P "${CMAKE_SOURCE_DIR}/cmake/CopyOpenSSLDlls.cmake"
      COMMENT "Copy OpenSSL DLLs (build-time)"
    )
  endif()

endmacro()
