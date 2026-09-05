// Copyright 2026
// SPDX-License-Identifier: Apache-2.0
//
// OpusCodec: statically embedded Opus audio codec for the single-binary
// overte-server. Exposes the canonical Overte CodecPlugin interface (the
// framework contract; the interface class names are kept for compatibility),
// but is built directly into the binary rather than loaded as a runtime .so.

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#pragma once

#include <plugins/CodecPlugin.h>

class OpusCodec : public CodecPlugin {
    Q_OBJECT

public:
    // Plugin interface
    bool isSupported() const override;
    const QString getName() const override { return NAME; }

    void init() override;
    void deinit() override;

    bool activate() override;
    void deactivate() override;

    // CodecPlugin interface
    Encoder* createEncoder(int sampleRate, int numChannels) override;
    Decoder* createDecoder(int sampleRate, int numChannels) override;
    void releaseEncoder(Encoder* encoder) override;
    void releaseDecoder(Decoder* decoder) override;

private:
    static const char* NAME;
};

// Factory for statically registering the embedded Opus codec with a
// PluginManager (see PluginManager::setCodecPluginProvider).
CodecPluginList opusCodecPlugins();
