set(TARGET_NAME model-serializers)


setup_hifi_library()

link_hifi_libraries(shared graphics networking image hfm procedural material-networking ktx shaders)
include_hifi_library_headers(gpu image)

if (OVERTE_HEADLESS)
    # Draco and cgltf are not available in the headless build. GLTFSerializer.cpp
    # was removed in this headless fork; its header stays because
    # model-networking/ModelCache.cpp references it (guarded internally by
    # OVERTE_HEADLESS). Keep AUTOMOC off the remaining header.
    set_source_files_properties(
        src/GLTFSerializer.h
        PROPERTIES HEADER_FILE_ONLY TRUE SKIP_AUTOMOC TRUE
    )
else()
    target_draco()
    target_cgltf()
endif()