#
#  SetupHifiProject.cmake
#
#  Copyright 2013 High Fidelity, Inc.
#  Copyright 2026 Overte e.V.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#

macro(SETUP_HIFI_PROJECT)
  project(${TARGET_NAME})

  # grab the implemenation and header files
  file(GLOB TARGET_SRCS src/*)

  file(GLOB SRC_SUBDIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/*)

  foreach(DIR ${SRC_SUBDIRS})
    if (IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/${DIR}")
      file(GLOB DIR_CONTENTS "src/${DIR}/*")
      set(TARGET_SRCS ${TARGET_SRCS} "${DIR_CONTENTS}")
    endif ()
  endforeach()

  add_executable(${TARGET_NAME} ${TARGET_SRCS} ${AUTOSCRIBE_SHADER_LIB_SRC} ${GENERATE_ENTITIES_LIB_SRC} ${GENERATE_RENDER_PIPELINES_LIB_SRC})

  # include the generated application version header
  target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_BINARY_DIR}/includes")

  set(${TARGET_NAME}_DEPENDENCY_QT_MODULES ${ARGN})
  list(APPEND ${TARGET_NAME}_DEPENDENCY_QT_MODULES Core)

  if (OVERTE_HEADLESS)
    # Headless mode: use real Qt5 with MOC, exclude WebSockets (not installed)
    target_compile_definitions(${TARGET_NAME} PRIVATE OVERTE_HEADLESS QT_CORE_LIB QT_GUI_LIB)
    set(_QT_MODULES ${${TARGET_NAME}_DEPENDENCY_QT_MODULES})
    list(REMOVE_ITEM _QT_MODULES WebSockets Qt5WebSockets)
    find_package(Qt5 COMPONENTS ${_QT_MODULES} QUIET)
    foreach(QT_MODULE ${_QT_MODULES})
      if(TARGET Qt5::${QT_MODULE})
        target_link_libraries(${TARGET_NAME} Qt5::${QT_MODULE})
      endif()
    endforeach()
  else()
    # find these Qt modules and link them to our own target
    find_package(Qt5 COMPONENTS ${${TARGET_NAME}_DEPENDENCY_QT_MODULES} QUIET REQUIRED)

    foreach(QT_MODULE ${${TARGET_NAME}_DEPENDENCY_QT_MODULES})
      target_link_libraries(${TARGET_NAME} Qt5::${QT_MODULE})
    endforeach()
  endif()
  target_link_libraries(${TARGET_NAME} ${CMAKE_THREAD_LIBS_INIT})

  target_glm()

endmacro()
