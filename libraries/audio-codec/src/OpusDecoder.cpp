// Copyright 2026
// SPDX-License-Identifier: Apache-2.0

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#include <PerfStat.h>
#include <QtCore/QLoggingCategory>
#include <AudioConstants.h>

#include "OpusDecoder.h"

static QLoggingCategory decoder("OpusAudioDecoder");

static QString errorToString(int error) {
    switch (error) {
        case OPUS_OK:
            return "OK";
        case OPUS_BAD_ARG:
            return "One or more invalid/out of range arguments.";
        case OPUS_BUFFER_TOO_SMALL:
            return "The mode struct passed is invalid.";
        case OPUS_INTERNAL_ERROR:
            return "An internal error was detected.";
        case OPUS_INVALID_PACKET:
            return "The compressed data passed is corrupted.";
        case OPUS_UNIMPLEMENTED:
            return "Invalid/unsupported request number.";
        case OPUS_INVALID_STATE:
            return "An encoder or decoder structure is invalid or already freed.";
        default:
            return QString("Unknown error code: %1").arg(error);
    }
}

OpusDecoder::OpusDecoder(int sampleRate, int numChannels) {
    int error;

    _opusSampleRate = sampleRate;
    _opusNumChannels = numChannels;

    _decoder = opus_decoder_create(sampleRate, numChannels, &error);

    if (error != OPUS_OK) {
        qCCritical(decoder) << "Failed to initialize Opus decoder: " << errorToString(error);
        _decoder = nullptr;
        return;
    }

    qCDebug(decoder) << "Opus decoder initialized, sampleRate = " << sampleRate << "; numChannels = " << numChannels;
}

OpusDecoder::~OpusDecoder() {
    if (_decoder) {
        opus_decoder_destroy(_decoder);
    }
}

void OpusDecoder::decode(const QByteArray& encodedBuffer, QByteArray& decodedBuffer) {
    assert(_decoder);
    PerformanceTimer perfTimer("OpusDecoder::decode");

    // The audio system encodes and decodes always in fixed size chunks
    int bufferSize = AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL * static_cast<int>(sizeof(int16_t))
        * _opusNumChannels;

    decodedBuffer.resize(bufferSize);
    int bufferFrames = decodedBuffer.size() / _opusNumChannels / static_cast<int>(sizeof(opus_int16));
    int decodedFrames = opus_decode(_decoder, reinterpret_cast<const unsigned char*>(encodedBuffer.data()),
        encodedBuffer.length(), reinterpret_cast<opus_int16*>(decodedBuffer.data()), bufferFrames, 0);

    if (decodedFrames >= 0) {
        if (decodedFrames < bufferFrames) {
            qCWarning(decoder) << "Opus decoder returned " << decodedFrames << ", but " << bufferFrames
                << " were expected!";

            int start = decodedFrames * static_cast<int>(sizeof(int16_t)) * _opusNumChannels;
            memset(&decodedBuffer.data()[start], 0, static_cast<size_t>(decodedBuffer.length() - start));
        } else if (decodedFrames > bufferFrames) {
            // This should never happen
            qCCritical(decoder) << "Opus decoder returned " << decodedFrames << ", but only " << bufferFrames
                << " were expected! Buffer overflow!?";
        }
    } else {
        qCCritical(decoder) << "Failed to decode audio: " << errorToString(decodedFrames);
        decodedBuffer.fill('\0');
    }
}

void OpusDecoder::lostFrame(QByteArray& decodedBuffer) {
    assert(_decoder);

    PerformanceTimer perfTimer("OpusDecoder::lostFrame");

    int bufferSize = AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL * static_cast<int>(sizeof(int16_t))
        * _opusNumChannels;
    decodedBuffer.resize(bufferSize);
    int bufferFrames = decodedBuffer.size() / _opusNumChannels / static_cast<int>(sizeof(opus_int16));

    int decodedFrames = opus_decode(_decoder, nullptr, 0, reinterpret_cast<opus_int16*>(decodedBuffer.data()),
        bufferFrames, 1);

    if (decodedFrames >= 0) {
        if (decodedFrames < bufferFrames) {
            qCWarning(decoder) << "Opus decoder returned " << decodedFrames << ", but " << bufferFrames
                << " were expected!";

            int start = decodedFrames * static_cast<int>(sizeof(int16_t)) * _opusNumChannels;
            memset(&decodedBuffer.data()[start], 0, static_cast<size_t>(decodedBuffer.length() - start));
        } else if (decodedFrames > bufferFrames) {
            // This should never happen
            qCCritical(decoder) << "Opus decoder returned " << decodedFrames << ", but only " << bufferFrames
                << " were expected! Buffer overflow!?";
        }
    } else {
        qCCritical(decoder) << "Failed to decode lost frame: " << errorToString(decodedFrames);
        decodedBuffer.fill('\0');
    }
}
