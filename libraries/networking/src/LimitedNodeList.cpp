//
//  LimitedNodeList.cpp
//  libraries/networking/src
//
//  Created by Stephen Birarda on 2/15/13.
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

#include "LimitedNodeList.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <array>

#include <QtCore/QDataStream>
#include <QtCore/QDebug>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <QtCore/QUrl>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QHostInfo>

#include <LogHandler.h>
#include <shared/NetworkUtils.h>
#include <NumericalConstants.h>
#include <SettingHandle.h>
#include <SharedUtil.h>
#include <UUID.h>

#include "AccountManager.h"
#include "AssetClient.h"
#include "Assignment.h"
#include "SockAddr.h"
#include "NetworkLogging.h"
#include "udt/Packet.h"
#include "HMACAuth.h"

#if defined(Q_OS_WIN)
#include <winsock.h>
#else
#include <arpa/inet.h>
#endif

static Setting::Handle<quint16> LIMITED_NODELIST_LOCAL_PORT("LimitedNodeList.LocalPort", 0);

using namespace std::chrono_literals;
static const std::chrono::milliseconds CONNECTION_RATE_INTERVAL_MS = 1s;

LimitedNodeList::LimitedNodeList(int socketListenPort, int dtlsListenPort) :
    _nodeSocket(this, true),
    _packetReceiver(new PacketReceiver(this))
{
    qRegisterMetaType<ConnectionStep>("ConnectionStep");
    auto port = (socketListenPort != INVALID_PORT) ? socketListenPort : LIMITED_NODELIST_LOCAL_PORT.get();
    _nodeSocket.bind(SocketType::UDP, QHostAddress::AnyIPv4, port);
    quint16 assignedPort = _nodeSocket.localPort(SocketType::UDP);
    if (socketListenPort != INVALID_PORT && socketListenPort != 0 && socketListenPort != assignedPort) {
        qCCritical(networking) << "PAGE: NodeList is unable to assign requested UDP port of" << socketListenPort;
    }
    qCDebug(networking) << "NodeList UDP socket is listening on" << assignedPort;

    // NOTE: WebRTC sockets are intentionally not bound here. WebRTC data channels
    // are not compiled into headless builds (WEBRTC_DATA_CHANNELS undefined), so an
    // unconditional bind() would hit the "not recognized" default branch and emit a
    // spurious CRITICAL. When WebRTC is built, the WebRTC socket is managed via its
    // own connection/signalling path.

    if (dtlsListenPort != INVALID_PORT) {
        // only create the DTLS socket during constructor if a custom port is passed
        _dtlsSocket = new QUdpSocket(this);

        _dtlsSocket->bind(QHostAddress::AnyIPv4, dtlsListenPort);
        if (dtlsListenPort != 0 && _dtlsSocket->localPort() != dtlsListenPort) {
            qCDebug(networking) << "NodeList is unable to assign requested DTLS port of" << dtlsListenPort;
        }
        qCDebug(networking) << "NodeList DTLS socket is listening on" << _dtlsSocket->localPort();
    }

    // check for local socket updates every so often
    const int LOCAL_SOCKET_UPDATE_INTERVAL_MSECS = 5 * 1000;
    QTimer* localSocketUpdate = new QTimer(this);
    connect(localSocketUpdate, &QTimer::timeout, this, &LimitedNodeList::updateLocalSocket);
    localSocketUpdate->start(LOCAL_SOCKET_UPDATE_INTERVAL_MSECS);

    QTimer* silentNodeTimer = new QTimer(this);
    connect(silentNodeTimer, &QTimer::timeout, this, &LimitedNodeList::removeSilentNodes);
    silentNodeTimer->start(NODE_SILENCE_THRESHOLD_MSECS);

    const int CONNECTION_STATS_SAMPLE_INTERVAL_MSECS = 1000;
    QTimer* statsSampleTimer = new QTimer(this);
    connect(statsSampleTimer, &QTimer::timeout, this, &LimitedNodeList::sampleConnectionStats);
    statsSampleTimer->start(CONNECTION_STATS_SAMPLE_INTERVAL_MSECS);

    // Flush delayed adds every second
    QTimer* delayedAddsFlushTimer = new QTimer(this);
    connect(delayedAddsFlushTimer, &QTimer::timeout, this, &NodeList::processDelayedAdds);
    delayedAddsFlushTimer->start(CONNECTION_RATE_INTERVAL_MS.count());

    // check the local socket right now
    updateLocalSocket();

    // set &PacketReceiver::handleVerifiedPacket as the verified packet callback for the udt::Socket
    _nodeSocket.setPacketHandler([this](std::unique_ptr<udt::Packet> packet) {
            _packetReceiver->handleVerifiedPacket(std::move(packet));
    });
    _nodeSocket.setMessageHandler([this](std::unique_ptr<udt::Packet> packet) {
            _packetReceiver->handleVerifiedMessagePacket(std::move(packet));
    });
    _nodeSocket.setMessageFailureHandler([this](SockAddr from,
                                                udt::Packet::MessageNumber messageNumber) {
            _packetReceiver->handleMessageFailure(from, messageNumber);
    });

    // set our isPacketVerified method as the verify operator for the udt::Socket
    using std::placeholders::_1;
    _nodeSocket.setPacketFilterOperator(std::bind(&LimitedNodeList::isPacketVerified, this, _1));

    // set our socketBelongsToNode method as the connection creation filter operator for the udt::Socket
    _nodeSocket.setConnectionCreationFilterOperator(std::bind(&LimitedNodeList::sockAddrBelongsToNode, this, _1));

    // handle when a socket connection has its receiver side reset - might need to emit clientConnectionToNodeReset
    connect(&_nodeSocket, &udt::Socket::clientHandshakeRequestComplete, this, &LimitedNodeList::clientConnectionToSockAddrReset);
}

QUuid LimitedNodeList::getSessionUUID() const {
    QReadLocker lock { &_sessionUUIDLock };
    return _sessionUUID;
}

void LimitedNodeList::setSessionUUID(const QUuid& sessionUUID) {
    QUuid oldUUID;
    {
        QWriteLocker lock { &_sessionUUIDLock };
        oldUUID = _sessionUUID;
        _sessionUUID = sessionUUID;
    }

    if (sessionUUID != oldUUID) {
        qCDebug(networking) << "NodeList UUID changed from" <<  uuidStringWithoutCurlyBraces(oldUUID)
        << "to" << uuidStringWithoutCurlyBraces(sessionUUID);
        emit uuidChanged(sessionUUID, oldUUID);
    }
}

