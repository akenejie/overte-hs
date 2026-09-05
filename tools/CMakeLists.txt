# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

# Headless project: no build tools are compiled (OVERTE_BUILD_TOOLS is forced
# OFF). tools/shadergen.py is still referenced at build time by
# AutoScribeShader.cmake for the `shaders` library, so it stays in the tree.
# This stub keeps the top-level `add_subdirectory(tools)` valid.

add_custom_target(tools SOURCES shadergen.py)
