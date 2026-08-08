set(TARGET_NAME image)


setup_hifi_library()
if (OVERTE_HEADLESS)
    # Headless servers do not link OpenEXR. Exclude the reader so the library
    # builds without the OpenEXR headers.
    set_source_files_properties(src/image/OpenEXRReader.cpp PROPERTIES HEADER_FILE_ONLY TRUE SKIP_AUTOMOC TRUE)
endif()
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