Node::LocalID LimitedNodeList::getSessionLocalID() const {
    QReadLocker readLock { &_sessionUUIDLock };
    return _sessionLocalID;
}

void LimitedNodeList::setSessionLocalID(Node::LocalID sessionLocalID) {
    QWriteLocker lock { &_sessionUUIDLock };
    _sessionLocalID = sessionLocalID;
}

void LimitedNodeList::setPermissions(const NodePermissions& newPermissions) {
    NodePermissions originalPermissions = _permissions;

    _permissions = newPermissions;

    if (originalPermissions.can(NodePermissions::Permission::canAdjustLocks) !=
        newPermissions.can(NodePermissions::Permission::canAdjustLocks)) {
        emit isAllowedEditorChanged(_permissions.can(NodePermissions::Permission::canAdjustLocks));
    }
    if (originalPermissions.can(NodePermissions::Permission::canRezPermanentEntities) !=
        newPermissions.can(NodePermissions::Permission::canRezPermanentEntities)) {
        emit canRezChanged(_permissions.can(NodePermissions::Permission::canRezPermanentEntities));
    }
    if (originalPermissions.can(NodePermissions::Permission::canRezTemporaryEntities) !=
        newPermissions.can(NodePermissions::Permission::canRezTemporaryEntities)) {
        emit canRezTmpChanged(_permissions.can(NodePermissions::Permission::canRezTemporaryEntities));
    }
    if (originalPermissions.can(NodePermissions::Permission::canWriteToAssetServer) !=
        newPermissions.can(NodePermissions::Permission::canWriteToAssetServer)) {
        emit canWriteAssetsChanged(_permissions.can(NodePermissions::Permission::canWriteToAssetServer));
    }
    if (originalPermissions.can(NodePermissions::Permission::canKick) !=
        newPermissions.can(NodePermissions::Permission::canKick)) {
        emit canKickChanged(_permissions.can(NodePermissions::Permission::canKick));
    }
    if (originalPermissions.can(NodePermissions::Permission::canReplaceDomainContent) !=
        newPermissions.can(NodePermissions::Permission::canReplaceDomainContent)) {
        emit canReplaceContentChanged(_permissions.can(NodePermissions::Permission::canReplaceDomainContent));
    }
    if (originalPermissions.can(NodePermissions::Permission::canGetAndSetPrivateUserData) !=
        newPermissions.can(NodePermissions::Permission::canGetAndSetPrivateUserData)) {
        emit canGetAndSetPrivateUserDataChanged(_permissions.can(NodePermissions::Permission::canGetAndSetPrivateUserData));
    }
    if (originalPermissions.can(NodePermissions::Permission::canRezAvatarEntities) !=
        newPermissions.can(NodePermissions::Permission::canRezAvatarEntities)) {
        emit canRezAvatarEntitiesChanged(_permissions.can(NodePermissions::Permission::canRezAvatarEntities));
    }
    if (originalPermissions.can(NodePermissions::Permission::canViewAssetURLs) !=
        newPermissions.can(NodePermissions::Permission::canViewAssetURLs)) {
        emit canViewAssetURLsChanged(_permissions.can(NodePermissions::Permission::canViewAssetURLs));
    }
}

void LimitedNodeList::setSocketLocalPort(SocketType socketType, quint16 socketLocalPort) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setSocketLocalPort", Qt::QueuedConnection,
                                  Q_ARG(quint16, socketLocalPort));
        return;
    }
    if (_nodeSocket.localPort(socketType) != socketLocalPort) {
        _nodeSocket.rebind(socketType, socketLocalPort);
        if (socketType == SocketType::UDP) {
            LIMITED_NODELIST_LOCAL_PORT.set(socketLocalPort);
        } else {
            // WEBRTC TODO: Add WebRTC equivalent?
            qCWarning(networking_webrtc) << "LIMITED_NODELIST_LOCAL_PORT not set for WebRTC socket";
        }
    }
}

QUdpSocket& LimitedNodeList::getDTLSSocket() {
    if (!_dtlsSocket) {
        // DTLS socket getter called but no DTLS socket exists, create it now
        _dtlsSocket = new QUdpSocket(this);

        _dtlsSocket->bind(QHostAddress::AnyIPv4, 0, QAbstractSocket::DontShareAddress);

        // we're using DTLS and our socket is good to go, so make the required DTLS changes
        // DTLS requires that IP_DONTFRAG be set
        // This is not accessible on some platforms (OS X) so we need to make sure DTLS still works without it

        qCDebug(networking) << "LimitedNodeList DTLS socket is listening on" << _dtlsSocket->localPort();
    }

    return *_dtlsSocket;
}

#if defined(WEBRTC_DATA_CHANNELS)
const WebRTCSocket* LimitedNodeList::getWebRTCSocket() {
    return _nodeSocket.getWebRTCSocket();
}
#endif

bool LimitedNodeList::isPacketVerifiedWithSource(const udt::Packet& packet, Node* sourceNode) {
    // We track bandwidth when doing packet verification to avoid needing to do a node lookup
    // later when we already do it in packetSourceAndHashMatchAndTrackBandwidth. A node lookup
    // incurs a lock, so it is ideal to avoid needing to do it 2+ times for each packet
    // received.
    return packetVersionMatch(packet) && packetSourceAndHashMatchAndTrackBandwidth(packet, sourceNode);
}

bool LimitedNodeList::packetVersionMatch(const udt::Packet& packet) {
    PacketType headerType = NLPacket::typeInHeader(packet);
    PacketVersion headerVersion = NLPacket::versionInHeader(packet);

    if (headerVersion != versionForPacketType(headerType)) {

        static QMultiHash<QUuid, PacketType> sourcedVersionDebugSuppressMap;
        static QMultiHash<SockAddr, PacketType> versionDebugSuppressMap;

        bool hasBeenOutput = false;
        QString senderString;
        const SockAddr& senderSockAddr = packet.getSenderSockAddr();
        QUuid sourceID;

        if (PacketTypeEnum::getNonSourcedPackets().contains(headerType)) {
            hasBeenOutput = versionDebugSuppressMap.contains(senderSockAddr, headerType);

            if (!hasBeenOutput) {
                versionDebugSuppressMap.insert(senderSockAddr, headerType);
                senderString = QString("%1:%2").arg(senderSockAddr.getAddress().toString()).arg(senderSockAddr.getPort());
            }
        } else {
            SharedNodePointer sourceNode = nodeWithLocalID(NLPacket::sourceIDInHeader(packet));
            if (sourceNode) {
                sourceID = sourceNode->getUUID();

                hasBeenOutput = sourcedVersionDebugSuppressMap.contains(sourceID, headerType);

                if (!hasBeenOutput) {
                    sourcedVersionDebugSuppressMap.insert(sourceID, headerType);
                    senderString = uuidStringWithoutCurlyBraces(sourceID.toString());
                }
            }
        }

        if (!hasBeenOutput) {
            qCDebug(networking) << "Packet version mismatch on" << headerType << "- Sender"
                << senderString << "sent" << qPrintable(QString::number(headerVersion)) << "but"
                << qPrintable(QString::number(versionForPacketType(headerType))) << "expected.";

            emit packetVersionMismatch(headerType, senderSockAddr, sourceID);
        }

        return false;
    } else {
        return true;
    }
}

