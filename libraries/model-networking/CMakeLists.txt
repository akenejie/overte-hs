# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

set(TARGET_NAME model-networking)


setup_hifi_library()
link_hifi_libraries(shared shaders networking graphics model-serializers procedural model-baker)
include_hifi_library_headers(hfm)
include_hifi_library_headers(task)
include_hifi_library_headers(gpu)
include_hifi_library_headers(image)
include_hifi_library_headers(ktx)
include_hifi_library_headers(material-networking)
