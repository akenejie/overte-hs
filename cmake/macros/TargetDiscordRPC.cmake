# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

macro(TARGET_DISCORD_RPC)
    if (OVERTE_HEADLESS)
        return()
    endif()
    if (OVERTE_USE_SYSTEM_LIBS)
        find_path(DiscordRPC_INCLUDE_DIRS "discord_rpc.h")

        target_include_directories(${TARGET_NAME} SYSTEM PRIVATE ${DiscordRPC_INCLUDE_DIRS})
        target_link_libraries(${TARGET_NAME} libdiscord-rpc.so)
    else()
        find_package(discord-rpc QUIET REQUIRED)
        target_link_libraries(${TARGET_NAME} discord-rpc::discord-rpc)
    endif()
endmacro()