bool LimitedNodeList::packetSourceAndHashMatchAndTrackBandwidth(const udt::Packet& packet, Node* sourceNode) {

    PacketType headerType = NLPacket::typeInHeader(packet);

    if (PacketTypeEnum::getNonSourcedPackets().contains(headerType)) {
        if (PacketTypeEnum::getReplicatedPacketMapping().key(headerType) != PacketType::Unknown) {
            // this is a replicated packet type - make sure the socket that sent it to us matches
            // one from one of our current upstream nodes

            NodeType_t sendingNodeType { NodeType::Unassigned };

            eachNodeBreakable([&packet, &sendingNodeType](const SharedNodePointer& node){
                if (NodeType::isUpstream(node->getType()) && node->getPublicSocket() == packet.getSenderSockAddr()) {
                    sendingNodeType = node->getType();
                    return false;
                } else {
                    return true;
                }
            });

            if (sendingNodeType != NodeType::Unassigned) {
                return true;
            } else {
                HIFI_FCDEBUG(networking(), "Replicated packet of type" << headerType
                    << "received from unknown upstream" << packet.getSenderSockAddr());

                return false;
            }
        } else {
            return true;
        }
    } else {
        NLPacket::LocalID sourceLocalID = Node::NULL_LOCAL_ID;

        // check if we were passed a sourceNode hint or if we need to look it up
        if (!sourceNode) {
            // figure out which node this is from
            sourceLocalID = NLPacket::sourceIDInHeader(packet);

            SharedNodePointer matchingNode = nodeWithLocalID(sourceLocalID);
            sourceNode = matchingNode.data();
        }

        QUuid sourceID = sourceNode ? sourceNode->getUUID() : QUuid();

        if (!sourceNode &&
            !isDomainServer() &&
            sourceLocalID == getDomainLocalID() &&
            packet.getSenderSockAddr() == getDomainSockAddr() &&
            PacketTypeEnum::getDomainSourcedPackets().contains(headerType)) {
            // This is a packet sourced by the domain server
            return true;
        }

        if (sourceNode) {
            bool verifiedPacket = !PacketTypeEnum::getNonVerifiedPackets().contains(headerType);
            bool verificationEnabled = !(isDomainServer() && PacketTypeEnum::getDomainIgnoredVerificationPackets().contains(headerType))
                && _useAuthentication;

            if (verifiedPacket && verificationEnabled) {

                QByteArray packetHeaderHash = NLPacket::verificationHashInHeader(packet);
                QByteArray expectedHash;
                auto sourceNodeHMACAuth = sourceNode->getAuthenticateHash();
                if (sourceNode->getAuthenticateHash()) {
                    expectedHash = NLPacket::hashForPacketAndHMAC(packet, *sourceNodeHMACAuth);
                }

                // check if the HMAC-md5 hash in the header matches the hash we would expect
                if (!sourceNodeHMACAuth || packetHeaderHash != expectedHash) {
                    static QMultiMap<QUuid, PacketType> hashDebugSuppressMap;

                    if (!hashDebugSuppressMap.contains(sourceID, headerType)) {
                        qCDebug(networking) << "Packet hash mismatch on" << headerType << "- Sender" << sourceID;
                        qCDebug(networking) << "Packet len:" << packet.getDataSize() << "Expected hash:" <<
                            expectedHash.toHex() << "Actual:" << packetHeaderHash.toHex();

                        hashDebugSuppressMap.insert(sourceID, headerType);
                    }

                    return false;
                }
            }

            // No matter if this packet is handled or not, we update the timestamp for the last time we heard
            // from this sending node
            sourceNode->setLastHeardMicrostamp(usecTimestampNow());

            return true;

        } else if (!isDelayedNode(sourceID)){
            HIFI_FCDEBUG(networking(),
                "Packet of type" << headerType << "received from unknown node with Local ID" << sourceLocalID);
        }
    }

    return false;
}

void LimitedNodeList::fillPacketHeader(const NLPacket& packet, HMACAuth* hmacAuth) {
    if (!PacketTypeEnum::getNonSourcedPackets().contains(packet.getType())) {
        packet.writeSourceID(getSessionLocalID());
    }

    if (_useAuthentication && hmacAuth
        && !PacketTypeEnum::getNonSourcedPackets().contains(packet.getType())
        && !PacketTypeEnum::getNonVerifiedPackets().contains(packet.getType())) {
        packet.writeVerificationHash(*hmacAuth);
    }
}

static const qint64 ERROR_SENDING_PACKET_BYTES = -1;

qint64 LimitedNodeList::sendUnreliablePacket(const NLPacket& packet, const Node& destinationNode) {
    Q_ASSERT(!packet.isPartOfMessage());

    if (!destinationNode.getActiveSocket()) {
        return 0;
    }

    return sendUnreliablePacket(packet, *destinationNode.getActiveSocket(), destinationNode.getAuthenticateHash());
}

qint64 LimitedNodeList::sendUnreliablePacket(const NLPacket& packet, const SockAddr& sockAddr,
        HMACAuth* hmacAuth) {
    Q_ASSERT(!packet.isPartOfMessage());
    Q_ASSERT_X(!packet.isReliable(), "LimitedNodeList::sendUnreliablePacket",
               "Trying to send a reliable packet unreliably.");

    if (_dropOutgoingNodeTraffic) {
        auto destinationNode = findNodeWithAddr(sockAddr);

        // findNodeWithAddr returns null for the address of the domain server
        if (!destinationNode.isNull()) {
            // This only suppresses individual unreliable packets, not unreliable packet lists
            return ERROR_SENDING_PACKET_BYTES;
        }
    }

    fillPacketHeader(packet, hmacAuth);

    return _nodeSocket.writePacket(packet, sockAddr);
}

