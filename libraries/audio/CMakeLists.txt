# Copyright 2013-2016, High Fidelity, Inc.
# Copyright 2022-2023 Overte e.V.
# SPDX-License-Identifier: Apache-2.0

set(TARGET_NAME audio)


setup_hifi_library(Network)

# The audio DSP code (AudioDynamics.h floatToInt, AudioHRTF.cpp) unconditionally
# uses SSE2 intrinsics on x86. AMD64 enables SSE2 by default, but i386 does not,
# so a FORCEINLINE (always_inline) SSE2 function cannot be inlined into an i386
# call site. Compile the whole library with SSE2 on 32-bit x86 to keep the call
# sites and the intrinsics on the same target.
if (NOT WIN32 AND NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86|i686)$")
  if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_options(${TARGET_NAME} PRIVATE -msse2)
  endif()
endif()

if (ANDROID)
  add_definitions("-D__STDC_CONSTANT_MACROS")
endif ()

link_hifi_libraries(networking shared plugins script-engine)
