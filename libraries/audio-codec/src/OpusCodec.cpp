// Copyright 2026
// SPDX-License-Identifier: Apache-2.0

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#include "OpusCodec.h"

#include "OpusEncoder.h"
#include "OpusDecoder.h"

const char* OpusCodec::NAME { "opus" };

void OpusCodec::init() {
}

void OpusCodec::deinit() {
}

bool OpusCodec::activate() {
    CodecPlugin::activate();
    return true;
}

void OpusCodec::deactivate() {
    CodecPlugin::deactivate();
}

bool OpusCodec::isSupported() const {
    return true;
}

Encoder* OpusCodec::createEncoder(int sampleRate, int numChannels) {
    return new OpusEncoder(sampleRate, numChannels);
}

Decoder* OpusCodec::createDecoder(int sampleRate, int numChannels) {
    return new OpusDecoder(sampleRate, numChannels);
}

void OpusCodec::releaseEncoder(Encoder* encoder) {
    delete encoder;
}

void OpusCodec::releaseDecoder(Decoder* decoder) {
    delete decoder;
}

CodecPluginList opusCodecPlugins() {
    CodecPluginPointer opusCodec(std::make_shared<OpusCodec>());
    if (opusCodec->isSupported()) {
        return { opusCodec };
    }
    return {};
}