qint64 LimitedNodeList::sendPacket(std::unique_ptr<NLPacket> packet, const Node& destinationNode) {
    Q_ASSERT(!packet->isPartOfMessage());
    auto activeSocket = destinationNode.getActiveSocket();

    if (activeSocket) {
        return sendPacket(std::move(packet), *activeSocket, destinationNode.getAuthenticateHash());
    } else {
        qCDebug(networking) << "LimitedNodeList::sendPacket called without active socket for node" << destinationNode << "- not sending";
        return ERROR_SENDING_PACKET_BYTES;
    }
}

qint64 LimitedNodeList::sendPacket(std::unique_ptr<NLPacket> packet, const SockAddr& sockAddr,
                                   HMACAuth* hmacAuth) {
    Q_ASSERT(!packet->isPartOfMessage());
    if (packet->isReliable()) {
        fillPacketHeader(*packet, hmacAuth);

        auto size = packet->getDataSize();
        _nodeSocket.writePacket(std::move(packet), sockAddr);

        return size;
    } else {
        auto size = sendUnreliablePacket(*packet, sockAddr, hmacAuth);
        if (size < 0) {
            auto now = usecTimestampNow();
            if (now - _sendErrorStatsTime > ERROR_STATS_PERIOD_US) {
                _sendErrorStatsTime = now;
                eachNode([now](const SharedNodePointer& node) {
                    qCDebug(networking) << "Stats for " << node->getPublicSocket() << "\n"
                        << "    Last Heard Microstamp: " << node->getLastHeardMicrostamp() << " (" << (now - node->getLastHeardMicrostamp()) << "usec ago)\n"
                        << "    Outbound Kbps: " << node->getOutboundKbps() << "\n"
                        << "    Inbound Kbps: " << node->getInboundKbps() << "\n"
                        << "    Ping: " << node->getPingMs();
                });
            }
        }
        return size;
    }
}

qint64 LimitedNodeList::sendUnreliableUnorderedPacketList(NLPacketList& packetList, const Node& destinationNode) {
    auto activeSocket = destinationNode.getActiveSocket();

    if (activeSocket) {
        qint64 bytesSent = 0;
        auto connectionHash = destinationNode.getAuthenticateHash();

        // close the last packet in the list
        packetList.closeCurrentPacket();

        while (!packetList._packets.empty()) {
            bytesSent += sendPacket(packetList.takeFront<NLPacket>(), *activeSocket,
                connectionHash);
        }
        return bytesSent;
    } else {
        qCDebug(networking) << "LimitedNodeList::sendUnreliableUnorderedPacketList called without active socket for node"
            << destinationNode << " - not sending.";
        return ERROR_SENDING_PACKET_BYTES;
    }
}

qint64 LimitedNodeList::sendUnreliableUnorderedPacketList(NLPacketList& packetList, const SockAddr& sockAddr,
                                                          HMACAuth* hmacAuth) {
    qint64 bytesSent = 0;

    // close the last packet in the list
    packetList.closeCurrentPacket();

    while (!packetList._packets.empty()) {
        bytesSent += sendPacket(packetList.takeFront<NLPacket>(), sockAddr, hmacAuth);
    }

    return bytesSent;
}

qint64 LimitedNodeList::sendPacketList(std::unique_ptr<NLPacketList> packetList, const SockAddr& sockAddr) {
    // close the last packet in the list
    packetList->closeCurrentPacket();

    for (std::unique_ptr<udt::Packet>& packet : packetList->_packets) {
        NLPacket* nlPacket = static_cast<NLPacket*>(packet.get());
        fillPacketHeader(*nlPacket);
    }

    return _nodeSocket.writePacketList(std::move(packetList), sockAddr);
}

qint64 LimitedNodeList::sendPacketList(std::unique_ptr<NLPacketList> packetList, const Node& destinationNode) {
    auto activeSocket = destinationNode.getActiveSocket();
    if (activeSocket) {
        // close the last packet in the list
        packetList->closeCurrentPacket();

        for (std::unique_ptr<udt::Packet>& packet : packetList->_packets) {
            NLPacket* nlPacket = static_cast<NLPacket*>(packet.get());
            fillPacketHeader(*nlPacket, destinationNode.getAuthenticateHash());
        }

        return _nodeSocket.writePacketList(std::move(packetList), *activeSocket);
    } else {
        qCDebug(networking) << "LimitedNodeList::sendPacketList called without active socket for node "
                            << destinationNode.getUUID() << ". Not sending.";
        return ERROR_SENDING_PACKET_BYTES;
    }
}

qint64 LimitedNodeList::sendPacket(std::unique_ptr<NLPacket> packet, const Node& destinationNode,
                                   const SockAddr& overridenSockAddr) {
    if (overridenSockAddr.isNull() && !destinationNode.getActiveSocket()) {
        qCDebug(networking) << "LimitedNodeList::sendPacket called without active socket for node"
                            << destinationNode.getUUID() << ". Not sending.";
        return ERROR_SENDING_PACKET_BYTES;
    }

    // use the node's active socket as the destination socket if there is no overriden socket address
    auto& destinationSockAddr = (overridenSockAddr.isNull()) ? *destinationNode.getActiveSocket()
                                                             : overridenSockAddr;

    return sendPacket(std::move(packet), destinationSockAddr, destinationNode.getAuthenticateHash());
}

int LimitedNodeList::updateNodeWithDataFromPacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer sendingNode) {

    NodeData* linkedData = getOrCreateLinkedData(sendingNode);

    if (linkedData) {
        QMutexLocker linkedDataLocker(&linkedData->getMutex());
        return linkedData->parseData(*message);
    }

    return 0;
}

NodeData* LimitedNodeList::getOrCreateLinkedData(SharedNodePointer node) {
    QMutexLocker locker(&node->getMutex());

    NodeData* linkedData = node->getLinkedData();
    if (!linkedData && linkedDataCreateCallback) {
        linkedDataCreateCallback(node.data());
    }

    return node->getLinkedData();
}

SharedNodePointer LimitedNodeList::nodeWithUUID(const QUuid& nodeUUID) {
    QReadLocker readLocker(&_nodeMutex);

    NodeHash::const_iterator it = _nodeHash.find(nodeUUID);
    return it == _nodeHash.cend() ? SharedNodePointer() : it->second;
 }

