set(TARGET_NAME image)


setup_hifi_library()
# The OpenEXR reader implementation (OpenEXRReader.cpp) was removed in this
# headless fork; its header is kept because TextureProcessing.cpp references it.
# The library still links OpenEXR so the header-only dependency resolves.
link_hifi_libraries(shared gpu)
target_nvtt()
target_tbb()
target_etc2comp()
target_openexr()

if (UNIX AND NOT APPLE)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(image Threads::Threads)
endif()

if (WIN32)
  add_compile_definitions(_USE_MATH_DEFINES)
endif()
