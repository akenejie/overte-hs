#
#  SetupHifiLibrary.cmake
#
#  Copyright 2013 High Fidelity, Inc.
#  Copyright 2024-2026 Overte e.V.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#

#
# overte-hs modifications:
# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
# (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

macro(SETUP_HIFI_LIBRARY)

  project(${TARGET_NAME})

  # grab the implementation and header files
  file(GLOB_RECURSE LIB_SRCS "src/*.h" "src/*.cpp" "src/*.c" "src/*.qrc")
  list(APPEND ${TARGET_NAME}_SRCS ${LIB_SRCS})

  # Compiler flags for x86 SIMD (-mavx* / /arch:*). Only valid on x86/x86_64:
  # guard against ARM hosts (Linux aarch64, Apple Silicon arm64, Android arm) to
  # avoid passing unsupported flags.
  set(_AVX_TARGET FALSE)
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86|x86)")
    set(_AVX_TARGET TRUE)
  endif()

  # add compiler flags to AVX source files
  file(GLOB_RECURSE AVX_SRCS "src/avx/*.cpp" "src/avx/*.c")
  foreach(SRC ${AVX_SRCS})
    if (WIN32 AND _AVX_TARGET)
      set_source_files_properties(${SRC} PROPERTIES COMPILE_FLAGS /arch:AVX)
    elseif (NOT WIN32 AND _AVX_TARGET AND NOT ANDROID)
      set_source_files_properties(${SRC} PROPERTIES COMPILE_FLAGS -mavx)
    endif()
  endforeach()

  # add compiler flags to AVX2 source files
  file(GLOB_RECURSE AVX2_SRCS "src/avx2/*.cpp" "src/avx2/*.c")
  foreach(SRC ${AVX2_SRCS})
    if (WIN32 AND _AVX_TARGET)
      set_source_files_properties(${SRC} PROPERTIES COMPILE_FLAGS /arch:AVX2)
    elseif (NOT WIN32 AND _AVX_TARGET AND NOT ANDROID)
      set_source_files_properties(${SRC} PROPERTIES COMPILE_FLAGS "-mavx2 -mfma")
    endif()
  endforeach()

  # add compiler flags to AVX512 source files, if supported by compiler
  include(CheckCXXCompilerFlag)
  file(GLOB_RECURSE AVX512_SRCS "src/avx512/*.cpp" "src/avx512/*.c")
  foreach(SRC ${AVX512_SRCS})
    if (WIN32 AND _AVX_TARGET)
      check_cxx_compiler_flag("/arch:AVX512" COMPILER_SUPPORTS_AVX512)
      if (COMPILER_SUPPORTS_AVX512)
        set_source_files_properties(${SRC} PROPERTIES COMPILE_FLAGS /arch:AVX512)
      endif()
    elseif (NOT WIN32 AND _AVX_TARGET AND NOT ANDROID)
      check_cxx_compiler_flag("-mavx512f" COMPILER_SUPPORTS_AVX512)
      if (COMPILER_SUPPORTS_AVX512)
        set_source_files_properties(${SRC} PROPERTIES COMPILE_FLAGS -mavx512f)
      endif()
    endif()
  endforeach()

  setup_memory_debugger()
  setup_thread_debugger()

  # create a library and set the property so it can be referenced later
  if (${${TARGET_NAME}_SHARED})
    add_library(${TARGET_NAME} SHARED ${LIB_SRCS} ${AUTOSCRIBE_SHADER_LIB_SRC} ${GENERATE_ENTITIES_LIB_SRC} ${GENERATE_RENDER_PIPELINES_LIB_SRC} ${QT_RESOURCES_FILE})
  else ()
    add_library(${TARGET_NAME} ${LIB_SRCS} ${AUTOSCRIBE_SHADER_LIB_SRC} ${GENERATE_ENTITIES_LIB_SRC} ${GENERATE_RENDER_PIPELINES_LIB_SRC} ${QT_RESOURCES_FILE})
  endif ()

  set(${TARGET_NAME}_DEPENDENCY_QT_MODULES ${ARGN})
  list(APPEND ${TARGET_NAME}_DEPENDENCY_QT_MODULES Core)

  if (OVERTE_HEADLESS)
    # Headless mode: use real Qt5 with MOC, exclude WebSockets (not installed)
    target_compile_definitions(${TARGET_NAME} PRIVATE OVERTE_HEADLESS QT_CORE_LIB QT_GUI_LIB)
    set(_QT_MODULES ${${TARGET_NAME}_DEPENDENCY_QT_MODULES})
    list(REMOVE_ITEM _QT_MODULES WebSockets Qt5WebSockets)
    if(_QT_MODULES)
      find_package(Qt5 COMPONENTS ${_QT_MODULES} QUIET)
      foreach(QT_MODULE ${_QT_MODULES})
        if(TARGET Qt5::${QT_MODULE})
          target_link_libraries(${TARGET_NAME} Qt5::${QT_MODULE})
        endif()
      endforeach()
    endif()
  else()
    # find these Qt modules and link them to our own target
    find_package(Qt5 COMPONENTS ${${TARGET_NAME}_DEPENDENCY_QT_MODULES} QUIET REQUIRED CMAKE_FIND_ROOT_PATH_BOTH)

    foreach(QT_MODULE ${${TARGET_NAME}_DEPENDENCY_QT_MODULES})
      target_link_libraries(${TARGET_NAME} Qt5::${QT_MODULE})
    endforeach()
  endif()

  # Don't make scribed shaders, generated entity files, generated pipelines, or QT resource files cumulative
  set(AUTOSCRIBE_SHADER_LIB_SRC "")
  set(GENERATE_ENTITIES_LIB_SRC "")
  set(GENERATE_RENDER_PIPELINES_LIB_SRC "")
  set(QT_RESOURCES_FILE "")

  target_glm()

  set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "Libraries")

endmacro(SETUP_HIFI_LIBRARY)