SharedNodePointer LimitedNodeList::nodeWithLocalID(Node::LocalID localID) const {
    QReadLocker readLocker(&_nodeMutex);

    LocalIDMapping::const_iterator idIter = _localIDMap.find(localID);
    return idIter == _localIDMap.cend() ? nullptr : idIter->second;
}

void LimitedNodeList::eraseAllNodes(QString reason) {
    std::vector<SharedNodePointer> killedNodes;

    {
        // iterate the current nodes - grab them so we can emit that they are dying
        // and then remove them from the hash
        QWriteLocker writeLocker(&_nodeMutex);

        if (_nodeHash.size() > 0) {
            qCDebug(networking) << "LimitedNodeList::eraseAllNodes() removing all nodes from NodeList:" << reason;

            killedNodes.reserve(_nodeHash.size());
            for (auto& pair : _nodeHash) {
                killedNodes.push_back(pair.second);
            }
        }
        _localIDMap.clear();
        _nodeHash.clear();
    }

    for(const auto& killedNode : killedNodes) {
        handleNodeKill(killedNode);
    }

    _delayedNodeAdds.clear();
}

void LimitedNodeList::reset(QString reason) {
    eraseAllNodes(reason);

    // we need to make sure any socket connections are gone so wait on that here
    _nodeSocket.clearConnections();
    _connectionIDs.clear();
}

bool LimitedNodeList::killNodeWithUUID(const QUuid& nodeUUID, ConnectionID newConnectionID) {
    auto matchingNode = nodeWithUUID(nodeUUID);

    if (matchingNode) {
        {
            QWriteLocker writeLocker(&_nodeMutex);
            _localIDMap.unsafe_erase(matchingNode->getLocalID());
            _nodeHash.unsafe_erase(matchingNode->getUUID());
        }

        handleNodeKill(matchingNode, newConnectionID);
        return true;
    }

    return false;
}

void LimitedNodeList::processKillNode(ReceivedMessage& message) {
    // read the node id
    QUuid nodeUUID = QUuid::fromRfc4122(message.readWithoutCopy(NUM_BYTES_RFC4122_UUID));

    // kill the node with this UUID, if it exists
    killNodeWithUUID(nodeUUID);
}

void LimitedNodeList::handleNodeKill(const SharedNodePointer& node, ConnectionID nextConnectionID) {
    _nodeDisconnectTimestamp = usecTimestampNow();
    qCDebug(networking) << "Killed" << *node;
    node->stopPingTimer();
    emit nodeKilled(node);

    if (auto activeSocket = node->getActiveSocket()) {
        _nodeSocket.cleanupConnection(*activeSocket);
    }

    auto it = _connectionIDs.find(node->getUUID());
    if (it != _connectionIDs.end()) {
        if (nextConnectionID == NULL_CONNECTION_ID) {
            it->second++;
        } else {
            it->second = nextConnectionID;
        }
    }
}

SharedNodePointer LimitedNodeList::addOrUpdateNode(const QUuid& uuid, NodeType_t nodeType,
                                                   const SockAddr& publicSocket, const SockAddr& localSocket,
                                                   Node::LocalID localID, bool isReplicated, bool isUpstream,
                                                   const QUuid& connectionSecret, const NodePermissions& permissions) {
    auto matchingNode = nodeWithUUID(uuid);
    if (matchingNode) {
        matchingNode->setPublicSocket(publicSocket);
        matchingNode->setLocalSocket(localSocket);
        matchingNode->setPermissions(permissions);
        matchingNode->setConnectionSecret(connectionSecret);
        matchingNode->setIsReplicated(isReplicated);
        matchingNode->setIsUpstream(isUpstream || NodeType::isUpstream(nodeType));
        matchingNode->setLocalID(localID);

        return matchingNode;
    }

    auto removeOldNode = [&](auto node) {
        if (node) {
            {
                QWriteLocker writeLocker(&_nodeMutex);
                _localIDMap.unsafe_erase(node->getLocalID());
                _nodeHash.unsafe_erase(node->getUUID());
            }
            handleNodeKill(node);
        }
    };

    // if this is a solo node type, we assume that the DS has replaced its assignment and we should kill the previous node
    if (SOLO_NODE_TYPES.count(nodeType)) {
        removeOldNode(soloNodeOfType(nodeType));
    }
    // If there is a new node with the same socket, this is a reconnection, kill the old node
    removeOldNode(findNodeWithAddr(publicSocket));
    removeOldNode(findNodeWithAddr(localSocket));
    // If there is an old Connection to the new node's address kill it
    _nodeSocket.cleanupConnection(publicSocket);
    _nodeSocket.cleanupConnection(localSocket);

    auto it = _connectionIDs.find(uuid);
    if (it == _connectionIDs.end()) {
        _connectionIDs[uuid] = INITIAL_CONNECTION_ID;
    }

    // we didn't have this node, so add them
    Node* newNode = new Node(uuid, nodeType, publicSocket, localSocket);
    newNode->setIsReplicated(isReplicated);
    newNode->setIsUpstream(isUpstream || NodeType::isUpstream(nodeType));
    newNode->setConnectionSecret(connectionSecret);
    newNode->setPermissions(permissions);
    newNode->setLocalID(localID);

    // move the newly constructed node to the LNL thread
    newNode->moveToThread(thread());

    if (nodeType == NodeType::AudioMixer) {
        LimitedNodeList::flagTimeForConnectionStep(LimitedNodeList::AddedAudioMixer);
    }

    SharedNodePointer newNodePointer(newNode, &QObject::deleteLater);


    {
        QReadLocker readLocker(&_nodeMutex);
        // insert the new node and release our read lock
        _nodeHash.insert({ newNode->getUUID(), newNodePointer });
        _localIDMap.insert({ localID, newNodePointer });
    }

    qCDebug(networking) << "Added" << *newNode;

    auto weakPtr = newNodePointer.toWeakRef(); // We don't want the lambdas to hold a strong ref

    emit nodeAdded(newNodePointer);
    if (newNodePointer->getActiveSocket()) {
        emit nodeActivated(newNodePointer);
    } else {
        connect(newNodePointer.data(), &NetworkPeer::socketActivated, this, [this, weakPtr] {
            auto sharedPtr = weakPtr.lock();
            if (sharedPtr) {
                emit nodeActivated(sharedPtr);
                disconnect(sharedPtr.data(), &NetworkPeer::socketActivated, this, 0);
            }
        });
    }

    // Signal when a socket changes, so we can start the hole punch over.
    connect(newNodePointer.data(), &NetworkPeer::socketUpdated, this, [this, weakPtr] {
        emit nodeSocketUpdated(weakPtr);
    });
    connect(newNodePointer.data(), &NetworkPeer::socketUpdated, &_nodeSocket, &udt::Socket::handleRemoteAddressChange);

    return newNodePointer;
}

