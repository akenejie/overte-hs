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

# Link Opus. Upstream's TARGET_opus() macro returns early when OVERTE_HEADLESS
# is set (that is why the stock single-binary build ships with no audio codec
# at all); here the codec is required, so link Opus unconditionally. With conan
# (OVERTE_USE_SYSTEM_LIBS=OFF) this resolves to the static Opus::opus target,
# keeping the deliverable a self-contained binary (no runtime libopus needed).
if (OVERTE_USE_SYSTEM_LIBS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(Opus REQUIRED opus)
  target_include_directories(${TARGET_NAME} SYSTEM PRIVATE ${Opus_INCLUDE_DIRS})
  target_link_libraries(${TARGET_NAME} ${Opus_LINK_LIBRARIES})
else()
  find_package(Opus QUIET REQUIRED)
  target_link_libraries(${TARGET_NAME} Opus::opus)
endif()
