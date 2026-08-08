set(TARGET_NAME model-baker)


setup_hifi_library()

link_hifi_libraries(shared shaders task gpu graphics hfm procedural)
include_hifi_library_headers(material-networking)
include_hifi_library_headers(networking)
include_hifi_library_headers(image)
include_hifi_library_headers(ktx)

if (OVERTE_HEADLESS)
    # Draco is not available in the headless build.
    set_source_files_properties(
        src/model-baker/BuildDracoMeshTask.cpp
        PROPERTIES HEADER_FILE_ONLY TRUE SKIP_AUTOMOC TRUE
    )
else()
    target_draco()
endif()