void LimitedNodeList::addNewNode(NewNodeInfo info) {
    // Throttle connection of new agents.
    if (info.type == NodeType::Agent && _nodesAddedInCurrentTimeSlice >= _maxConnectionRate) {
        delayNodeAdd(info);
        return;
    }

    SharedNodePointer node = addOrUpdateNode(info.uuid, info.type, info.publicSocket, info.localSocket,
                                             info.sessionLocalID, info.isReplicated, false,
                                             info.connectionSecretUUID, info.permissions);

    ++_nodesAddedInCurrentTimeSlice;
}

void LimitedNodeList::delayNodeAdd(NewNodeInfo info) {
    _delayedNodeAdds.push_back(info);
}

void LimitedNodeList::removeDelayedAdd(QUuid nodeUUID) {
    auto it = std::find_if(_delayedNodeAdds.begin(), _delayedNodeAdds.end(), [&](const auto& info) {
        return info.uuid == nodeUUID;
    });
    if (it != _delayedNodeAdds.end()) {
        _delayedNodeAdds.erase(it);
    }
}

bool LimitedNodeList::isDelayedNode(QUuid nodeUUID) {
    auto it = std::find_if(_delayedNodeAdds.begin(), _delayedNodeAdds.end(), [&](const auto& info) {
        return info.uuid == nodeUUID;
    });
    return it != _delayedNodeAdds.end();
}

void LimitedNodeList::processDelayedAdds() {
    _nodesAddedInCurrentTimeSlice = 0;

    auto nodesToAdd = glm::min(_delayedNodeAdds.size(), _maxConnectionRate);
    auto firstNodeToAdd = _delayedNodeAdds.begin();
    auto lastNodeToAdd = firstNodeToAdd + nodesToAdd;

    for (auto it = firstNodeToAdd; it != lastNodeToAdd; ++it) {
        addNewNode(*it);
    }
    _delayedNodeAdds.erase(firstNodeToAdd, lastNodeToAdd);
}

std::unique_ptr<NLPacket> LimitedNodeList::constructPingPacket(const QUuid& nodeId, PingType_t pingType) {
    int packetSize = sizeof(PingType_t) + sizeof(quint64) + sizeof(int64_t);

    auto pingPacket = NLPacket::create(PacketType::Ping, packetSize);
    pingPacket->writePrimitive(pingType);
    pingPacket->writePrimitive(usecTimestampNow());
    pingPacket->writePrimitive(_connectionIDs[nodeId]);

    return pingPacket;
}

std::unique_ptr<NLPacket> LimitedNodeList::constructPingReplyPacket(ReceivedMessage& message) {
    PingType_t typeFromOriginalPing;
    quint64 timeFromOriginalPing;
    message.readPrimitive(&typeFromOriginalPing);
    message.readPrimitive(&timeFromOriginalPing);

    int packetSize = sizeof(PingType_t) + sizeof(quint64) + sizeof(quint64);
    auto replyPacket = NLPacket::create(PacketType::PingReply, packetSize);
    replyPacket->writePrimitive(typeFromOriginalPing);
    replyPacket->writePrimitive(timeFromOriginalPing);
    replyPacket->writePrimitive(usecTimestampNow());

    return replyPacket;
}

std::unique_ptr<NLPacket> LimitedNodeList::constructICEPingPacket(PingType_t pingType, const QUuid& iceID) {
    int packetSize = NUM_BYTES_RFC4122_UUID + sizeof(PingType_t);

    auto icePingPacket = NLPacket::create(PacketType::ICEPing, packetSize);
    icePingPacket->write(iceID.toRfc4122());
    icePingPacket->writePrimitive(pingType);

    return icePingPacket;
}

std::unique_ptr<NLPacket> LimitedNodeList::constructICEPingReplyPacket(ReceivedMessage& message, const QUuid& iceID) {
    // pull out the ping type so we can reply back with that
    PingType_t pingType;

    memcpy(&pingType, message.getRawMessage() + NUM_BYTES_RFC4122_UUID, sizeof(PingType_t));

    int packetSize = NUM_BYTES_RFC4122_UUID + sizeof(PingType_t);
    auto icePingReplyPacket = NLPacket::create(PacketType::ICEPingReply, packetSize);

    // pack the ICE ID and then the ping type
    icePingReplyPacket->write(iceID.toRfc4122());
    icePingReplyPacket->writePrimitive(pingType);

    return icePingReplyPacket;
}

unsigned int LimitedNodeList::broadcastToNodes(std::unique_ptr<NLPacket> packet, const NodeSet& destinationNodeTypes) {
    unsigned int n = 0;

    eachNode([&](const SharedNodePointer& node){
        if (node && destinationNodeTypes.contains(node->getType())) {
            if (packet->isReliable()) {
                auto packetCopy = NLPacket::createCopy(*packet);
                sendPacket(std::move(packetCopy), *node);
            } else {
                sendUnreliablePacket(*packet, *node);
            }
            ++n;
        }
    });

    return n;
}

SharedNodePointer LimitedNodeList::soloNodeOfType(NodeType_t nodeType) {
    return nodeMatchingPredicate([&](const SharedNodePointer& node){
        return node->getType() == nodeType;
    });
}

