# Copyright 2026
# SPDX-License-Identifier: Apache-2.0
#
# audio-codec: statically embedded audio codec implementations for the
# single-binary overte-server. Provides an Opus encoder/decoder exposed through
# the canonical Overte CodecPlugin/Encoder/Decoder interfaces. Unlike upstream,
# where codecs are loaded at runtime from a plugins/ directory via QPluginLoader,
# this HS build compiles the codec directly into the binary and registers it
# through PluginManager::setCodecPluginProvider().

set(TARGET_NAME audio-codec)

setup_hifi_library()

link_hifi_libraries(shared plugins audio)

# Link the system Opus codec library. Upstream's TARGET_opus() macro bails out
# when OVERTE_HEADLESS is set (that is why the stock single-binary build ships
# with no audio codec at all); here the codec is required, so link it directly.
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(HIFI_OPUS QUIET opus)
endif()
if (HIFI_OPUS_FOUND)
  target_include_directories(${TARGET_NAME} SYSTEM PRIVATE ${HIFI_OPUS_INCLUDE_DIRS})
  target_link_libraries(${TARGET_NAME} ${HIFI_OPUS_LINK_LIBRARIES})
else()
  target_include_directories(${TARGET_NAME} SYSTEM PRIVATE /usr/include)
  target_link_libraries(${TARGET_NAME} opus)
endif()
