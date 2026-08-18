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

macro(manually_install_openssl_for_qt)

  # Qt dynamically links OpenSSL if it can find it on the user's machine
  # We want to avoid it being found somewhere random and have it not being a compatible version
  # So even though we don't need the dynamic version of OpenSSL for our direct-use purposes
  # we use this macro to include the two SSL DLLs with the targets using QtNetwork
  if (WIN32)
    # Detect actual OpenSSL DLL names — they differ between versions:
    #   1.1.x:  libcrypto-1_1-x64.dll, libssl-1_1-x64.dll
    #   3.x:    libcrypto-3-x64.dll,   libssl-3-x64.dll
    # Conan copies all *.dll into conanlibs/<config>/ at install time.
    # We glob at configure time to pick up whichever version is present.
    set(_OPENSSL_CONANLIB_DIR "${CMAKE_BINARY_DIR}/conanlibs/Release")
    file(GLOB _OPENSSL_CRYPTO_DLLS "${_OPENSSL_CONANLIB_DIR}/libcrypto*.dll")
    file(GLOB _OPENSSL_SSL_DLLS "${_OPENSSL_CONANLIB_DIR}/libssl*.dll")

    foreach(_dll IN LISTS _OPENSSL_CRYPTO_DLLS _OPENSSL_SSL_DLLS)
      get_filename_component(_dll_name "${_dll}" NAME)
      install(
        FILES "${_dll}"
        DESTINATION ${TARGET_INSTALL_DIR}
        COMPONENT ${TARGET_INSTALL_COMPONENT}
      )
      add_custom_command(
        TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${_dll}" "$<TARGET_FILE_DIR:${TARGET_NAME}>/${_dll_name}"
        COMMENT "Copy ${_dll_name}"
      )
    endforeach()

  endif()

endmacro()
