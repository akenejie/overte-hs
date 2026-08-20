//
//  AssignmentFactory.cpp
//  assignment-client/src
//
//  Created by Stephen Birarda on 9/17/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AssignmentFactory.h"

#include <cstdio>
#include <udt/PacketHeaders.h>

#include "Agent.h"
#include "assets/AssetServer.h"
#include "audio/AudioMixer.h"
#include "avatars/AvatarMixer.h"
#include "entities/EntityServer.h"
#include "messages/MessagesMixer.h"
#include "scripts/EntityScriptServer.h"

ThreadedAssignment* AssignmentFactory::unpackAssignment(ReceivedMessage& message) {

    quint8 packedType;
    if (message.peekPrimitive(&packedType) != sizeof(packedType)) {
        fprintf(stderr, "[CRASH-DBG] unpackAssignment: peekPrimitive failed\n");
        fflush(stderr);
        return nullptr;
    }

    Assignment::Type unpackedType = (Assignment::Type) packedType;
    fprintf(stderr, "[CRASH-DBG] unpackAssignment: type=%d\n", (int)unpackedType);
    fflush(stderr);

    switch (unpackedType) {
        case Assignment::AudioMixerType:
            fprintf(stderr, "[CRASH-DBG] unpackAssignment: new AudioMixer\n"); fflush(stderr);
            return new AudioMixer(message);
        case Assignment::AvatarMixerType:
            fprintf(stderr, "[CRASH-DBG] unpackAssignment: new AvatarMixer\n"); fflush(stderr);
            return new AvatarMixer(message);
        case Assignment::AgentType:
            fprintf(stderr, "[CRASH-DBG] unpackAssignment: new Agent\n"); fflush(stderr);
            return new Agent(message);
        case Assignment::EntityServerType:
            fprintf(stderr, "[CRASH-DBG] unpackAssignment: new EntityServer\n"); fflush(stderr);
            return new EntityServer(message);
        case Assignment::AssetServerType:
            fprintf(stderr, "[CRASH-DBG] unpackAssignment: new AssetServer\n"); fflush(stderr);
            return new AssetServer(message);
        case Assignment::MessagesMixerType:
            fprintf(stderr, "[CRASH-DBG] unpackAssignment: new MessagesMixer\n"); fflush(stderr);
            return new MessagesMixer(message);
        case Assignment::EntityScriptServerType:
            fprintf(stderr, "[CRASH-DBG] unpackAssignment: new EntityScriptServer\n"); fflush(stderr);
            return new EntityScriptServer(message);
        default:
            fprintf(stderr, "[CRASH-DBG] unpackAssignment: unknown type %d\n", (int)unpackedType); fflush(stderr);
            return nullptr;
    }
}
