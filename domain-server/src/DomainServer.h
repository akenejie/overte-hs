//
//  DomainServer.h
//  domain-server/src
//
//  Created by Stephen Birarda on 9/26/13.
//  Copyright 2013 High Fidelity, Inc.
//  Copyright 2020 Vircadia contributors.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_DomainServer_h
#define hifi_DomainServer_h

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QQueue>
#include <QtCore/QSharedPointer>
#include <QtCore/QStringList>
#include <QtCore/QThread>
#include <QtCore/QUrl>
#include <QHostAddress>
#include <QAbstractNativeEventFilter>
#include <QTemporaryFile>

#include <Assignment.h>
#include <LimitedNodeList.h>
#ifndef OVERTE_HEADLESS
#include <HTTPSConnection.h>
#include <shared/WebRTC.h>
#include <webrtc/WebRTCSignalingServer.h>
#endif

#ifndef OVERTE_HEADLESS
#include "AssetsBackupHandler.h"
#endif
#include "DomainGatekeeper.h"
#include "DomainMetadata.h"
#include "DomainServerSettingsManager.h"
#ifndef OVERTE_HEADLESS
#include "DomainServerWebSessionData.h"
#include "DomainContentBackupManager.h"
#endif

#include "PendingAssignedNodeData.h"
#ifndef OVERTE_HEADLESS
#include "DomainServerExporter.h"
#endif

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(domain_server)
Q_DECLARE_LOGGING_CATEGORY(domain_server_ice)
Q_DECLARE_LOGGING_CATEGORY(domain_server_auth)

typedef QSharedPointer<Assignment> DomainAssignmentPointer;

using Subnet = QPair<QHostAddress, int>;
using SubnetList = std::vector<Subnet>;

const int INVALID_ICE_LOOKUP_ID = -1;

enum ReplicationServerDirection {
    Upstream,
    Downstream
};

class DomainServer : public QCoreApplication
#ifndef OVERTE_HEADLESS
    , public HTTPSRequestHandler
