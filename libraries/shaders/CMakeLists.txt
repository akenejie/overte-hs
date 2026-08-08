set(TARGET_NAME shaders)

if (OVERTE_HEADLESS)
    # Shader enum generation is configure-time only and needs no scribe binary, so
    # run it here to produce the full ShaderEnums.h that gpu and the other linked
    # libraries include. The scribed/reflected shader targets are never built in
    # headless builds (nothing depends on them).
    autoscribe_shader_libs(gpu graphics display-plugins procedural render render-utils entities-renderer)
    # Minimal implementation for headless builds - shader processing (scribe/spirv) is unavailable
    add_library(${TARGET_NAME} src/shaders/ShadersHeadless.cpp)
    set_target_properties(${TARGET_NAME} PROPERTIES LINKER_LANGUAGE CXX)
    target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_BINARY_DIR}/libraries/shaders")
    target_include_directories(${TARGET_NAME} INTERFACE "${CMAKE_BINARY_DIR}/libraries/shaders")
    target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/libraries/shaders/src/shaders")
    find_package(Qt5 COMPONENTS Core QUIET)
    target_link_libraries(${TARGET_NAME} Qt5::Core)
    return()
endif()

autoscribe_shader_libs(gpu graphics display-plugins procedural render render-utils entities-renderer)
setup_hifi_library(Gui)
add_dependencies(${TARGET_NAME} scribed_shaders spirv_shaders reflected_shaders)
target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_BINARY_DIR}/libraries/shaders/src/shaders")
target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/libraries/shaders/src/shaders")
link_hifi_libraries(shared gl)
target_json()
