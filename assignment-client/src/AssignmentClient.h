//
//  AssignmentClient.h
//  assignment-client/src
//
//  Created by Stephen Birarda on 11/25/2013.
//  Copyright 2013 High Fidelity, Inc.
//  Copyright 2021 Vircadia contributors.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#ifndef hifi_AssignmentClient_h
#define hifi_AssignmentClient_h

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QSharedPointer>

#include <shared/WebRTC.h>

#include "ThreadedAssignment.h"

class AssignmentClient : public QObject {
    Q_OBJECT
public:
    AssignmentClient(Assignment::Type requestAssignmentType, QString assignmentPool,
                     quint16 listenPort, QString assignmentServerHostname,
                     quint16 assignmentServerPort, quint16 assignmentMonitorPort);
    ~AssignmentClient();

public slots:
    void aboutToQuit();

private slots:
    void sendAssignmentRequest();
    void assignmentCompleted();
    void handleAuthenticationRequest();
    void sendStatusPacketToACM();
    void stopAssignmentClient();
    void handleCreateAssignmentPacket(QSharedPointer<ReceivedMessage> message);
    void handleStopNodePacket(QSharedPointer<ReceivedMessage> message);
#if defined(WEBRTC_DATA_CHANNELS)
    void handleWebRTCSignalingPacket(QSharedPointer<ReceivedMessage> message);
    void sendSignalingMessageToUserClient(const QJsonObject& json);
#endif

signals:
#if defined(WEBRTC_DATA_CHANNELS)
    void webrtcSignalingMessageFromUserClient(const QJsonObject& json);
#endif

private:
    void setUpStatusToMonitor();

    Assignment _requestAssignment;
    QPointer<ThreadedAssignment> _currentAssignment;
    bool _isAssigned { false };
    QString _assignmentServerHostname;
    SockAddr _assignmentServerSocket;
    QTimer _requestTimer; // timer for requesting and assignment
    QTimer _statsTimerACM; // timer for sending stats to assignment client monitor
    QUuid _childAssignmentUUID = QUuid::createUuid();

 protected:
    SockAddr _assignmentClientMonitorSocket;
};

#endif // hifi_AssignmentClient_h
