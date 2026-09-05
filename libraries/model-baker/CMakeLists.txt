# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

set(TARGET_NAME model-baker)


setup_hifi_library()

link_hifi_libraries(shared shaders task gpu graphics hfm procedural)
include_hifi_library_headers(material-networking)
include_hifi_library_headers(networking)
include_hifi_library_headers(image)
include_hifi_library_headers(ktx)

if (OVERTE_HEADLESS)
    # Draco is not available in the headless build. BuildDracoMeshTask.cpp was
    # removed in this headless fork; its header stays because Baker.cpp
    # references it (and is internally guarded by OVERTE_HEADLESS).
else()
    target_draco()
endif()
