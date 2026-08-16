# Copyright 2013-2019 High Fidelity, Inc.
# Copyright 2021-2025 Overte e.V.
# SPDX-License-Identifier: Apache-2.0

set(TARGET_NAME assignment-client)

setup_hifi_project(Core Network)

# Fix up the rpath so macdeployqt works
if (APPLE)
  set_target_properties(${TARGET_NAME} PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
endif ()

setup_memory_debugger()
setup_thread_debugger()

# link in the shared libraries
link_hifi_libraries(
  networking shared physics octree avatars entities
  audio animation recording
  plugins
  script-engine
  image
  graphics
  model-networking model-serializers
  gpu ktx shaders render material-networking procedural
  hfm model-baker
  workload task
)
include_hifi_library_headers(procedural)
include_hifi_library_headers(entities)

add_crashpad()
target_breakpad()

if (OVERTE_BUILD_TOOLS)
  add_dependencies(${TARGET_NAME} oven)

  if (WIN32)
    add_custom_command(
      TARGET ${TARGET_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              $<TARGET_FILE_DIR:oven>
              $<TARGET_FILE_DIR:${TARGET_NAME}>)
  else()
    add_custom_command(
      TARGET ${TARGET_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E create_symlink
              $<TARGET_FILE:oven>
              $<TARGET_FILE_DIR:${TARGET_NAME}>/oven)
  endif()
endif()

if (WIN32)
  package_libraries_for_deployment()
endif()

install_beside_console()