void LimitedNodeList::removeSilentNodes() {

    QSet<SharedNodePointer> killedNodes;

    auto startedAt = usecTimestampNow();

    eachNodeHashIterator([&](NodeHash::iterator& it){
        SharedNodePointer node = it->second;
        node->getMutex().lock();

        if (!node->isForcedNeverSilent()
            && (usecTimestampNow() - node->getLastHeardMicrostamp()) > (NODE_SILENCE_THRESHOLD_MSECS * USECS_PER_MSEC)) {
            // call the NodeHash erase to get rid of this node
            _localIDMap.unsafe_erase(node->getLocalID());
            it = _nodeHash.unsafe_erase(it);

            killedNodes.insert(node);
        } else {
            // we didn't erase this node, push the iterator forwards
            ++it;
        }

        node->getMutex().unlock();
    });

    for(const auto& killedNode : killedNodes) {
        auto now = usecTimestampNow();
        qCDebug(networking_ice) << "Removing silent node" << *killedNode << "\n"
            << "    Now: " << now << "\n"
            << "    Started at: " << startedAt << " (" << (now - startedAt) << "us ago)\n"
            << "    Last Heard Microstamp: " << killedNode->getLastHeardMicrostamp() << " (" << (now - killedNode->getLastHeardMicrostamp()) << "us ago)\n"
            << "    Forced Never Silent: " << killedNode->isForcedNeverSilent() << "\n"
            << "    Inbound PPS: " << killedNode->getInboundPPS() << "\n"
            << "    Inbound Kbps: " << killedNode->getInboundKbps() << "\n"
            << "    Ping: " << killedNode->getPingMs();
        handleNodeKill(killedNode);
    }
}

void LimitedNodeList::sampleConnectionStats() {
    uint32_t packetsIn { 0 };
    uint32_t packetsOut { 0 };
    uint64_t bytesIn { 0 };
    uint64_t bytesOut { 0 };
    int elapsedSum { 0 };
    int elapsedCount { 0 };

    auto allStats = _nodeSocket.sampleStatsForAllConnections();
    for (const auto& stats : allStats) {
        auto node = findNodeWithAddr(stats.first);
        if (node && node->getActiveSocket() &&
            *node->getActiveSocket() == stats.first) {
            node->updateStats(stats.second);
        }

        packetsIn += stats.second.receivedPackets;
        packetsIn += stats.second.receivedUnreliablePackets;
        packetsOut += stats.second.sentPackets;
        packetsOut += stats.second.sentUnreliablePackets;
        bytesIn += stats.second.receivedBytes;
        bytesIn += stats.second.receivedUnreliableBytes;
        bytesOut += stats.second.sentBytes;
        bytesOut += stats.second.sentUnreliableBytes;
        elapsedSum += (stats.second.endTime - stats.second.startTime).count();
        elapsedCount++;
    }

    if (elapsedCount > 0) {
        float elapsedAvg = (float)elapsedSum / elapsedCount;
        float factor = USECS_PER_SECOND / elapsedAvg;

        float kilobitsReceived = (float)bytesIn * BITS_IN_BYTE / BYTES_PER_KILOBYTE;
        float kilobitsSent = (float)bytesOut * BITS_IN_BYTE / BYTES_PER_KILOBYTE;

        _inboundPPS = packetsIn * factor;
        _outboundPPS = packetsOut * factor;
        _inboundKbps = kilobitsReceived * factor;
        _outboundKbps = kilobitsSent * factor;
    } else {
        _inboundPPS = 0;
        _outboundPPS = 0;
        _inboundKbps = 0.0f;
        _outboundKbps = 0.0f;
    }
}

namespace {
    // Public, widely-reachable DNS recursive resolvers used only to learn the outbound
    // (egress) source IP of this machine so its P2P reachability can be advertised. They are
    // tried in order; the first that accepts a TCP:53 connection supplies the source address.
    // Composed of several independent operators (including non-profit resolvers such as
    // Quad9 and DNS.WATCH) so we do not depend on any single external party.
    const std::array<const char*, 12> RELIABLE_LOCAL_IP_CHECK_HOSTS = {
        "9.9.9.9",          // Quad9 (non-profit foundation)
        "149.112.112.112",  // Quad9
        "84.200.69.80",     // DNS.WATCH (non-profit)
        "84.200.70.40",     // DNS.WATCH
        "8.8.8.8",          // Google
        "8.8.4.4",          // Google
        "1.1.1.1",          // Cloudflare
        "1.0.0.1",          // Cloudflare
        "94.140.14.14",     // AdGuard
        "94.140.15.15",     // AdGuard
        "208.67.222.222",   // OpenDNS (Cisco)
        "208.67.220.220"    // OpenDNS
    };
    constexpr int RELIABLE_LOCAL_IP_CHECK_PORT = 53;
}

void LimitedNodeList::updateLocalSocket() {
    // when update is called, if the local socket is empty then start with the guessed local socket
    if (_localSockAddr.isNull()) {
        setLocalSocket(SockAddr { SocketType::UDP, getGuessedLocalAddress(), _nodeSocket.localPort(SocketType::UDP) });
    }

    // start a fresh attempt at the first candidate
    _localSocketCheckHostIndex = 0;
    attemptLocalSocketCheckForward();
}

void LimitedNodeList::attemptLocalSocketCheckForward() {
    // cancel any check that was still in-flight from a previous update cycle
    if (_localSocketCheckSocket) {
        _localSocketCheckSocket->abort();
        _localSocketCheckSocket->deleteLater();
        _localSocketCheckSocket.clear();
    }

    if (_localSocketCheckHostIndex < (int)RELIABLE_LOCAL_IP_CHECK_HOSTS.size()) {
        QTcpSocket* localIPTestSocket = new QTcpSocket;
        _localSocketCheckSocket = localIPTestSocket;

        connect(localIPTestSocket, &QTcpSocket::connected, this, &LimitedNodeList::connectedForLocalSocketTest);
        connect(localIPTestSocket, &QTcpSocket::errorOccurred, this, &LimitedNodeList::errorTestingLocalSocket);

        localIPTestSocket->connectToHost(QHostAddress(RELIABLE_LOCAL_IP_CHECK_HOSTS[_localSocketCheckHostIndex]),
                                         RELIABLE_LOCAL_IP_CHECK_PORT);
    } else {
        // all candidates failed - fall back to the guessed local address
        if (!_hasTCPCheckedLocalSocket) {
            qCWarning(networking) << "Unable to reach any local-IP check host over TCP, falling back to guessed local address"
                << getLocalSockAddr();
        }
        _localSocketCheckHostIndex = 0;
        setLocalSocket(SockAddr { SocketType::UDP, getGuessedLocalAddress(), _nodeSocket.localPort(SocketType::UDP) });
    }
}

void LimitedNodeList::connectedForLocalSocketTest() {
    auto localIPTestSocket = qobject_cast<QTcpSocket*>(sender());

    if (localIPTestSocket) {
        auto localHostAddress = localIPTestSocket->localAddress();

        if (localHostAddress.protocol() == QAbstractSocket::IPv4Protocol) {
            setLocalSocket(SockAddr { SocketType::UDP, localHostAddress, _nodeSocket.localPort(SocketType::UDP) });
            _hasTCPCheckedLocalSocket = true;
        }

        if (_localSocketCheckSocket == localIPTestSocket) {
            _localSocketCheckSocket.clear();
        }
        localIPTestSocket->deleteLater();

        // this update cycle is done
        _localSocketCheckHostIndex = 0;
    }
}

