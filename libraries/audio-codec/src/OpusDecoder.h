// Copyright 2026
// SPDX-License-Identifier: Apache-2.0

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#pragma once

#include <plugins/CodecPlugin.h>
#include <opus/opus.h>

class OpusDecoder : public Decoder {
public:
    OpusDecoder(int sampleRate, int numChannels);
    ~OpusDecoder() override;

    void decode(const QByteArray& encodedBuffer, QByteArray& decodedBuffer) override;
    void lostFrame(QByteArray& decodedBuffer) override;

private:
    int _encodedSize { 0 };
    OpusDecoder* _decoder { nullptr };
    int _opusSampleRate { 0 };
    int _opusNumChannels { 0 };
    int _decodedSize { 0 };
};
