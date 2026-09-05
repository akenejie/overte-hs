//
//  AudioStreamStats.h
//  libraries/audio/src
//
//  Created by Yixin Wang on 6/25/2014
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#ifndef hifi_AudioStreamStats_h
#define hifi_AudioStreamStats_h

#include "SequenceNumberStats.h"

// This struct is copied verbatim onto messages (see NLPacket::writePrimitive in
// AudioIOStats.cpp), so its in-memory layout must be identical on 32-bit and
// 64-bit builds. The quint64 members are naturally 8-byte aligned on 64-bit but
// only 4-byte aligned on 32-bit, which would offset every later member. Force
// the same 8-byte alignment everywhere so hosts of either bitness interoperate.
#if defined(_MSC_VER)
#define HIFI_AUDIO_STREAM_8BYTE_ALIGN __declspec(align(8))
#else
#define HIFI_AUDIO_STREAM_8BYTE_ALIGN __attribute__((aligned(8)))
#endif

class AudioStreamStats {
public:
    // Intermediate packets should have no flag set
    // Unique packets should have both flags set
    enum AppendFlag : quint8 {
        START = 1,
        END = 2
    };

    AudioStreamStats()
        : _streamType(-1),
        _streamIdentifier(),
        _timeGapMin(0),
        _timeGapMax(0),
        _timeGapAverage(0.0f),
        _timeGapWindowMin(0),
        _timeGapWindowMax(0),
        _timeGapWindowAverage(0.0f),
        _framesAvailable(0),
        _framesAvailableAverage(0),
        _desiredJitterBufferFrames(0),
        _starveCount(0),
        _consecutiveNotMixedCount(0),
        _overflowCount(0),
        _framesDropped(0),
        _packetStreamStats(),
        _packetStreamWindowStats()
    {}

    qint32 _streamType;
    QUuid _streamIdentifier;

    HIFI_AUDIO_STREAM_8BYTE_ALIGN quint64 _timeGapMin;
    HIFI_AUDIO_STREAM_8BYTE_ALIGN quint64 _timeGapMax;
    float _timeGapAverage;
    HIFI_AUDIO_STREAM_8BYTE_ALIGN quint64 _timeGapWindowMin;
    HIFI_AUDIO_STREAM_8BYTE_ALIGN quint64 _timeGapWindowMax;
    float _timeGapWindowAverage;

    quint32 _framesAvailable;
    quint16 _framesAvailableAverage;
    quint16 _unplayedMs;
    quint16 _desiredJitterBufferFrames;
    quint32 _starveCount;
    quint32 _consecutiveNotMixedCount;
    quint32 _overflowCount;
    quint32 _framesDropped;

    PacketStreamStats _packetStreamStats;
    PacketStreamStats _packetStreamWindowStats;
};

static_assert(sizeof(AudioStreamStats) == 152,
              "AudioStreamStats size isn't right (struct is sent verbatim over the wire; "
              "it must stay 152 bytes on both 32-bit and 64-bit builds)");

#endif  // hifi_AudioStreamStats_h
