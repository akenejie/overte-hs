set(TARGET_NAME ice-server)

# setup the project and link required Qt modules
setup_hifi_project(Network)

# link the shared hifi libraries
if (OVERTE_HEADLESS)
    target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/compat")
    link_hifi_libraries(networking shared)
else()
    link_hifi_libraries(embedded-webserver networking shared)
    package_libraries_for_deployment()
endif()

# find OpenSSL
find_package(OpenSSL QUIET REQUIRED)

setup_memory_debugger()
setup_thread_debugger()

# append OpenSSL to our list of libraries to link
target_link_libraries(${TARGET_NAME} OpenSSL::SSL OpenSSL::Crypto)
