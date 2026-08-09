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
# QEMU-emulated linux/386 images report the host kernel arch (x86_64) via
# CMAKE_SYSTEM_PROCESSOR, so key off sizeof(void*) instead of the arch name and
# probe the flag: gcc/clang on 32-bit ARM reject -msse2, so it stays x86-only.
include(CheckCXXCompilerFlag)
if (NOT WIN32 AND NOT ANDROID AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  check_cxx_compiler_flag("-msse2" HIFI_HAVE_SSE2)
  if (HIFI_HAVE_SSE2)
    target_compile_options(${TARGET_NAME} PRIVATE -msse2)
  endif()
endif()

if (ANDROID)
  add_definitions("-D__STDC_CONSTANT_MACROS")
endif ()

link_hifi_libraries(networking shared plugins script-engine)