#endif
{
    Q_OBJECT
public:
    DomainServer(int argc, char* argv[]);
    ~DomainServer();

    static void parseCommandLine(int argc, char* argv[]);

    enum DomainType {
        NonMetaverse,
        MetaverseDomain,
        MetaverseTemporaryDomain
    };

    static int const EXIT_CODE_REBOOT;

#ifndef OVERTE_HEADLESS
    bool handleHTTPRequest(HTTPConnection* connection, const QUrl& url, bool skipSubHandler = false) override;
    bool handleHTTPSRequest(HTTPSConnection* connection, const QUrl& url, bool skipSubHandler = false) override;
#endif

    static const QString REPLACEMENT_FILE_EXTENSION;

    bool isAssetServerEnabled();

    static bool forceCrashReporting() { return _forceCrashReporting; }

public slots:
    /// Called by NodeList to inform us a node has been added
    void nodeAdded(SharedNodePointer node);
    /// Called by NodeList to inform us a node has been killed
    void nodeKilled(SharedNodePointer node);

    void restart();

private slots:
    void processRequestAssignmentPacket(QSharedPointer<ReceivedMessage> packet);
    void processListRequestPacket(QSharedPointer<ReceivedMessage> packet, SharedNodePointer sendingNode);
    void processNodeJSONStatsPacket(QSharedPointer<ReceivedMessage> packetList, SharedNodePointer sendingNode);
    void processPathQueryPacket(QSharedPointer<ReceivedMessage> packet);
    void processNodeDisconnectRequestPacket(QSharedPointer<ReceivedMessage> message);
    void processICEServerHeartbeatDenialPacket(QSharedPointer<ReceivedMessage> message);
    void processICEServerHeartbeatACK(QSharedPointer<ReceivedMessage> message);
    void processAvatarZonePresencePacket(QSharedPointer<ReceivedMessage> packet);

#ifndef OVERTE_HEADLESS
    void handleDomainContentReplacementFromURLRequest(QSharedPointer<ReceivedMessage> message);
    void handleOctreeFileReplacementRequest(QSharedPointer<ReceivedMessage> message);
    bool handleOctreeFileReplacement(QByteArray octreeFile, QString sourceFilename, QString name, QString username);
#endif

    void processOctreeDataRequestMessage(QSharedPointer<ReceivedMessage> message);
    void processOctreeDataPersistMessage(QSharedPointer<ReceivedMessage> message);

    void performIPAddressPortUpdate(const SockAddr& newPublicSockAddr);
    void sendHeartbeatToMetaverse() { sendHeartbeatToMetaverse(QString(), int()); }
    void sendHeartbeatToIceServer();
    void nodePingMonitor();

    void handleConnectedNode(SharedNodePointer newNode, quint64 requestReceiveTime);
#ifndef OVERTE_HEADLESS
    void handleTempDomainSuccess(QNetworkReply* requestReply);
    void handleTempDomainError(QNetworkReply* requestReply);
#endif

    void handleMetaverseHeartbeatError(QNetworkReply* requestReply);

    void queuedQuit(QString quitMessage, int exitCode);

    void handleKeypairChange();

    void updateICEServerAddresses();
    void handleICEHostInfo(const QHostInfo& hostInfo);

    void sendICEServerAddressToMetaverseAPI();
    void handleSuccessfulICEServerAddressUpdate(QNetworkReply* requestReply);
    void handleFailedICEServerAddressUpdate(QNetworkReply* requestReply);

    void updateReplicatedNodes();
    void updateDownstreamNodes();
    void updateUpstreamNodes();

#ifndef OVERTE_HEADLESS
    void initializeExporter();
    void initializeMetadataExporter();
#endif

#ifndef OVERTE_HEADLESS
    void tokenGrantFinished();
    void profileRequestFinished();
#endif

#if defined(WEBRTC_DATA_CHANNELS)
    void forwardAssignmentClientSignalingMessageToUserClient(QSharedPointer<ReceivedMessage> message);
#endif

    void aboutToQuit();

signals:
    void iceServerChanged();
    void userConnected();
    void userDisconnected();

#if defined(WEBRTC_DATA_CHANNELS)
    void webrtcSignalingMessageForDomainServer(const QJsonObject& json);
    void webrtcSignalingMessageForUserClient(const QJsonObject& json);
#endif


private:
    QUuid getID();

    QString getContentBackupDir();
    QString getEntitiesDirPath();
    QString getEntitiesFilePath();
    QString getEntitiesReplacementFilePath();

    void maybeHandleReplacementEntityFile();

    void setupNodeListAndAssignments();
#ifndef OVERTE_HEADLESS
    bool optionallySetupOAuth();
    bool optionallyReadX509KeyAndCertificate();
#endif

#ifndef OVERTE_HEADLESS
    void getTemporaryName(bool force = false);
#endif

    static bool isPacketVerified(const udt::Packet& packet);

    bool resetAccountManagerAccessToken();

    void setupAutomaticNetworking();
    void setupICEHeartbeatForFullNetworking();
    void setupHeartbeatToMetaverse();
    void sendHeartbeatToMetaverse(const QString& networkAddress, const int port);

    void randomizeICEServerAddress(bool shouldTriggerHostLookup);

    unsigned int countConnectedUsers();

    void handleKillNode(SharedNodePointer nodeToKill);
    void broadcastNodeDisconnect(const SharedNodePointer& disconnnectedNode);

    void sendDomainListToNode(const SharedNodePointer& node, quint64 requestPacketReceiveTime, const SockAddr& senderSockAddr, bool newConnection);

    bool isInInterestSet(const SharedNodePointer& nodeA, const SharedNodePointer& nodeB);

    QUuid connectionSecretForNodes(const SharedNodePointer& nodeA, const SharedNodePointer& nodeB);
    void broadcastNewNode(const SharedNodePointer& node);

    void parseAssignmentConfigs(QSet<Assignment::Type>& excludedTypes);
    void addStaticAssignmentToAssignmentHash(Assignment* newAssignment);
    void createStaticAssignmentsForType(Assignment::Type type, const QVariantList& configList);
    void populateDefaultStaticAssignmentsExcludingTypes(const QSet<Assignment::Type>& excludedTypes);
    void populateStaticScriptedAssignmentsFromSettings();

    DomainAssignmentPointer dequeueMatchingAssignment(const QUuid& checkInUUID, NodeType_t nodeType);
    DomainAssignmentPointer deployableAssignmentForRequest(const Assignment& requestAssignment);
    void refreshStaticAssignmentAndAddToQueue(DomainAssignmentPointer& assignment);
    void addStaticAssignmentsToQueue();

#ifndef OVERTE_HEADLESS
    QUrl oauthRedirectURL();
    QUrl oauthAuthorizationURL(const QUuid& stateUUID = QUuid::createUuid());

    std::pair<bool, QString>  isAuthenticatedRequest(HTTPConnection* connection);

    QNetworkReply* profileRequestGivenTokenReply(QNetworkReply* tokenReply);
    Headers setupCookieHeadersFromProfileReply(QNetworkReply* profileReply);
#endif

    QJsonObject jsonForSocket(const SockAddr& socket);
    QJsonObject jsonObjectForNode(const SharedNodePointer& node);

    bool shouldReplicateNode(const Node& node);

    void setupGroupCacheRefresh();

    QString pathForRedirect(QString path = QString()) const;

    void updateReplicationNodes(ReplicationServerDirection direction);

#ifndef OVERTE_HEADLESS
    HTTPSConnection* connectionFromReplyWithState(QNetworkReply* reply);

    bool processPendingContent(HTTPConnection* connection, QString itemName, QString filename, QByteArray dataChunk);

    bool forwardMetaverseAPIRequest(HTTPConnection* connection,
                                    const QUrl& requestUrl,
                                    const QString& metaversePath,
                                    const QString& requestSubobjectKey = "",
                                    std::initializer_list<QString> requiredData = { },
                                    std::initializer_list<QString> optionalData = { },
                                    bool requireAccessToken = true);
#endif

#if defined(WEBRTC_DATA_CHANNELS)
    void setUpWebRTCSignalingServer();
    void routeWebRTCSignalingMessage(const QJsonObject& json);
    void sendWebRTCSignalingMessageToAssignmentClient(const QJsonObject& json);
#endif

#ifndef OVERTE_HEADLESS
    QString operationToString(const QNetworkAccessManager::Operation &op);
#endif

    SubnetList _acSubnetAllowlist;

    std::vector<QString> _replicatedUsernames;

    DomainGatekeeper _gatekeeper;
#ifndef OVERTE_HEADLESS
    DomainServerExporter _exporter;
#endif

#ifndef OVERTE_HEADLESS
    HTTPManager _httpManager;
    HTTPManager* _httpExporterManager { nullptr };
    HTTPManager* _httpMetadataExporterManager { nullptr };

    std::unique_ptr<HTTPSManager> _httpsManager;
#endif

    QHash<QUuid, DomainAssignmentPointer> _allAssignments;
    QQueue<DomainAssignmentPointer> _unfulfilledAssignments;

    bool _isUsingDTLS { false };

    bool _oauthEnable { false };
    QUrl _oauthProviderURL;
    QString _oauthClientID;
    QString _oauthClientSecret;
    QString _hostname;

    std::unordered_map<QUuid, QByteArray> _ephemeralACScripts;

#ifndef OVERTE_HEADLESS
    QSet<QUuid> _webAuthenticationStateSet;
    QHash<QUuid, DomainServerWebSessionData> _cookieSessionHash;
#endif

    QString _automaticNetworkingSetting;

    DomainServerSettingsManager _settingsManager;

    SockAddr _iceServerSocket;
    std::unique_ptr<NLPacket> _iceServerHeartbeatPacket;

    // These will be parented to this, they are not dangling
    DomainMetadata* _metadata { nullptr };
    QTimer* _iceHeartbeatTimer { nullptr };
    QTimer* _metaverseHeartbeatTimer { nullptr };
    QTimer* _metaverseGroupCacheTimer { nullptr };
    QTimer* _nodePingMonitorTimer { nullptr };

    QList<QHostAddress> _iceServerAddresses;
    QSet<QHostAddress> _failedIceServerAddresses;
    int _iceAddressLookupID { INVALID_ICE_LOOKUP_ID };
    int _noReplyICEHeartbeats { 0 };
    int _numHeartbeatDenials { 0 };
    bool _connectedToICEServer { false };

    DomainType _type { DomainType::NonMetaverse };

    friend class DomainGatekeeper;
    friend class DomainMetadata;

    static QString _iceServerAddr;
    static int _iceServerPort;
    static bool _overrideDomainID; // should we override the domain-id from settings?
    static QUuid _overridingDomainID; // what should we override it with?
    static bool _getTempName;
    static QString _userConfigFilename;
    static int _parentPID;
    static bool _forceCrashReporting;
    static int _customLocalPort; // -1 = not specified; the port is CLI-only (--port)


    bool _sendICEServerAddressToMetaverseAPIInProgress { false };
    bool _sendICEServerAddressToMetaverseAPIRedo { false };

#ifndef OVERTE_HEADLESS
    std::unique_ptr<DomainContentBackupManager> _contentManager { nullptr };
#endif

#ifndef OVERTE_HEADLESS
    QHash<QUuid, QPointer<HTTPSConnection>> _pendingOAuthConnections;
#endif

    std::unordered_map<int, QByteArray> _pendingUploadedContents;
    std::unordered_map<int, std::unique_ptr<QTemporaryFile>> _pendingContentFiles;

    QThread _assetClientThread;

#if defined(WEBRTC_DATA_CHANNELS)
    std::unique_ptr<WebRTCSignalingServer> _webrtcSignalingServer { nullptr };
#endif
};


#endif // hifi_DomainServer_h