void LimitedNodeList::errorTestingLocalSocket() {
    auto localIPTestSocket = qobject_cast<QTcpSocket*>(sender());

    if (localIPTestSocket) {
        if (_localSocketCheckSocket == localIPTestSocket) {
            _localSocketCheckSocket.clear();
        }
        localIPTestSocket->deleteLater();

        // move on to the next candidate
        ++_localSocketCheckHostIndex;
        attemptLocalSocketCheckForward();
    }
}

void LimitedNodeList::setLocalSocket(const SockAddr& sockAddr) {
    if (sockAddr.getAddress() != _localSockAddr.getAddress()) {

        if (_localSockAddr.isNull()) {
            qCInfo(networking) << "Local socket is" << sockAddr;
            _localSockAddr = sockAddr;
        } else {
            qCInfo(networking) << "Local socket has changed from" << _localSockAddr << "to" << sockAddr;
            _localSockAddr = sockAddr;
            if (_hasTCPCheckedLocalSocket) {  // Force a port change for NAT:
                reset("local socket change");
                _nodeSocket.rebind(SocketType::UDP, 0);
                _localSockAddr.setPort(_nodeSocket.localPort(SocketType::UDP));
                qCInfo(networking) << "Local port changed to" << _localSockAddr.getPort();
            }
        }

        emit localSockAddrChanged(_localSockAddr);
    }

    // The reachable public socket of this machine is simply its own local socket, since
    // public (NAT-mapped) socket discovery via STUN has been removed. Without this, a null
    // public socket would cause the node to refuse to send its domain-server check-in, so it
    // could never register with the domain server.
    if (_publicSockAddr.getAddress() != sockAddr.getAddress()) {
        _publicSockAddr = sockAddr;
    }
}

void LimitedNodeList::sendPeerQueryToIceServer(const SockAddr& iceServerSockAddr, const QUuid& clientID,
                                               const QUuid& peerID) {
    sendPacketToIceServer(PacketType::ICEServerQuery, iceServerSockAddr, clientID, peerID);
}

SharedNodePointer LimitedNodeList::findNodeWithAddr(const SockAddr& addr) {
    QReadLocker locker(&_nodeMutex);
    auto it = std::find_if(std::begin(_nodeHash), std::end(_nodeHash), [&addr](const UUIDNodePair& pair) {
        return pair.second->getPublicSocket() == addr
            || pair.second->getLocalSocket() == addr
            || pair.second->getSymmetricSocket() == addr;
    });
    return (it != std::end(_nodeHash)) ? it->second : SharedNodePointer();
}

bool LimitedNodeList::sockAddrBelongsToNode(const SockAddr& sockAddr) {
    QReadLocker locker(&_nodeMutex);
    auto it = std::find_if(std::begin(_nodeHash), std::end(_nodeHash), [&sockAddr](const UUIDNodePair& pair) {
        return pair.second->getPublicSocket() == sockAddr
            || pair.second->getLocalSocket() == sockAddr
            || pair.second->getSymmetricSocket() == sockAddr;
    });
    return it != std::end(_nodeHash);
}

void LimitedNodeList::sendPacketToIceServer(PacketType packetType, const SockAddr& iceServerSockAddr,
                                            const QUuid& clientID, const QUuid& peerID) {
    auto icePacket = NLPacket::create(packetType);

    QDataStream iceDataStream(icePacket.get());
    iceDataStream << clientID << _publicSockAddr << _localSockAddr;

    if (packetType == PacketType::ICEServerQuery) {
        assert(!peerID.isNull());

        iceDataStream << peerID;

        qCDebug(networking_ice) << "Sending packet to ICE server to request connection info for peer with ID"
            << uuidStringWithoutCurlyBraces(peerID);
    }

    sendPacket(std::move(icePacket), iceServerSockAddr);
}

void LimitedNodeList::flagTimeForConnectionStep(ConnectionStep connectionStep) {
    quint64 timestamp = usecTimestampNow();
    QMetaObject::invokeMethod(this, "flagTimeForConnectionStep",
                              Q_ARG(ConnectionStep, connectionStep),
                              Q_ARG(quint64, timestamp));
}

void LimitedNodeList::flagTimeForConnectionStep(ConnectionStep connectionStep, quint64 timestamp) {
    if (!_flagTimeForConnectionStep) {
        // this is only true in interface
        return;
    }
    if (connectionStep == ConnectionStep::LookupAddress) {
        QWriteLocker writeLock(&_connectionTimeLock);

        // we clear the current times if the user just fired off a lookup
        _lastConnectionTimes.clear();
        _areConnectionTimesComplete = false;

        _lastConnectionTimes[timestamp] = connectionStep;
    } else if (!_areConnectionTimesComplete) {
        QWriteLocker writeLock(&_connectionTimeLock);


        // anything > than sending the first DS check should not come before the DS check in, so we drop those
        // this handles the case where you lookup an address and get packets in the existing domain before changing domains
        if (connectionStep > LimitedNodeList::ConnectionStep::SendDSCheckIn
            && (_lastConnectionTimes.key(ConnectionStep::SendDSCheckIn) == 0
                || timestamp <= _lastConnectionTimes.key(ConnectionStep::SendDSCheckIn))) {
            return;
        }

        // if there is no time for existing step add a timestamp on the first call for each ConnectionStep
        _lastConnectionTimes[timestamp] = connectionStep;

        // if this is a received audio packet we consider our connection times complete
        if (connectionStep == ConnectionStep::ReceiveFirstAudioPacket) {
            _areConnectionTimesComplete = true;
        }
    }
}

void LimitedNodeList::clientConnectionToSockAddrReset(const SockAddr& sockAddr) {
    // for certain reliable channels higher level classes may need to know if the udt::Connection has been reset
    auto matchingNode = findNodeWithAddr(sockAddr);

    if (matchingNode) {
        emit clientConnectionToNodeReset(matchingNode);
    }
}

#if (PR_BUILD || DEV_BUILD)

void LimitedNodeList::sendFakedHandshakeRequestToNode(SharedNodePointer node) {

    if (node && node->getActiveSocket()) {
        _nodeSocket.sendFakedHandshakeRequest(*node->getActiveSocket());
    }
}

#endif
