//
//  DomainServer.cpp
//  domain-server/src
//
//  Created by Stephen Birarda on 9/26/13.
//  Copyright 2013 High Fidelity, Inc.
//  Copyright 2020 Vircadia contributors.
//  Copyright 2023 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "DomainServer.h"

#include <memory>
#include <random>
#include <iostream>
#include <chrono>

#include <QDir>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QSharedMemory>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QCommandLineParser>
#include <QUuid>

#include <AccountManager.h>
#include <AssetClient.h>
#include <BuildInfo.h>
#include <CrashAnnotations.h>
#include <DependencyManager.h>
#include <HifiConfigVariantMap.h>
#include <LogUtils.h>
#include <NetworkingConstants.h>
#include <MetaverseAPI.h>
#include <udt/PacketHeaders.h>
#include <SettingHandle.h>
#include <SharedUtil.h>
#include <ShutdownEventListener.h>
#include <UUID.h>
#include <LogHandler.h>
#include <PathUtils.h>
#include <NumericalConstants.h>
#include <Trace.h>
#include <StatTracker.h>

#include "DomainServerNodeData.h"
#include "NodeConnectionData.h"

#include <ThreadHelpers.h>
#include <crash-handler/CrashHandler.h>


using namespace std::chrono;

Q_LOGGING_CATEGORY(domain_server, "hifi.domain_server")
Q_LOGGING_CATEGORY(domain_server_ice, "hifi.domain_server.ice")
Q_LOGGING_CATEGORY(domain_server_auth, "overte.domain_server.auth")

const QString ACCESS_TOKEN_KEY_PATH = "metaverse.access_token";
const QString& DOMAIN_SERVER_SETTINGS_KEY = "domain_server";
const QString PUBLIC_SOCKET_ADDRESS_KEY = "network_address";
const QString PUBLIC_SOCKET_PORT_KEY = "network_port";
const int MIN_PORT = 1;
const int MAX_PORT = 65535;

int const DomainServer::EXIT_CODE_REBOOT = 234923;

QString DomainServer::_iceServerAddr { NetworkingConstants::ICE_SERVER_DEFAULT_HOSTNAME };
int DomainServer::_iceServerPort { ICE_SERVER_DEFAULT_PORT };
bool DomainServer::_overrideDomainID { false };
QUuid DomainServer::_overridingDomainID;
QString DomainServer::_userConfigFilename;
int DomainServer::_parentPID { -1 };
bool DomainServer::_forceCrashReporting{false};
int DomainServer::_customLocalPort { -1 };


/// @brief The Domain server can proxy requests to the Directory Server, this function handles those forwarding requests.
/// @param connection The HTTP connection object.
/// @param requestUrl The full URL of the request. e.g. https://google.com/api/v1/test
/// @param metaversePath The path on the Directory Server to route to.
/// @param requestSubobjectKey (Optional) The parent object key that any data will be inserted into for the forwarded request.
/// @param requiredData (Optional) This data is required to be present for the request.
/// @param optionalData (Optional) If provided, this optional data will be forwarded with the request.
/// @param requireAccessToken Require a valid access token to be sent with this request.

DomainServer::DomainServer(int argc, char* argv[]) :
    QCoreApplication(argc, argv),
    _gatekeeper(this)
{
    static const QString CRASH_REPORTER = "crash_reporting.enable_crash_reporter";

    if (_parentPID != -1) {
        watchParentProcess(_parentPID);
    }

    PathUtils::removeTemporaryApplicationDirs();

    DependencyManager::set<tracing::Tracer>();
    DependencyManager::set<StatTracker>();

    LogUtils::init();

    LogHandler::getInstance().moveToThread(thread());
    LogHandler::getInstance().setupRepeatedMessageFlusher();

    qDebug() << "Setting up domain-server";
    qDebug() << "[VERSION] Build sequence:" << qPrintable(applicationVersion());
    qDebug() << "[VERSION] MODIFIED_ORGANIZATION:" << BuildInfo::MODIFIED_ORGANIZATION;
    qDebug() << "[VERSION] VERSION:" << BuildInfo::VERSION;
    qDebug() << "[VERSION] BUILD_TYPE_STRING:" << BuildInfo::BUILD_TYPE_STRING;
    qDebug() << "[VERSION] BUILD_GLOBAL_SERVICES:" << BuildInfo::BUILD_GLOBAL_SERVICES;
    qDebug() << "[VERSION] We will be using this name to find ICE servers:" << _iceServerAddr;

    connect(this, &QCoreApplication::aboutToQuit, this, &DomainServer::aboutToQuit);

    // make sure we have a fresh AccountManager instance
    // (need this since domain-server can restart itself and maintain static variables)
    DependencyManager::set<AccountManager>();

    // load the user config
    QString userConfigFilename;
    if (!_userConfigFilename.isEmpty()) {
        userConfigFilename = _userConfigFilename;
    } else {
        // we weren't passed a user config path
        static const QString USER_CONFIG_FILE_NAME = "config.json";
        userConfigFilename = PathUtils::getAppDataFilePath(USER_CONFIG_FILE_NAME);
    }
    _settingsManager.setupConfigMap(userConfigFilename);

    // setup a shutdown event listener to handle SIGTERM or WM_CLOSE for us

#ifdef _WIN32
    installNativeEventFilter(&ShutdownEventListener::getInstance());
#else
    ShutdownEventListener::getInstance();
#endif

    auto &ch = CrashHandler::getInstance();
    ch.setEnabled(_settingsManager.valueOrDefaultValueForKeyPath(CRASH_REPORTER).toBool());


    // make sure we hear about newly connected nodes from our gatekeeper
    connect(&_gatekeeper, &DomainGatekeeper::connectedNode, this, &DomainServer::handleConnectedNode);

    // if a connected node loses connection privileges, hang up on it
    connect(&_gatekeeper, &DomainGatekeeper::killNode, this, &DomainServer::handleKillNode);

    // if permissions are updated, relay the changes to the Node datastructures
    connect(&_settingsManager, &DomainServerSettingsManager::updateNodePermissions,
            &_gatekeeper, &DomainGatekeeper::updateNodePermissions);
    connect(&_settingsManager, &DomainServerSettingsManager::settingsUpdated,
            this, &DomainServer::updateReplicatedNodes);
    connect(&_settingsManager, &DomainServerSettingsManager::settingsUpdated,
            this, &DomainServer::updateDownstreamNodes);
    connect(&_settingsManager, &DomainServerSettingsManager::settingsUpdated,
            this, &DomainServer::updateUpstreamNodes);
    setupGroupCacheRefresh();



    setupNodeListAndAssignments();

    updateReplicatedNodes();
    updateDownstreamNodes();
    updateUpstreamNodes();


    if (_type != NonMetaverse) {
        // if we have a directory services domain, we'll use an access token for API calls
        resetAccountManagerAccessToken();

        setupAutomaticNetworking();
    }

    if (!getID().isNull() && _type != NonMetaverse) {
        // setup periodic heartbeats to directory services API
        setupHeartbeatToMetaverse();

        // send the first heartbeat immediately
        sendHeartbeatToMetaverse();
    }

    // send signal to DomainMetadata when descriptors changed
    _metadata = new DomainMetadata(this);
    connect(&_settingsManager, &DomainServerSettingsManager::settingsUpdated,
            _metadata, &DomainMetadata::descriptorsChanged);

    // update the metadata when a user (dis)connects
    connect(this, &DomainServer::userConnected, _metadata, &DomainMetadata::usersChanged);
    connect(this, &DomainServer::userDisconnected, _metadata, &DomainMetadata::usersChanged);

    // update the metadata when security changes
    connect(&_settingsManager, &DomainServerSettingsManager::updateNodePermissions, [this] { _metadata->securityChanged(true); });

    qDebug() << "domain-server is running";
    static const QString AC_SUBNET_ALLOWLIST_SETTING_PATH = "security.ac_subnet_allowlist";

    static const Subnet LOCALHOST { QHostAddress("127.0.0.1"), 32 };
    _acSubnetAllowlist = { LOCALHOST };

    auto allowlist = _settingsManager.valueOrDefaultValueForKeyPath(AC_SUBNET_ALLOWLIST_SETTING_PATH).toStringList();
    for (auto& subnet : allowlist) {
        auto netmaskParts = subnet.trimmed().split("/");

        if (netmaskParts.size() > 2) {
            qDebug() << "Ignoring subnet in allowlist, malformed: " << subnet;
            continue;
        }

        // The default netmask is 32 if one has not been specified, which will
        // match only the ip provided.
        int netmask = 32;

        if (netmaskParts.size() == 2) {
            bool ok;
            netmask = netmaskParts[1].toInt(&ok);
            if (!ok) {
                qDebug() << "Ignoring subnet in allowlist, bad netmask: " << subnet;
                continue;
            }
        }

        auto ip = QHostAddress(netmaskParts[0]);

        if (!ip.isNull()) {
            qDebug() << "Adding AC allowlist subnet: " << subnet << " -> " << (ip.toString() + "/" + QString::number(netmask));
            _acSubnetAllowlist.push_back({ ip , netmask });
        } else {
            qDebug() << "Ignoring subnet in allowlist, invalid ip portion: " << subnet;
        }
    }


    static const int NODE_PING_MONITOR_INTERVAL_MSECS = 1 * MSECS_PER_SECOND;
    _nodePingMonitorTimer = new QTimer{ this };
    connect(_nodePingMonitorTimer, &QTimer::timeout, this, &DomainServer::nodePingMonitor);
    _nodePingMonitorTimer->start(NODE_PING_MONITOR_INTERVAL_MSECS);

}

void DomainServer::parseCommandLine(int argc, char* argv[]) {
    QCommandLineParser parser;
    parser.setApplicationDescription("Overte Domain Server");
    const QCommandLineOption versionOption = parser.addVersionOption();
    const QCommandLineOption helpOption = parser.addHelpOption();

    const QCommandLineOption iceServerAddressOption("i", "ice-server address", "IP:PORT or HOSTNAME:PORT");
    parser.addOption(iceServerAddressOption);

    const QCommandLineOption domainIDOption("d", "domain-server uuid", "uuid");
    parser.addOption(domainIDOption);



    const QCommandLineOption userConfigOption("user-config", "Pass user config file pass", "path");
    parser.addOption(userConfigOption);

    const QCommandLineOption localPortOption({ "p", "port" }, "Domain server UDP port (command-line only)", "port");
    parser.addOption(localPortOption);

    const QCommandLineOption parentPIDOption(PARENT_PID_OPTION, "PID of the parent process", "parent-pid");
    parser.addOption(parentPIDOption);

    const QCommandLineOption logOption("logOptions", "Logging options, comma separated: color,nocolor,process_id,thread_id,milliseconds,keep_repeats,journald,nojournald", "options");
    parser.addOption(logOption);

    const QCommandLineOption forceCrashReportingOption("forceCrashReporting", "Force crash reporting to initialize.");
    parser.addOption(forceCrashReportingOption);

    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments << argv[i];
    }
    if (!parser.parse(arguments)) {
        std::cout << parser.errorText().toStdString() << std::endl; // Avoid Qt log spam
        QCoreApplication mockApp(argc, argv); // required for call to showHelp()
        parser.showHelp();
        Q_UNREACHABLE();
    }

    if (parser.isSet(versionOption)) {
        parser.showVersion();
        Q_UNREACHABLE();
    }
    if (parser.isSet(helpOption)) {
        QCoreApplication mockApp(argc, argv); // required for call to showHelp()
        parser.showHelp();
        Q_UNREACHABLE();
    }

    // The UDP port is only configurable via the command line. It is intentionally not read
    // from the settings/config files and not persisted there.
    if (!parser.isSet(localPortOption)) {
        std::cout << "Error: domain-server requires --port <port>" << std::endl;
        QCoreApplication mockApp(argc, argv); // required for call to showHelp()
        parser.showHelp(1);
        Q_UNREACHABLE();
    }

    // We want to configure the logging system as early as possible
    auto &logHandler = LogHandler::getInstance();
    if (parser.isSet(logOption)) {
        if (!logHandler.parseOptions(parser.value(logOption).toUtf8(), logOption.names().first())) {
            QCoreApplication mockApp(argc, const_cast<char**>(argv)); // required for call to showHelp()
            parser.showHelp();
            Q_UNREACHABLE();
        }
    }

    if (parser.isSet(iceServerAddressOption)) {
        // parse the IP and port combination for this target
        QString hostnamePortString = parser.value(iceServerAddressOption);

        _iceServerAddr = hostnamePortString.left(hostnamePortString.indexOf(':'));
        _iceServerPort = (quint16) hostnamePortString.mid(hostnamePortString.indexOf(':') + 1).toUInt();
        if (_iceServerPort == 0) {
            _iceServerPort = ICE_SERVER_DEFAULT_PORT;
        }

        if (_iceServerAddr.isEmpty()) {
            qCWarning(domain_server_ice) << "ALERT: Could not parse an IP address and port combination from" << hostnamePortString;
            ::exit(0);
        }
    }

    if (parser.isSet(domainIDOption)) {
        _overridingDomainID = QUuid(parser.value(domainIDOption));
        _overrideDomainID = true;
        qDebug() << "domain-server ID is" << _overridingDomainID;
    }

    if (parser.isSet(userConfigOption)) {
        _userConfigFilename = parser.value(userConfigOption);
    }

    if (parser.isSet(localPortOption)) {
        bool ok = false;
        int customPort = parser.value(localPortOption).toInt(&ok);
        if (ok && customPort > 0 && customPort <= MAX_PORT) {
            _customLocalPort = customPort;
        } else {
            std::cout << "Invalid value for --port: " << parser.value(localPortOption).toStdString() << std::endl;
            QCoreApplication mockApp(argc, argv); // required for call to showHelp()
            parser.showHelp();
            Q_UNREACHABLE();
        }
    }

    if (parser.isSet(parentPIDOption)) {
        bool ok = false;
        int parentPID = parser.value(parentPIDOption).toInt(&ok);

        if (ok) {
            _parentPID = parentPID;
            qDebug() << "Parent process PID is" << _parentPID;
        }
    }

    if (parser.isSet(forceCrashReportingOption)) {
        _forceCrashReporting = true;
    }
}

DomainServer::~DomainServer() {
    qInfo() << "Domain Server is shutting down.";



    DependencyManager::destroy<AccountManager>();

    // cleanup the AssetClient thread
    DependencyManager::destroy<AssetClient>();
    _assetClientThread.quit();
    _assetClientThread.wait();

    // destroy the LimitedNodeList before the DomainServer QCoreApplication is down
    DependencyManager::destroy<LimitedNodeList>();
}

void DomainServer::aboutToQuit() {
    crash::annotations::setShutdownState(true);
}

void DomainServer::queuedQuit(QString quitMessage, int exitCode) {
    if (!quitMessage.isEmpty()) {
        qWarning() << qPrintable(quitMessage);
    }

    QCoreApplication::exit(exitCode);
}

void DomainServer::restart() {
    qDebug() << "domain-server is restarting.";

    exit(DomainServer::EXIT_CODE_REBOOT);
}

QUuid DomainServer::getID() {
    return DependencyManager::get<LimitedNodeList>()->getSessionUUID();
}


static const QString METAVERSE_DOMAIN_ID_KEY_PATH = "metaverse.id";




const QString DOMAIN_CONFIG_ID_KEY = "id";

const QString METAVERSE_AUTOMATIC_NETWORKING_KEY_PATH = "metaverse.automatic_networking";
const QString FULL_AUTOMATIC_NETWORKING_VALUE = "full";
const QString IP_ONLY_AUTOMATIC_NETWORKING_VALUE = "ip";
const QString DISABLED_AUTOMATIC_NETWORKING_VALUE = "disabled";



bool DomainServer::isPacketVerified(const udt::Packet& packet) {
    PacketType headerType = NLPacket::typeInHeader(packet);
    PacketVersion headerVersion = NLPacket::versionInHeader(packet);

    auto nodeList = DependencyManager::get<LimitedNodeList>();

    // if this is a mismatching connect packet, we can't simply drop it on the floor
    // send back a packet to the interface that tells them we refuse connection for a mismatch
    if ((headerType == PacketType::DomainConnectRequest || headerType == PacketType::DomainConnectRequestPending)
        && headerVersion != versionForPacketType(PacketType::DomainConnectRequest)) {
        DomainGatekeeper::sendProtocolMismatchConnectionDenial(packet.getSenderSockAddr());
    }

    if (!PacketTypeEnum::getNonSourcedPackets().contains(headerType)) {
        // this is a sourced packet - first check if we have a node that matches
        Node::LocalID localSourceID = NLPacket::sourceIDInHeader(packet);
        SharedNodePointer sourceNode = nodeList->nodeWithLocalID(localSourceID);

        if (sourceNode) {
            // unverified DS packets (due to a lack of connection secret between DS + node)
            // must come either from the same public IP address or a local IP address (set by RFC 1918)

            DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(sourceNode->getLinkedData());

            bool exactAddressMatch = nodeData->getSendingSockAddr() == packet.getSenderSockAddr();
            bool bothPrivateAddresses = nodeData->getSendingSockAddr().hasPrivateAddress()
                && packet.getSenderSockAddr().hasPrivateAddress();

            if (nodeData && (exactAddressMatch || bothPrivateAddresses)) {
                // to the best of our ability we've verified that this packet comes from the right place
                // let the NodeList do its checks now (but pass it the sourceNode so it doesn't need to look it up again)
                return nodeList->isPacketVerifiedWithSource(packet, sourceNode.data());
            } else {
                HIFI_FDEBUG("Packet of type" << headerType
                    << "received from unmatched IP for UUID" << uuidStringWithoutCurlyBraces(sourceNode->getUUID()));
                return false;
            }
        } else {
            HIFI_FDEBUG("Packet of type" << headerType
                << "received from unknown node with Local ID" << localSourceID);
            return false;
        }
    }

    // fallback to allow the normal NodeList implementation to verify packets
    return nodeList->isPacketVerified(packet);
}


void DomainServer::setupNodeListAndAssignments() {
    static const QString ENABLE_PACKET_AUTHENTICATION = "metaverse.enable_packet_verification";

    // The listening port comes exclusively from the --port command-line argument.
    // It is never read from the settings/config files.
    int domainServerPort = _customLocalPort;

    if (domainServerPort == -1) {
        qCritical() << "domain-server requires --port; the UDP port is only configurable via the command line";
        QCoreApplication::exit(EXIT_FAILURE);
        return;
    }

    int domainServerDTLSPort = INVALID_PORT;

    if (_isUsingDTLS) {
        domainServerDTLSPort = DEFAULT_DOMAIN_SERVER_DTLS_PORT;

        const QString CUSTOM_DTLS_PORT_OPTION = "dtls-port";

        auto dtlsPortVariant = _settingsManager.valueForKeyPath(CUSTOM_DTLS_PORT_OPTION);
        if (dtlsPortVariant.isValid()) {
            domainServerDTLSPort = (unsigned short) dtlsPortVariant.toUInt();
        }
    }

    QSet<Assignment::Type> parsedTypes;
    parseAssignmentConfigs(parsedTypes);

    populateDefaultStaticAssignmentsExcludingTypes(parsedTypes);

    // check for scripts the user wants to persist from their domain-server config
    populateStaticScriptedAssignmentsFromSettings();

    auto nodeList = DependencyManager::set<LimitedNodeList>(domainServerPort, domainServerDTLSPort);

    // no matter the local port, save it to shared mem so that local assignment clients can ask what it is
    nodeList->putLocalPortIntoSharedMemory(DOMAIN_SERVER_LOCAL_PORT_SMEM_KEY, this,
        nodeList->getSocketLocalPort(SocketType::UDP));

    // set our LimitedNodeList UUID to match the UUID from our config
    // nodes will currently use this to add resources to data-web that relate to our domain
    bool isMetaverseDomain = false;
    if (_overrideDomainID) {
        nodeList->setSessionUUID(_overridingDomainID);
        isMetaverseDomain = true; // assume metaverse domain
    } else {
        QVariant idValueVariant = _settingsManager.valueForKeyPath(METAVERSE_DOMAIN_ID_KEY_PATH);
        if (idValueVariant.isValid()) {
            nodeList->setSessionUUID(idValueVariant.toString());
            isMetaverseDomain = true; // if we have an ID, we'll assume we're a metaverse domain
        } else {
            nodeList->setSessionUUID(QUuid::createUuid()); // Use random UUID
        }
    }

    // Create our own short session ID.
    Node::LocalID serverSessionLocalID = _gatekeeper.findOrCreateLocalID(nodeList->getSessionUUID());
    nodeList->setSessionLocalID(serverSessionLocalID);

    if (isMetaverseDomain) {
        // see if we think we're a temp domain (we have an API key) or a full domain
        const auto& temporaryDomainKey = DependencyManager::get<AccountManager>()->getTemporaryDomainKey(getID());
        if (temporaryDomainKey.isEmpty()) {
            _type = MetaverseDomain;
        } else {
            _type = MetaverseTemporaryDomain;
        }
    }

    bool isAuthEnabled = _settingsManager.valueOrDefaultValueForKeyPath(ENABLE_PACKET_AUTHENTICATION).toBool();
    nodeList->setAuthenticatePackets(isAuthEnabled);

    connect(nodeList.data(), &LimitedNodeList::nodeAdded, this, &DomainServer::nodeAdded);
    connect(nodeList.data(), &LimitedNodeList::nodeKilled, this, &DomainServer::nodeKilled);
    connect(nodeList.data(), &LimitedNodeList::localSockAddrChanged, this,
        [this](const SockAddr& localSockAddr) {
        DependencyManager::get<LimitedNodeList>()->putLocalPortIntoSharedMemory(DOMAIN_SERVER_LOCAL_PORT_SMEM_KEY, this, localSockAddr.getPort());
    });

    // register as the packet receiver for the types we want
    PacketReceiver& packetReceiver = nodeList->getPacketReceiver();
    packetReceiver.registerListener(PacketType::RequestAssignment,
        PacketReceiver::makeUnsourcedListenerReference<DomainServer>(this, &DomainServer::processRequestAssignmentPacket));
    packetReceiver.registerListener(PacketType::DomainListRequest,
        PacketReceiver::makeSourcedListenerReference<DomainServer>(this, &DomainServer::processListRequestPacket));
    packetReceiver.registerListener(PacketType::DomainServerPathQuery,
        PacketReceiver::makeUnsourcedListenerReference<DomainServer>(this, &DomainServer::processPathQueryPacket));
    packetReceiver.registerListener(PacketType::NodeJsonStats,
        PacketReceiver::makeSourcedListenerReference<DomainServer>(this, &DomainServer::processNodeJSONStatsPacket));
    packetReceiver.registerListener(PacketType::DomainDisconnectRequest,
        PacketReceiver::makeUnsourcedListenerReference<DomainServer>(this, &DomainServer::processNodeDisconnectRequestPacket));
    packetReceiver.registerListener(PacketType::AvatarZonePresence,
        PacketReceiver::makeUnsourcedListenerReference<DomainServer>(this, &DomainServer::processAvatarZonePresencePacket));

    // NodeList won't be available to the settings manager when it is created, so call registerListener here
    packetReceiver.registerListener(PacketType::DomainSettingsRequest,
        PacketReceiver::makeUnsourcedListenerReference<DomainServerSettingsManager>(&_settingsManager, &DomainServerSettingsManager::processSettingsRequestPacket));
    packetReceiver.registerListener(PacketType::NodeKickRequest,
        PacketReceiver::makeSourcedListenerReference<DomainServerSettingsManager>(&_settingsManager, &DomainServerSettingsManager::processNodeKickRequestPacket));
    packetReceiver.registerListener(PacketType::UsernameFromIDRequest,
        PacketReceiver::makeSourcedListenerReference<DomainServerSettingsManager>(&_settingsManager, &DomainServerSettingsManager::processUsernameFromIDRequestPacket));

    // register the gatekeeper for the packets it needs to receive
    packetReceiver.registerListener(PacketType::DomainConnectRequest,
        PacketReceiver::makeUnsourcedListenerReference<DomainGatekeeper>(&_gatekeeper, &DomainGatekeeper::processConnectRequestPacket));
    packetReceiver.registerListener(PacketType::DomainConnectRequestPending,
        PacketReceiver::makeUnsourcedListenerReference<DomainGatekeeper>(&_gatekeeper, &DomainGatekeeper::processConnectRequestPacket));
    packetReceiver.registerListener(PacketType::ICEPing,
        PacketReceiver::makeUnsourcedListenerReference<DomainGatekeeper>(&_gatekeeper, &DomainGatekeeper::processICEPingPacket));
    packetReceiver.registerListener(PacketType::ICEPingReply,
        PacketReceiver::makeUnsourcedListenerReference<DomainGatekeeper>(&_gatekeeper, &DomainGatekeeper::processICEPingReplyPacket));
    packetReceiver.registerListener(PacketType::ICEServerPeerInformation,
        PacketReceiver::makeUnsourcedListenerReference<DomainGatekeeper>(&_gatekeeper, &DomainGatekeeper::processICEPeerInformationPacket));

    packetReceiver.registerListener(PacketType::ICEServerHeartbeatDenied,
        PacketReceiver::makeUnsourcedListenerReference<DomainServer>(this, &DomainServer::processICEServerHeartbeatDenialPacket));
    packetReceiver.registerListener(PacketType::ICEServerHeartbeatACK,
        PacketReceiver::makeUnsourcedListenerReference<DomainServer>(this, &DomainServer::processICEServerHeartbeatACK));

    packetReceiver.registerListener(PacketType::OctreeDataFileRequest,
        PacketReceiver::makeUnsourcedListenerReference<DomainServer>(this, &DomainServer::processOctreeDataRequestMessage));
    packetReceiver.registerListener(PacketType::OctreeDataPersist,
        PacketReceiver::makeUnsourcedListenerReference<DomainServer>(this, &DomainServer::processOctreeDataPersistMessage));


    // set a custom packetVersionMatch as the verify packet operator for the udt::Socket
    nodeList->setPacketFilterOperator(&DomainServer::isPacketVerified);

    QString name = "AssetClient Thread";
    _assetClientThread.setObjectName(name);
    auto assetClient = DependencyManager::set<AssetClient>();
    assetClient->moveToThread(&_assetClientThread);
    connect(&_assetClientThread, &QThread::started, [name] { setThreadName(name.toStdString()); });
    _assetClientThread.start();
    // add whatever static assignments that have been parsed to the queue
    addStaticAssignmentsToQueue();
}




bool DomainServer::resetAccountManagerAccessToken() {
    // OAuth and the metaverse access-token flow have been removed from the headless build.
    return false;
}

void DomainServer::setupAutomaticNetworking() {

    _automaticNetworkingSetting =
        _settingsManager.valueOrDefaultValueForKeyPath(METAVERSE_AUTOMATIC_NETWORKING_KEY_PATH).toString();

    qDebug() << "Configuring automatic networking in domain-server as" << _automaticNetworkingSetting;

    if (_automaticNetworkingSetting != DISABLED_AUTOMATIC_NETWORKING_VALUE) {
        const QUuid& domainID = getID();

        if (_automaticNetworkingSetting == FULL_AUTOMATIC_NETWORKING_VALUE) {
            setupICEHeartbeatForFullNetworking();
        }

        if (_automaticNetworkingSetting == IP_ONLY_AUTOMATIC_NETWORKING_VALUE ||
            _automaticNetworkingSetting == FULL_AUTOMATIC_NETWORKING_VALUE) {

            if (!domainID.isNull()) {
                qDebug() << "domain-server" << _automaticNetworkingSetting << "automatic networking enabled for ID"
                    << uuidStringWithoutCurlyBraces(domainID) << "via" << _oauthProviderURL.toString();

                auto nodeList = DependencyManager::get<LimitedNodeList>();

                // send any public socket changes to the data server so nodes can find us at our new IP
                connect(nodeList.data(), &LimitedNodeList::publicSockAddrChanged, this,
                        &DomainServer::performIPAddressPortUpdate);

                if (_automaticNetworkingSetting == IP_ONLY_AUTOMATIC_NETWORKING_VALUE) {
                    // have the LNL enable public socket updating via STUN
                    nodeList->startSTUNPublicSocketUpdate();
                }
            } else {
                qCCritical(domain_server) << "PAGE: Cannot enable domain-server automatic networking without a domain ID."
                << "Please add an ID to your config file or via the web interface.";
                return;
            }
        }
    }

}

void DomainServer::setupHeartbeatToMetaverse() {
    // heartbeat to the data-server every 15s
    const int DOMAIN_SERVER_DATA_WEB_HEARTBEAT_MSECS = 15 * 1000;

    if (!_metaverseHeartbeatTimer) {
        // setup a timer to heartbeat with the metaverse-server
        _metaverseHeartbeatTimer = new QTimer { this };
        connect(_metaverseHeartbeatTimer, SIGNAL(timeout()), this, SLOT(sendHeartbeatToMetaverse()));
        // do not send a heartbeat immediately - this avoids flooding if the heartbeat fails with a 401
        _metaverseHeartbeatTimer->start(DOMAIN_SERVER_DATA_WEB_HEARTBEAT_MSECS);
    }
}

void DomainServer::setupICEHeartbeatForFullNetworking() {
    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();

    // lookup the available ice-server hosts now
    updateICEServerAddresses();

    // call our sendHeartbeatToIceServer immediately anytime a local or public socket changes
    connect(limitedNodeList.data(), &LimitedNodeList::localSockAddrChanged,
            this, &DomainServer::sendHeartbeatToIceServer);
    connect(limitedNodeList.data(), &LimitedNodeList::publicSockAddrChanged,
            this, &DomainServer::sendHeartbeatToIceServer);

    // we need this DS to know what our public IP is - start trying to figure that out now
    limitedNodeList->startSTUNPublicSocketUpdate();

    // to send ICE heartbeats we'd better have a private key locally with an uploaded public key
    // if we have an access token and we don't have a private key or the current domain ID has changed
    // we should generate a new keypair
    auto accountManager = DependencyManager::get<AccountManager>();
    if (!accountManager->getAccountInfo().hasPrivateKey() || accountManager->getAccountInfo().getDomainID() != getID()) {
        accountManager->generateNewDomainKeypair(getID());
    }

    // hookup to the signal from account manager that tells us when keypair is available
    connect(accountManager.data(), &AccountManager::newKeypair, this, &DomainServer::handleKeypairChange);

    if (!_iceHeartbeatTimer) {
        // setup a timer to heartbeat with the ice-server
        _iceHeartbeatTimer = new QTimer { this };
        connect(_iceHeartbeatTimer, &QTimer::timeout, this, &DomainServer::sendHeartbeatToIceServer);
        sendHeartbeatToIceServer();
        _iceHeartbeatTimer->start(ICE_HEARBEAT_INTERVAL_MSECS);
    }
}

void DomainServer::updateICEServerAddresses() {
    if (_iceAddressLookupID == INVALID_ICE_LOOKUP_ID) {
        _iceAddressLookupID = QHostInfo::lookupHost(_iceServerAddr, this, SLOT(handleICEHostInfo(QHostInfo)));
    }
}

void DomainServer::parseAssignmentConfigs(QSet<Assignment::Type>& excludedTypes) {
    const QString ASSIGNMENT_CONFIG_PREFIX = "config-";

    // scan for assignment config keys
    for (int i = 0; i < Assignment::AllTypes; ++i) {
        QVariant assignmentConfigVariant = _settingsManager.valueOrDefaultValueForKeyPath(ASSIGNMENT_CONFIG_PREFIX + QString::number(i));

        if (assignmentConfigVariant.isValid()) {
            // figure out which assignment type this matches
            Assignment::Type assignmentType = static_cast<Assignment::Type>(i);

            if (!excludedTypes.contains(assignmentType)) {
                QVariantList assignmentList = assignmentConfigVariant.toList();

                if (assignmentType != Assignment::AgentType) {
                    createStaticAssignmentsForType(assignmentType, assignmentList);
                }

                excludedTypes.insert(assignmentType);
            }
        }
    }
}

void DomainServer::addStaticAssignmentToAssignmentHash(Assignment* newAssignment) {
    qDebug() << "Inserting assignment" << *newAssignment << "to static assignment hash.";
    newAssignment->setIsStatic(true);
    _allAssignments.insert(newAssignment->getUUID(), DomainAssignmentPointer(newAssignment));
}

void DomainServer::populateStaticScriptedAssignmentsFromSettings() {
    const QString PERSISTENT_SCRIPTS_KEY_PATH = "scripts.persistent_scripts";
    QVariant persistentScriptsVariant = _settingsManager.valueOrDefaultValueForKeyPath(PERSISTENT_SCRIPTS_KEY_PATH);

    if (persistentScriptsVariant.isValid()) {
        QVariantList persistentScriptsList = persistentScriptsVariant.toList();
        foreach(const QVariant& persistentScriptVariant, persistentScriptsList) {
            QVariantMap persistentScript = persistentScriptVariant.toMap();

            const QString PERSISTENT_SCRIPT_URL_KEY = "url";
            const QString PERSISTENT_SCRIPT_NUM_INSTANCES_KEY = "num_instances";
            const QString PERSISTENT_SCRIPT_POOL_KEY = "pool";

            if (persistentScript.contains(PERSISTENT_SCRIPT_URL_KEY)) {
                // check how many instances of this script to add

                int numInstances = persistentScript[PERSISTENT_SCRIPT_NUM_INSTANCES_KEY].toInt();
                QString scriptURL = persistentScript[PERSISTENT_SCRIPT_URL_KEY].toString();

                QString scriptPool = persistentScript.value(PERSISTENT_SCRIPT_POOL_KEY).toString();

                qDebug() << "Adding" << numInstances << "of persistent script at URL" << scriptURL << "- pool" << scriptPool;

                for (int i = 0; i < numInstances; ++i) {
                    // add a scripted assignment to the queue for this instance
                    Assignment* scriptAssignment = new Assignment(Assignment::CreateCommand,
                                                                  Assignment::AgentType,
                                                                  scriptPool);
                    scriptAssignment->setPayload(scriptURL.toUtf8());

                    // add it to static hash so we know we have to keep giving it back out
                    addStaticAssignmentToAssignmentHash(scriptAssignment);
                }
            }
        }
    }
}

void DomainServer::createStaticAssignmentsForType(Assignment::Type type, const QVariantList &configList) {
    // we have a string for config for this type
    qDebug() << "Parsing config for assignment type" << type;

    int configCounter = 0;

    foreach(const QVariant& configVariant, configList) {
        if (configVariant.canConvert(QMetaType::QVariantMap)) {
            QVariantMap configMap = configVariant.toMap();

            // check the config string for a pool
            const QString ASSIGNMENT_POOL_KEY = "pool";

            QString assignmentPool = configMap.value(ASSIGNMENT_POOL_KEY).toString();
            if (!assignmentPool.isEmpty()) {
                configMap.remove(ASSIGNMENT_POOL_KEY);
            }

            ++configCounter;
            qDebug() << "Type" << type << "config" << configCounter << "=" << configMap;

            Assignment* configAssignment = new Assignment(Assignment::CreateCommand, type, assignmentPool);

            // setup the payload as a semi-colon separated list of key = value
            QStringList payloadStringList;
            foreach(const QString& payloadKey, configMap.keys()) {
                QString dashes = payloadKey.size() == 1 ? "-" : "--";
                payloadStringList << QString("%1%2 %3").arg(dashes).arg(payloadKey).arg(configMap[payloadKey].toString());
            }

            configAssignment->setPayload(payloadStringList.join(" ").toUtf8());

            addStaticAssignmentToAssignmentHash(configAssignment);
        }
    }
}

void DomainServer::populateDefaultStaticAssignmentsExcludingTypes(const QSet<Assignment::Type>& excludedTypes) {
    // enumerate over all assignment types and see if we've already excluded it
    for (Assignment::Type defaultedType = Assignment::FirstType;
         defaultedType != Assignment::AllTypes;
         defaultedType =  static_cast<Assignment::Type>(static_cast<int>(defaultedType) + 1)) {
        if (!excludedTypes.contains(defaultedType) && defaultedType != Assignment::AgentType) {

            // Make sure the asset-server is enabled before adding it here.
            // Initially we do not assign it by default so we can test it in HF domains first
            if (defaultedType == Assignment::AssetServerType && !isAssetServerEnabled()) {
                // skip to the next iteraion if asset-server isn't enabled
                continue;
            }

            // type has not been set from a command line or config file config, use the default
            // by clearing whatever exists and writing a single default assignment with no payload
            Assignment* newAssignment = new Assignment(Assignment::CreateCommand, (Assignment::Type) defaultedType);
            addStaticAssignmentToAssignmentHash(newAssignment);
        }
    }
}

void DomainServer::processListRequestPacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer sendingNode) {
    QDataStream packetStream(message->getMessage());
    NodeConnectionData nodeRequestData = NodeConnectionData::fromDataStream(packetStream, message->getSenderSockAddr(), false);

    // update this node's sockets in case they have changed
    sendingNode->setPublicSocket(nodeRequestData.publicSockAddr);
    sendingNode->setLocalSocket(nodeRequestData.localSockAddr);

    DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(sendingNode->getLinkedData());

    if (!nodeData->hasCheckedIn()) {
        nodeData->setHasCheckedIn(true);

        // on first check in, make sure we've cleaned up any ICE peer for this node
        _gatekeeper.cleanupICEPeerForNode(sendingNode->getUUID());
    }

    // guard against patched agents asking to hear about other agents
    auto safeInterestSet = QSet<NodeType_t>(nodeRequestData.interestList.begin(), nodeRequestData.interestList.end());
    if (sendingNode->getType() == NodeType::Agent) {
        safeInterestSet.remove(NodeType::Agent);
    }

    // update the NodeInterestSet in case there have been any changes
    nodeData->setNodeInterestSet(safeInterestSet);

    // update the connecting hostname in case it has changed
    nodeData->setPlaceName(nodeRequestData.placeName);

    // client-side send time of last connect/domain list request
    nodeData->setLastDomainCheckinTimestamp(nodeRequestData.lastPingTimestamp);

    sendDomainListToNode(sendingNode, message->getFirstPacketReceiveTime(), message->getSenderSockAddr(), false);
}

bool DomainServer::isInInterestSet(const SharedNodePointer& nodeA, const SharedNodePointer& nodeB) {
    auto nodeAData = static_cast<DomainServerNodeData*>(nodeA->getLinkedData());
    return nodeAData && nodeAData->getNodeInterestSet().contains(nodeB->getType());
}

unsigned int DomainServer::countConnectedUsers() {
    unsigned int result = 0;
    auto nodeList = DependencyManager::get<LimitedNodeList>();
    nodeList->eachNode([&result](const SharedNodePointer& node){
        // only count unassigned agents (i.e., users)
        if (node->getType() == NodeType::Agent) {
            auto nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());
            if (nodeData && !nodeData->wasAssigned()) {
                result++;
            }
        }
    });
    return result;
}


const QString OAUTH_CLIENT_ID_QUERY_KEY = "client_id";
const QString OAUTH_REDIRECT_URI_QUERY_KEY = "redirect_uri";


void DomainServer::handleConnectedNode(SharedNodePointer newNode, quint64 requestReceiveTime) {
    DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(newNode->getLinkedData());

    // reply back to the user with a PacketType::DomainList
    sendDomainListToNode(newNode, requestReceiveTime, nodeData->getSendingSockAddr(), true);

    // if this node is a user (unassigned Agent), signal
    if (newNode->getType() == NodeType::Agent && !nodeData->wasAssigned()) {
        emit userConnected();
    }

    if (shouldReplicateNode(*newNode)) {
        qDebug() << "Setting node to replicated: " << newNode->getUUID();
        newNode->setIsReplicated(true);
    }

    // send out this node to our other connected nodes
    broadcastNewNode(newNode);
}

void DomainServer::sendDomainListToNode(const SharedNodePointer& node, quint64 requestPacketReceiveTime, const SockAddr &senderSockAddr, bool newConnection) {
    const int NUM_DOMAIN_LIST_EXTENDED_HEADER_BYTES = NUM_BYTES_RFC4122_UUID + NLPacket::NUM_BYTES_LOCALID +
        NUM_BYTES_RFC4122_UUID + NLPacket::NUM_BYTES_LOCALID + 4;

    // setup the extended header for the domain list packets
    // this data is at the beginning of each of the domain list packets
    QByteArray extendedHeader(NUM_DOMAIN_LIST_EXTENDED_HEADER_BYTES, 0);
    QDataStream extendedHeaderStream(&extendedHeader, QIODevice::WriteOnly);
    DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());
    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();

    extendedHeaderStream << limitedNodeList->getSessionUUID();
    extendedHeaderStream << limitedNodeList->getSessionLocalID();
    extendedHeaderStream << node->getUUID();
    extendedHeaderStream << node->getLocalID();
    extendedHeaderStream << node->getPermissions();
    extendedHeaderStream << limitedNodeList->getAuthenticatePackets();
    extendedHeaderStream << nodeData->getLastDomainCheckinTimestamp();
    extendedHeaderStream << quint64(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
    extendedHeaderStream << quint64(duration_cast<microseconds>(p_high_resolution_clock::now().time_since_epoch()).count()) - requestPacketReceiveTime;
    extendedHeaderStream << newConnection;
    auto domainListPackets = NLPacketList::create(PacketType::DomainList, extendedHeader);

    // always send the node their own UUID back
    QDataStream domainListStream(domainListPackets.get());

    // store the nodeInterestSet on this DomainServerNodeData, in case it has changed
    auto& nodeInterestSet = nodeData->getNodeInterestSet();

    if (nodeInterestSet.size() > 0) {

        // DTLSServerSession* dtlsSession = _isUsingDTLS ? _dtlsSessions[senderSockAddr] : NULL;
        if (nodeData->isAuthenticated()) {
            // if this authenticated node has any interest types, send back those nodes as well
            limitedNodeList->eachNode([this, node, &domainListPackets, &domainListStream](const SharedNodePointer& otherNode) {
                if (otherNode->getUUID() != node->getUUID() && isInInterestSet(node, otherNode)) {
                    // since we're about to add a node to the packet we start a segment
                    domainListPackets->startSegment();

                    // don't send avatar nodes to other avatars, that will come from avatar mixer
                    domainListStream << *otherNode.data();

                    // pack the secret that these two nodes will use to communicate with each other
                    domainListStream << connectionSecretForNodes(node, otherNode);

                    // we've added the node we wanted so end the segment now
                    domainListPackets->endSegment();
                }
            });
        }
    }

    // send an empty list to the node, in case there were no other nodes
    domainListPackets->closeCurrentPacket(true);

    // write the PacketList to this node
    limitedNodeList->sendPacketList(std::move(domainListPackets), *node);
}

QUuid DomainServer::connectionSecretForNodes(const SharedNodePointer& nodeA, const SharedNodePointer& nodeB) {
    DomainServerNodeData* nodeAData = static_cast<DomainServerNodeData*>(nodeA->getLinkedData());
    DomainServerNodeData* nodeBData = static_cast<DomainServerNodeData*>(nodeB->getLinkedData());

    if (nodeAData && nodeBData) {
        QUuid& secretUUID = nodeAData->getSessionSecretHash()[nodeB->getUUID()];

        if (secretUUID.isNull()) {
            // generate a new secret UUID these two nodes can use
            secretUUID = QUuid::createUuid();

            // set it on the other Node's sessionSecretHash
            static_cast<DomainServerNodeData*>(nodeBData)->getSessionSecretHash().insert(nodeA->getUUID(), secretUUID);
        }

        return secretUUID;
    }

    return QUuid();
}

void DomainServer::broadcastNewNode(const SharedNodePointer& addedNode) {

    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();
    QWeakPointer<LimitedNodeList> limitedNodeListWeak = limitedNodeList;

    auto addNodePacket = NLPacket::create(PacketType::DomainServerAddedNode);

    // setup the add packet for this new node
    QDataStream addNodeStream(addNodePacket.get());

    addNodeStream << *addedNode.data();

    int connectionSecretIndex = addNodePacket->pos();

    limitedNodeList->eachMatchingNode(
        [this, addedNode](const SharedNodePointer& node)->bool {
            // is the added Node in this node's interest list?
            return node->getLinkedData()
                && node->getActiveSocket()
                && node != addedNode
                && isInInterestSet(node, addedNode);
        },
        [this, &addNodePacket, connectionSecretIndex, addedNode, limitedNodeListWeak](const SharedNodePointer& node) {
            // send off this packet to the node
            auto limitedNodeList = limitedNodeListWeak.lock();
            if (limitedNodeList) {
                addNodePacket->seek(connectionSecretIndex);

                QByteArray rfcConnectionSecret = connectionSecretForNodes(node, addedNode).toRfc4122();

                // replace the bytes at the end of the packet for the connection secret between these nodes
                addNodePacket->write(rfcConnectionSecret);

                limitedNodeList->sendUnreliablePacket(*addNodePacket, *node);
            }
        }
    );
}

void DomainServer::processRequestAssignmentPacket(QSharedPointer<ReceivedMessage> message) {
    // construct the requested assignment from the packet data
    Assignment requestAssignment(*message);

    auto senderAddr = message->getSenderSockAddr().getAddress();

    auto isHostAddressInSubnet = [&senderAddr](const Subnet& mask) -> bool {
        return senderAddr.isInSubnet(mask);
    };

    auto it = find_if(_acSubnetAllowlist.begin(), _acSubnetAllowlist.end(), isHostAddressInSubnet);
    if (it == _acSubnetAllowlist.end()) {
        HIFI_FDEBUG("Received an assignment connect request from a disallowed ip address:"
            << senderAddr.toString());
        return;
    }

    static bool printedAssignmentTypeMessage = false;
    if (!printedAssignmentTypeMessage && requestAssignment.getType() != Assignment::AgentType) {
        printedAssignmentTypeMessage = true;
        qDebug() << "Received a request for assignment type" << requestAssignment.getType()
                 << "from" << message->getSenderSockAddr();
    }

    DomainAssignmentPointer assignmentToDeploy = deployableAssignmentForRequest(requestAssignment);

    if (assignmentToDeploy) {
        qDebug() << "Deploying assignment -" << *assignmentToDeploy.data() << "- to" << message->getSenderSockAddr();

        // give this assignment out, either the type matches or the requestor said they will take any
        static std::unique_ptr<NLPacket> assignmentPacket;

        if (!assignmentPacket) {
            assignmentPacket = NLPacket::create(PacketType::CreateAssignment);
        }

        // setup a copy of this assignment that will have a unique UUID, for packaging purposes
        Assignment uniqueAssignment(*assignmentToDeploy.data());
        uniqueAssignment.setUUID(QUuid::createUuid());

        // reset the assignmentPacket
        assignmentPacket->reset();

        QDataStream assignmentStream(assignmentPacket.get());

        assignmentStream << uniqueAssignment;

        auto limitedNodeList = DependencyManager::get<LimitedNodeList>();
        limitedNodeList->sendUnreliablePacket(*assignmentPacket, message->getSenderSockAddr());

        // give the information for that deployed assignment to the gatekeeper so it knows to that that node
        // in when it comes back around
        _gatekeeper.addPendingAssignedNode(uniqueAssignment.getUUID(), assignmentToDeploy->getUUID(), requestAssignment.getNodeVersion());
    } else {
        static bool printedAssignmentRequestMessage = false;
        if (!printedAssignmentRequestMessage && requestAssignment.getType() != Assignment::AgentType) {
            printedAssignmentRequestMessage = true;
            qDebug() << "Unable to fulfill assignment request of type" << requestAssignment.getType()
                << "from" << message->getSenderSockAddr();
        }
    }
}

QJsonObject jsonForDomainSocketUpdate(const SockAddr& socket) {
    const QString SOCKET_NETWORK_ADDRESS_KEY = "network_address";
    const QString SOCKET_PORT_KEY = "port";

    QJsonObject socketObject;
    socketObject[SOCKET_NETWORK_ADDRESS_KEY] = socket.getAddress().toString();
    socketObject[SOCKET_PORT_KEY] = socket.getPort();

    return socketObject;
}

void DomainServer::performIPAddressPortUpdate(const SockAddr& newPublicSockAddr) {
    const QString& publicSocketAddress = newPublicSockAddr.getAddress().toString();
    const int publicSocketPort = newPublicSockAddr.getPort();

    if (_automaticNetworkingSetting == IP_ONLY_AUTOMATIC_NETWORKING_VALUE) {
        sendHeartbeatToMetaverse(publicSocketAddress, 0);
    } else {
        // Full automatic networking, update both port and IP address
        sendHeartbeatToMetaverse(publicSocketAddress, publicSocketPort);
    }

    QJsonObject rootObject;
    QJsonObject domainServerObject;
    domainServerObject.insert(PUBLIC_SOCKET_ADDRESS_KEY, publicSocketAddress);
    if (_automaticNetworkingSetting == FULL_AUTOMATIC_NETWORKING_VALUE) {
        domainServerObject.insert(PUBLIC_SOCKET_PORT_KEY, publicSocketPort);
    }
    rootObject.insert(DOMAIN_SERVER_SETTINGS_KEY, domainServerObject);
    QJsonDocument doc(rootObject);
    qDebug() << "DomainServer::performIPAddressPortUpdate: " << doc;
    _settingsManager.recurseJSONObjectAndOverwriteSettings(rootObject, DomainSettings);
}

void DomainServer::sendHeartbeatToMetaverse(const QString& networkAddress, const int port) {
    // Setup the domain object to send to the data server
    QJsonObject domainObject;

    // add the versions
    static const QString VERSION_KEY = "version";
    domainObject[VERSION_KEY] = BuildInfo::VERSION;
    static const QString PROTOCOL_VERSION_KEY = "protocol";
    domainObject[PROTOCOL_VERSION_KEY] = protocolVersionsSignatureBase64();

    static const QString NETWORK_ADDRESS_SETTINGS_KEY = "domain_server." + PUBLIC_SOCKET_ADDRESS_KEY;
    const QString networkAddressFromSettings = _settingsManager.valueForKeyPath(NETWORK_ADDRESS_SETTINGS_KEY).toString();
    if (!networkAddress.isEmpty()) {
        domainObject[PUBLIC_SOCKET_ADDRESS_KEY] = networkAddress;
    } else if (!networkAddressFromSettings.isEmpty()) {
        domainObject[PUBLIC_SOCKET_ADDRESS_KEY] = networkAddressFromSettings;
    }

    static const QString PORT_SETTINGS_KEY = "domain_server." + PUBLIC_SOCKET_PORT_KEY;
    const int portFromSettings = _settingsManager.valueForKeyPath(PORT_SETTINGS_KEY).toInt();
    if (port != 0) {
        domainObject[PUBLIC_SOCKET_PORT_KEY] = port;
    } else if (portFromSettings != 0) {
        domainObject[PUBLIC_SOCKET_PORT_KEY] = portFromSettings;
    }

    static const QString AUTOMATIC_NETWORKING_KEY = "automatic_networking";
    domainObject[AUTOMATIC_NETWORKING_KEY] = _automaticNetworkingSetting;

    // add access level for anonymous connections
    // consider the domain to be "restricted" if anonymous connections are disallowed
    static const QString RESTRICTED_ACCESS_FLAG = "restricted";
    NodePermissions anonymousPermissions = _settingsManager.getPermissionsForName(NodePermissions::standardNameAnonymous);
    domainObject[RESTRICTED_ACCESS_FLAG] = !anonymousPermissions.can(NodePermissions::Permission::canConnectToDomain);

    const auto& temporaryDomainKey = DependencyManager::get<AccountManager>()->getTemporaryDomainKey(getID());
    if (!temporaryDomainKey.isEmpty()) {
        // add the temporary domain token
        const QString KEY_KEY = "api_key";
        domainObject[KEY_KEY] = temporaryDomainKey;
    }

    if (_metadata) {
        // Add the metadata to the heartbeat
        static const QString DOMAIN_HEARTBEAT_KEY = "heartbeat";
        domainObject[DOMAIN_HEARTBEAT_KEY] = _metadata->get(DomainMetadata::USERS);
    }

    QString domainUpdateJSON = QString("{\"domain\":%1}").arg(QString(QJsonDocument(domainObject).toJson(QJsonDocument::Compact)));

    static const QString DOMAIN_UPDATE = "/api/v1/domains/%1";
    DependencyManager::get<AccountManager>()->sendRequest(DOMAIN_UPDATE.arg(uuidStringWithoutCurlyBraces(getID())),
                                              AccountManagerAuth::Optional,
                                              QNetworkAccessManager::PutOperation,
                                              JSONCallbackParameters(this, QString(), "handleMetaverseHeartbeatError"),
                                              domainUpdateJSON.toUtf8());
}

void DomainServer::handleMetaverseHeartbeatError(QNetworkReply* requestReply) {
    if (!_metaverseHeartbeatTimer) {
        // avoid rehandling errors from the same issue
        return;
    }

    // only attempt to grab a new temporary name if we're already a temporary domain server
    if (_type == MetaverseTemporaryDomain) {
        // check if we need to force a new temporary domain name
        switch (requestReply->error()) {
                // if we have a temporary domain with a bad token, we get a 401
            case QNetworkReply::NetworkError::AuthenticationRequiredError: {
                static const QString DATA_KEY = "data";
                static const QString TOKEN_KEY = "api_key";

                QJsonObject jsonObject = QJsonDocument::fromJson(requestReply->readAll()).object();
                auto tokenFailure = jsonObject[DATA_KEY].toObject()[TOKEN_KEY];

                if (!tokenFailure.isNull()) {
                    qWarning() << "Temporary domain name lacks a valid API key, and is being reset.";
                }
                break;
            }
                // if the domain does not (or no longer) exists, we get a 404
            case QNetworkReply::NetworkError::ContentNotFoundError:
                qWarning() << "Domain not found, getting a new temporary domain.";
                break;
                // otherwise, we erred on something else, and should not force a temporary domain
            default:
                return;
        }

        // halt heartbeats until we have a token
        _metaverseHeartbeatTimer->deleteLater();
        _metaverseHeartbeatTimer = nullptr;

        // give up eventually to avoid flooding traffic
        static const int MAX_ATTEMPTS = 5;
        static int attempt = 0;
        if (++attempt < MAX_ATTEMPTS) {
            // get a new temporary name and token
        } else {
            qWarning() << "Already attempted too many temporary domain requests. Please set a domain ID manually or restart.";
        }
    }
}

void DomainServer::sendICEServerAddressToMetaverseAPI() {
    if (_sendICEServerAddressToMetaverseAPIInProgress) {
        // don't have more than one of these in-flight at a time.  set a flag to indicate that once the current one
        // is done, we need to do update metaverse again.
        _sendICEServerAddressToMetaverseAPIRedo = true;
        return;
    }
    _sendICEServerAddressToMetaverseAPIInProgress = true;
    const QString ICE_SERVER_ADDRESS = "ice_server_address";

    QJsonObject domainObject;

    if (!_connectedToICEServer || _iceServerSocket.isNull()) {
        domainObject[ICE_SERVER_ADDRESS] = "0.0.0.0";
    } else {
        // we're using full automatic networking and we have a current ice-server socket, use that now
        domainObject[ICE_SERVER_ADDRESS] = _iceServerSocket.getAddress().toString();
    }

    const auto& temporaryDomainKey = DependencyManager::get<AccountManager>()->getTemporaryDomainKey(getID());
    if (!temporaryDomainKey.isEmpty()) {
        // add the temporary domain token
        const QString KEY_KEY = "api_key";
        domainObject[KEY_KEY] = temporaryDomainKey;
    }

    QString domainUpdateJSON = QString("{\"domain\": %1 }").arg(QString(QJsonDocument(domainObject).toJson()));

    // make sure we hear about failure so we can retry
    JSONCallbackParameters callbackParameters;
    callbackParameters.callbackReceiver = this;
    callbackParameters.errorCallbackMethod = "handleFailedICEServerAddressUpdate";
    callbackParameters.jsonCallbackMethod = "handleSuccessfulICEServerAddressUpdate";

    qCDebug(domain_server_ice) << "Updating ice-server address in Directory Services API to"
        << (_iceServerSocket.isNull() ? "" : _iceServerSocket.getAddress().toString());

    static const QString DOMAIN_ICE_ADDRESS_UPDATE = "/api/v1/domains/%1/ice_server_address";

    DependencyManager::get<AccountManager>()->sendRequest(DOMAIN_ICE_ADDRESS_UPDATE.arg(uuidStringWithoutCurlyBraces(getID())),
                                                          AccountManagerAuth::Optional,
                                                          QNetworkAccessManager::PutOperation,
                                                          callbackParameters,
                                                          domainUpdateJSON.toUtf8());
}

void DomainServer::handleSuccessfulICEServerAddressUpdate(QNetworkReply* requestReply) {
    _sendICEServerAddressToMetaverseAPIInProgress = false;
    if (_sendICEServerAddressToMetaverseAPIRedo) {
        qCDebug(domain_server_ice) << "ice-server address (" << _iceServerSocket << ") updated with directory server, but has since changed.  redoing update...";
        _sendICEServerAddressToMetaverseAPIRedo = false;
        sendICEServerAddressToMetaverseAPI();
    } else {
        qCDebug(domain_server_ice) << "ice-server address (" << _iceServerSocket << ") updated with directory server.";
    }
}

void DomainServer::handleFailedICEServerAddressUpdate(QNetworkReply* requestReply) {
    _sendICEServerAddressToMetaverseAPIInProgress = false;
    if (_sendICEServerAddressToMetaverseAPIRedo) {
        // if we have new data, retry right away, even though the previous attempt didn't go well.
        _sendICEServerAddressToMetaverseAPIRedo = false;
        sendICEServerAddressToMetaverseAPI();
    } else {
        const int ICE_SERVER_UPDATE_RETRY_MS = 2 * 1000;

        qCWarning(domain_server_ice) << "PAGE: Failed to update ice-server address (" << _iceServerSocket <<
            ") with Directory Server (" << requestReply->url() << ") (critical error for auto-networking) error:" <<
            requestReply->errorString();
        qCWarning(domain_server_ice) << "\tRe-attempting in" << ICE_SERVER_UPDATE_RETRY_MS / 1000 << "seconds";

        QTimer::singleShot(ICE_SERVER_UPDATE_RETRY_MS, this, SLOT(sendICEServerAddressToMetaverseAPI()));
    }
}

void DomainServer::sendHeartbeatToIceServer() {
    if (!_iceServerSocket.getAddress().isNull()) {

        auto accountManager = DependencyManager::get<AccountManager>();
        auto limitedNodeList = DependencyManager::get<LimitedNodeList>();

        if (!accountManager->getAccountInfo().hasPrivateKey()) {
            qCWarning(domain_server_ice) << "Cannot send an ice-server heartbeat without a private key for signature.";
            qCWarning(domain_server_ice) << "Waiting for keypair generation to complete before sending ICE heartbeat.";

            if (!limitedNodeList->getSessionUUID().isNull()) {
                accountManager->generateNewDomainKeypair(limitedNodeList->getSessionUUID());
            } else {
                qCWarning(domain_server_ice) << "Attempting to send ICE server heartbeat with no domain ID. This is not supported";
            }

            return;
        }

        const int FAILOVER_NO_REPLY_ICE_HEARTBEATS { 6 };

        // increase the count of no reply ICE heartbeats and check the current value
        ++_noReplyICEHeartbeats;

        if (_noReplyICEHeartbeats > FAILOVER_NO_REPLY_ICE_HEARTBEATS) {
            qCWarning(domain_server_ice) << "There have been" << _noReplyICEHeartbeats - 1 << "heartbeats sent with no reply from the ice-server";
            qCWarning(domain_server_ice) << "Clearing the current ice-server socket and selecting a new candidate ice-server";

            // add the current address to our list of failed addresses
            _failedIceServerAddresses << _iceServerSocket.getAddress();

            // if we've failed to hear back for three heartbeats, we clear the current ice-server socket and attempt
            // to randomize a new one
            _iceServerSocket.clear();

            // reset the number of no reply ICE hearbeats
            _noReplyICEHeartbeats = 0;

            // reset the connection flag for ICE server
            _connectedToICEServer = false;
            sendICEServerAddressToMetaverseAPI();

            // randomize our ice-server address (and simultaneously look up any new hostnames for available ice-servers)
            randomizeICEServerAddress(true);
        }

        // NOTE: I'd love to specify the correct size for the packet here, but it's a little trickey with
        // QDataStream and the possibility of IPv6 address for the sockets.
        if (!_iceServerHeartbeatPacket) {
            _iceServerHeartbeatPacket = NLPacket::create(PacketType::ICEServerHeartbeat);
        }

        bool shouldRecreatePacket = false;

        if (_iceServerHeartbeatPacket->getPayloadSize() > 0) {
            // if either of our sockets have changed we need to re-sign the heartbeat
            // first read the sockets out from the current packet
            _iceServerHeartbeatPacket->seek(0);
            QDataStream heartbeatStream(_iceServerHeartbeatPacket.get());

            QUuid senderUUID;
            SockAddr publicSocket, localSocket;
            heartbeatStream >> senderUUID >> publicSocket >> localSocket;

            if (senderUUID != limitedNodeList->getSessionUUID()
                || publicSocket != limitedNodeList->getPublicSockAddr()
                || localSocket != limitedNodeList->getLocalSockAddr()) {
                shouldRecreatePacket = true;
            }
        } else {
            shouldRecreatePacket = true;
        }

        if (shouldRecreatePacket) {
            // either we don't have a heartbeat packet yet or some combination of sockets, ID and keypair have changed
            // and we need to make a new one

            // reset the position in the packet before writing
            _iceServerHeartbeatPacket->reset();

            // write our plaintext data to the packet
            QDataStream heartbeatDataStream(_iceServerHeartbeatPacket.get());
            heartbeatDataStream << limitedNodeList->getSessionUUID()
                << limitedNodeList->getPublicSockAddr() << limitedNodeList->getLocalSockAddr();

            // setup a QByteArray that points to the plaintext data
            auto plaintext = QByteArray::fromRawData(_iceServerHeartbeatPacket->getPayload(), _iceServerHeartbeatPacket->getPayloadSize());

            // generate a signature for the plaintext data in the packet
            auto signature = accountManager->getAccountInfo().signPlaintext(plaintext);

            // pack the signature with the data
            heartbeatDataStream << signature;
        }

        // send the heartbeat packet to the ice server now
        limitedNodeList->sendUnreliablePacket(*_iceServerHeartbeatPacket, _iceServerSocket);

    } else {
        qCDebug(domain_server_ice) << "Not sending ice-server heartbeat since there is no selected ice-server.";
        qCDebug(domain_server_ice) << "Waiting for" << _iceServerAddr << "host lookup response";
    }
}

void DomainServer::nodePingMonitor() {
    auto nodeList = DependencyManager::get<LimitedNodeList>();
    quint64 now = usecTimestampNow();

    nodeList->eachNode([now](const SharedNodePointer& node) {
        quint64 lastHeard = now - node->getLastHeardMicrostamp();
        if (lastHeard > 2 * USECS_PER_SECOND) {
            QString username;
            DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());
            if (nodeData) {
                username = nodeData->getUsername();
            }
            qCDebug(domain_server) << "Haven't heard from " << node->getPublicSocket() << username << " in " << lastHeard / USECS_PER_MSEC << " msec";
        }
    });
}

void DomainServer::processOctreeDataPersistMessage(QSharedPointer<ReceivedMessage> message) {
    auto data = message->readAll();
    qDebug() << "Received octree data persist message" << (data.size() / 1000) << "kbytes.";
    // Entity-server is the source of truth. Domain-server does not store entity data on disk.
}

void DomainServer::processOctreeDataRequestMessage(QSharedPointer<ReceivedMessage> message) {
    qDebug() << "Got request for octree data from " << message->getSenderSockAddr();

    // Entity-server is autonomous and does not request data from domain-server.
    // This handler exists only for backward compatibility with old entity-servers.
    auto reply = NLPacketList::create(PacketType::OctreeDataFileReply, QByteArray(), true, true);
    reply->writePrimitive(false);

    auto nodeList = DependencyManager::get<LimitedNodeList>();
    nodeList->sendPacketList(std::move(reply), message->getSenderSockAddr());
}

void DomainServer::processNodeJSONStatsPacket(QSharedPointer<ReceivedMessage> packetList, SharedNodePointer sendingNode) {
    auto nodeData = static_cast<DomainServerNodeData*>(sendingNode->getLinkedData());
    if (nodeData) {
        nodeData->updateJSONStats(packetList->getMessage());
    }
}

QJsonObject DomainServer::jsonForSocket(const SockAddr& socket) {
    QJsonObject socketJSON;

    socketJSON["ip"] = socket.getAddress().toString();
    socketJSON["port"] = socket.getPort();

    return socketJSON;
}

const char JSON_KEY_UUID[] = "uuid";
const char JSON_KEY_TYPE[] = "type";
const char JSON_KEY_PUBLIC_SOCKET[] = "public";
const char JSON_KEY_LOCAL_SOCKET[] = "local";
const char JSON_KEY_POOL[] = "pool";
const char JSON_KEY_PENDING_CREDITS[] = "pending_credits";
const char JSON_KEY_UPTIME[] = "uptime";
const char JSON_KEY_USERNAME[] = "username";
const char JSON_KEY_VERSION[] = "version";
QJsonObject DomainServer::jsonObjectForNode(const SharedNodePointer& node) {
    QJsonObject nodeJson;

    // re-format the type name so it matches the target name
    QString nodeTypeName = NodeType::getNodeTypeName(node->getType());
    nodeTypeName = nodeTypeName.toLower();
    nodeTypeName.replace(' ', '-');

    // add the node UUID
    nodeJson[JSON_KEY_UUID] = uuidStringWithoutCurlyBraces(node->getUUID());

    // add the node type
    nodeJson[JSON_KEY_TYPE] = nodeTypeName;

    // add the node socket information
    nodeJson[JSON_KEY_PUBLIC_SOCKET] = jsonForSocket(node->getPublicSocket());
    nodeJson[JSON_KEY_LOCAL_SOCKET] = jsonForSocket(node->getLocalSocket());

    // add the node uptime in our list
    nodeJson[JSON_KEY_UPTIME] = QString::number(double(QDateTime::currentMSecsSinceEpoch() - node->getWakeTimestamp()) / 1000.0);

    // if the node has pool information, add it
    DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());

    // add the node username, if it exists
    nodeJson[JSON_KEY_USERNAME] = nodeData->getUsername();
    nodeJson[JSON_KEY_VERSION] = nodeData->getNodeVersion();

    DomainAssignmentPointer matchingAssignment = _allAssignments.value(nodeData->getAssignmentUUID());
    if (matchingAssignment) {
        nodeJson[JSON_KEY_POOL] = matchingAssignment->getPool();
    }

    return nodeJson;
}

QDir pathForAssignmentScriptsDirectory() {
    static const QString SCRIPTS_DIRECTORY_NAME = "/scripts/";

    QDir directory(PathUtils::getAppDataPath() + SCRIPTS_DIRECTORY_NAME);
    if (!directory.exists()) {
        directory.mkpath(".");
        qInfo() << "Created path to " << directory.path();
    }

    return directory;
}

QString pathForAssignmentScript(const QUuid& assignmentUUID) {
    QDir directory = pathForAssignmentScriptsDirectory();
    // append the UUID for this script as the new filename, remove the curly braces
    return directory.absoluteFilePath(uuidStringWithoutCurlyBraces(assignmentUUID));
}




void DomainServer::refreshStaticAssignmentAndAddToQueue(DomainAssignmentPointer& assignment) {
    QUuid oldUUID = assignment->getUUID();
    assignment->resetUUID();

    qDebug() << "Reset UUID for assignment -" << *assignment.data() << "- and added to queue. Old UUID was"
        << uuidStringWithoutCurlyBraces(oldUUID);

    if (assignment->getType() == Assignment::AgentType && assignment->getPayload().isEmpty()) {
        // if this was an Agent without a script URL, we need to rename the old file so it can be retrieved at the new UUID
        QFile::rename(pathForAssignmentScript(oldUUID), pathForAssignmentScript(assignment->getUUID()));
    }

    // add the static assignment back under the right UUID, and to the queue
    _allAssignments.insert(assignment->getUUID(), assignment);
    _unfulfilledAssignments.enqueue(assignment);
}

static const QString BROADCASTING_SETTINGS_KEY = "broadcasting";

struct ReplicationServerInfo {
    NodeType_t nodeType;
    SockAddr sockAddr;
};

ReplicationServerInfo serverInformationFromSettings(QVariantMap serverMap, ReplicationServerDirection direction) {
    static const QString REPLICATION_SERVER_ADDRESS = "address";
    static const QString REPLICATION_SERVER_PORT = "port";
    static const QString REPLICATION_SERVER_TYPE = "server_type";

    if (serverMap.contains(REPLICATION_SERVER_ADDRESS) && serverMap.contains(REPLICATION_SERVER_PORT)
        && serverMap.contains(REPLICATION_SERVER_TYPE)) {

        auto nodeType = NodeType::fromString(serverMap[REPLICATION_SERVER_TYPE].toString());

        ReplicationServerInfo serverInfo;

        if (direction == Upstream) {
            serverInfo.nodeType = NodeType::upstreamType(nodeType);
        } else if (direction == Downstream) {
            serverInfo.nodeType = NodeType::downstreamType(nodeType);
        }

        // read the address and port and construct a SockAddr from them
        serverInfo.sockAddr = {
            SocketType::UDP,
            serverMap[REPLICATION_SERVER_ADDRESS].toString(),
            (quint16) serverMap[REPLICATION_SERVER_PORT].toString().toInt()
        };

        return serverInfo;
    }

    return { NodeType::Unassigned, SockAddr() };
}

void DomainServer::updateReplicationNodes(ReplicationServerDirection direction) {

    auto broadcastSettingsVariant = _settingsManager.valueForKeyPath(BROADCASTING_SETTINGS_KEY);

    if (broadcastSettingsVariant.isValid()) {
        auto nodeList = DependencyManager::get<LimitedNodeList>();
        std::vector<SockAddr> replicationNodesInSettings;

        auto replicationSettings = broadcastSettingsVariant.toMap();

        QString serversKey = direction == Upstream ? "upstream_servers" : "downstream_servers";
        QString replicationDirection = direction == Upstream ? "upstream" : "downstream";

        if (replicationSettings.contains(serversKey)) {
            auto serversSettings = replicationSettings.value(serversKey).toList();

            std::vector<SockAddr> knownReplicationNodes;
            nodeList->eachNode([direction, &knownReplicationNodes](const SharedNodePointer& otherNode) {
                if ((direction == Upstream && NodeType::isUpstream(otherNode->getType()))
                    || (direction == Downstream && NodeType::isDownstream(otherNode->getType()))) {
                    knownReplicationNodes.push_back(otherNode->getPublicSocket());
                }
            });

            for (auto& server : serversSettings) {
                auto replicationServer = serverInformationFromSettings(server.toMap(), direction);

                if (!replicationServer.sockAddr.isNull() && replicationServer.nodeType != NodeType::Unassigned) {
                    // make sure we have the settings we need for this replication server
                    replicationNodesInSettings.push_back(replicationServer.sockAddr);

                    bool knownNode = find(knownReplicationNodes.cbegin(), knownReplicationNodes.cend(),
                                          replicationServer.sockAddr) != knownReplicationNodes.cend();
                    if (!knownNode) {
                        // manually add the replication node to our node list
                        auto node = nodeList->addOrUpdateNode(QUuid::createUuid(), replicationServer.nodeType,
                                                              replicationServer.sockAddr, replicationServer.sockAddr,
                                                              Node::NULL_LOCAL_ID, false, direction == Upstream);
                        node->setIsForcedNeverSilent(true);

                        qDebug() << "Adding" << (direction == Upstream ? "upstream" : "downstream")
                            << "node:" << node->getUUID() << replicationServer.sockAddr;

                        // manually activate the public socket for the replication node
                        node->activatePublicSocket();
                    }
                }

            }
        }

        // enumerate the nodes to determine which are no longer downstream for this domain
        // collect them in a vector to separately remove them with handleKillNode (since eachNode has a read lock and
        // we cannot recursively take the write lock required by handleKillNode)
        std::vector<SharedNodePointer> nodesToKill;
        nodeList->eachNode([&direction, &replicationNodesInSettings, &replicationDirection, &nodesToKill](const SharedNodePointer& otherNode) {
            if ((direction == Upstream && NodeType::isUpstream(otherNode->getType()))
                || (direction == Downstream && NodeType::isDownstream(otherNode->getType()))) {
                bool nodeInSettings = find(replicationNodesInSettings.cbegin(), replicationNodesInSettings.cend(),
                                           otherNode->getPublicSocket()) != replicationNodesInSettings.cend();
                if (!nodeInSettings) {
                    qDebug() << "Removing" << replicationDirection
                        << "node:" << otherNode->getUUID() << otherNode->getPublicSocket();
                    nodesToKill.push_back(otherNode);
                }
            }
        });

        for (auto& node : nodesToKill) {
            handleKillNode(node);
        }
    }
}

void DomainServer::updateDownstreamNodes() {
    updateReplicationNodes(Downstream);
}

void DomainServer::updateUpstreamNodes() {
    updateReplicationNodes(Upstream);
}



void DomainServer::updateReplicatedNodes() {
    // Make sure we have downstream nodes in our list
    static const QString REPLICATED_USERS_KEY = "users";
    _replicatedUsernames.clear();

    auto replicationVariant = _settingsManager.valueForKeyPath(BROADCASTING_SETTINGS_KEY);
    if (replicationVariant.isValid()) {
        auto replicationSettings = replicationVariant.toMap();
        if (replicationSettings.contains(REPLICATED_USERS_KEY)) {
            auto usersSettings = replicationSettings.value(REPLICATED_USERS_KEY).toList();
            for (auto& username : usersSettings) {
                _replicatedUsernames.push_back(username.toString().toLower());
            }
        }
    }

    auto nodeList = DependencyManager::get<LimitedNodeList>();
    nodeList->eachMatchingNode([](const SharedNodePointer& otherNode) -> bool {
            return otherNode->getType() == NodeType::Agent;
        }, [this](const SharedNodePointer& otherNode) {
            auto shouldReplicate = shouldReplicateNode(*otherNode);
            auto isReplicated = otherNode->isReplicated();
            if (isReplicated && !shouldReplicate) {
                qDebug() << "Setting node to NOT be replicated:"
                    << otherNode->getPermissions().getVerifiedUserName() << otherNode->getUUID();
            } else if (!isReplicated && shouldReplicate) {
                qDebug() << "Setting node to replicated:"
                    << otherNode->getPermissions().getVerifiedUserName() << otherNode->getUUID();
            }
            otherNode->setIsReplicated(shouldReplicate);
        }
    );
}

bool DomainServer::shouldReplicateNode(const Node& node) {
    if (node.getType() == NodeType::Agent) {
        QString verifiedUsername = node.getPermissions().getVerifiedUserName();

        // Both the verified username and usernames in _replicatedUsernames are lowercase, so
        // comparisons here are case-insensitive.
        auto it = find(_replicatedUsernames.cbegin(), _replicatedUsernames.cend(), verifiedUsername);
        return it != _replicatedUsernames.end();
    } else {
        return false;
    }
};


bool DomainServer::isAssetServerEnabled() {
    static const QString ASSET_SERVER_ENABLED_KEYPATH = "asset_server.enabled";
    return _settingsManager.valueOrDefaultValueForKeyPath(ASSET_SERVER_ENABLED_KEYPATH).toBool();
}

void DomainServer::nodeAdded(SharedNodePointer node) {
    // we don't use updateNodeWithData, so add the DomainServerNodeData to the node here
    node->setLinkedData(std::unique_ptr<DomainServerNodeData> { new DomainServerNodeData() });
}

void DomainServer::nodeKilled(SharedNodePointer node) {
    // if this peer connected via ICE then remove them from our ICE peers hash
    _gatekeeper.cleanupICEPeerForNode(node->getUUID());

    DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());

    if (nodeData) {
        // if this node's UUID matches a static assignment we need to throw it back in the assignment queue
        if (!nodeData->getAssignmentUUID().isNull()) {
            DomainAssignmentPointer matchedAssignment = _allAssignments.take(nodeData->getAssignmentUUID());

            if (matchedAssignment && matchedAssignment->isStatic()) {
                refreshStaticAssignmentAndAddToQueue(matchedAssignment);
            }
        }

        // cleanup the connection secrets that we set up for this node (on the other nodes)
        foreach (const QUuid& otherNodeSessionUUID, nodeData->getSessionSecretHash().keys()) {
            SharedNodePointer otherNode = DependencyManager::get<LimitedNodeList>()->nodeWithUUID(otherNodeSessionUUID);
            if (otherNode) {
                static_cast<DomainServerNodeData*>(otherNode->getLinkedData())->getSessionSecretHash().remove(node->getUUID());
            }
        }

        if (node->getType() == NodeType::Agent) {
            // if this node was an Agent ask DomainServerNodeData to remove the interpolation we potentially stored
            nodeData->removeOverrideForKey(USERNAME_UUID_REPLACEMENT_STATS_KEY,
                    uuidStringWithoutCurlyBraces(node->getUUID()));

            // if this node is a user (unassigned Agent), signal
            if (!nodeData->wasAssigned()) {
                emit userDisconnected();
            }
        }
    }

    broadcastNodeDisconnect(node);
}

DomainAssignmentPointer DomainServer::dequeueMatchingAssignment(const QUuid& assignmentUUID, NodeType_t nodeType) {
    QQueue<DomainAssignmentPointer>::iterator i = _unfulfilledAssignments.begin();

    while (i != _unfulfilledAssignments.end()) {
        if (i->data()->getType() == Assignment::typeForNodeType(nodeType)
            && i->data()->getUUID() == assignmentUUID) {
            // we have an unfulfilled assignment to return

            // return the matching assignment
            return _unfulfilledAssignments.takeAt(i - _unfulfilledAssignments.begin());
        } else {
            ++i;
        }
    }

    return DomainAssignmentPointer();
}

DomainAssignmentPointer DomainServer::deployableAssignmentForRequest(const Assignment& requestAssignment) {
    // this is an unassigned client talking to us directly for an assignment
    // go through our queue and see if there are any assignments to give out
    QQueue<DomainAssignmentPointer>::iterator sharedAssignment = _unfulfilledAssignments.begin();

    while (sharedAssignment != _unfulfilledAssignments.end()) {
        Assignment* assignment = sharedAssignment->data();
        bool requestIsAllTypes = requestAssignment.getType() == Assignment::AllTypes;
        bool assignmentTypesMatch = assignment->getType() == requestAssignment.getType();
        bool neitherHasPool = assignment->getPool().isEmpty() && requestAssignment.getPool().isEmpty();
        bool assignmentPoolsMatch = assignment->getPool() == requestAssignment.getPool();

        if ((requestIsAllTypes || assignmentTypesMatch) && (neitherHasPool || assignmentPoolsMatch)) {

            // remove the assignment from the queue
            DomainAssignmentPointer deployableAssignment = _unfulfilledAssignments.takeAt(sharedAssignment
                                                                                          - _unfulfilledAssignments.begin());

            // until we get a connection for this assignment
            // put assignment back in queue but stick it at the back so the others have a chance to go out
            _unfulfilledAssignments.enqueue(deployableAssignment);

            // stop looping, we've handed out an assignment
            return deployableAssignment;
        } else {
            // push forward the iterator to check the next assignment
            ++sharedAssignment;
        }
    }

    return DomainAssignmentPointer();
}

void DomainServer::addStaticAssignmentsToQueue() {

    // if the domain-server has just restarted,
    // check if there are static assignments that we need to throw into the assignment queue
    auto sharedAssignments = _allAssignments.values();

    // sort the assignments to put the server/mixer assignments first
    std::sort(sharedAssignments.begin(), sharedAssignments.end(), [](DomainAssignmentPointer a, DomainAssignmentPointer b){
        if (a->getType() == b->getType()) {
            return true;
        } else if (a->getType() != Assignment::AgentType && b->getType() != Assignment::AgentType) {
            return a->getType() < b->getType();
        } else {
            return a->getType() != Assignment::AgentType;
        }
    });

    auto staticAssignment = sharedAssignments.begin();

    while (staticAssignment != sharedAssignments.end()) {
        // add any of the un-matched static assignments to the queue

        // enumerate the nodes and check if there is one with an attached assignment with matching UUID
        if (!DependencyManager::get<LimitedNodeList>()->nodeWithUUID((*staticAssignment)->getUUID())) {
            // this assignment has not been fulfilled - reset the UUID and add it to the assignment queue
            refreshStaticAssignmentAndAddToQueue(*staticAssignment);
        }

        ++staticAssignment;
    }
}

void DomainServer::processPathQueryPacket(QSharedPointer<ReceivedMessage> message) {
    // this is a query for the viewpoint resulting from a path
    // first pull the query path from the packet

    // figure out how many bytes the sender said this path is
    quint16 numPathBytes;
    message->readPrimitive(&numPathBytes);

    if (numPathBytes <= message->getBytesLeftToRead()) {
        // the number of path bytes makes sense for the sent packet - pull out the path
        QString pathQuery = QString::fromUtf8(message->getRawMessage() + message->getPosition(), numPathBytes);

        // our settings contain paths that start with a leading slash, so make sure this query has that
        if (!pathQuery.startsWith("/")) {
            pathQuery.prepend("/");
        }

        const QString PATHS_SETTINGS_KEYPATH_FORMAT = "%1.%2";
        const QString PATH_VIEWPOINT_KEY = "viewpoint";
        const QString INDEX_PATH = "/";

        QString responseViewpoint;

        // check out paths in the _configMap to see if we have a match
        auto pathsVariant = _settingsManager.valueForKeyPath(SETTINGS_PATHS_KEY);

        auto lowerPathQuery = pathQuery.toLower();

        if (pathsVariant.canConvert<QVariantMap>()) {
            auto pathsMap = pathsVariant.toMap();

            // enumerate the paths and look case-insensitively for a matching one
            for (auto it = pathsMap.constKeyValueBegin(); it != pathsMap.constKeyValueEnd(); ++it) {
                if ((*it).first.toLower() == lowerPathQuery) {
                    responseViewpoint = (*it).second.toMap()[PATH_VIEWPOINT_KEY].toString().toLower();
                    break;
                }
            }
        }

        if (responseViewpoint.isEmpty() && pathQuery == INDEX_PATH) {
            const QString DEFAULT_INDEX_PATH = "/0,0,0/0,0,0,1";
            responseViewpoint = DEFAULT_INDEX_PATH;
        }

        if (!responseViewpoint.isEmpty()) {
            // we got a match, respond with the resulting viewpoint
            auto nodeList = DependencyManager::get<LimitedNodeList>();

            if (!responseViewpoint.isEmpty()) {
                QByteArray viewpointUTF8 = responseViewpoint.toUtf8();

                // prepare a packet for the response
                auto pathResponsePacket = NLPacket::create(PacketType::DomainServerPathResponse, -1, true);

                // check the number of bytes the viewpoint is
                quint16 numViewpointBytes = viewpointUTF8.size();

                // are we going to be able to fit this response viewpoint in a packet?
                if (numPathBytes + numViewpointBytes + sizeof(numViewpointBytes) + sizeof(numPathBytes)
                        < (unsigned long) pathResponsePacket->bytesAvailableForWrite()) {
                    // append the number of bytes this path is
                    pathResponsePacket->writePrimitive(numPathBytes);

                    // append the path itself
                    pathResponsePacket->write(pathQuery.toUtf8());

                    // append the number of bytes the resulting viewpoint is
                    pathResponsePacket->writePrimitive(numViewpointBytes);

                    // append the viewpoint itself
                    pathResponsePacket->write(viewpointUTF8);

                    qDebug() << "Sending a viewpoint response for path query" << pathQuery << "-" << viewpointUTF8;

                    // send off the packet - see if we can associate this outbound data to a particular node
                    // TODO: does this senderSockAddr always work for a punched DS client?
                    nodeList->sendPacket(std::move(pathResponsePacket), message->getSenderSockAddr());
                }
            }

        } else {
            // we don't respond if there is no match - this may need to change once this packet
            // query/response is made reliable
            qDebug() << "No match for path query" << pathQuery << "- refusing to respond.";
        }
    }
}

void DomainServer::processNodeDisconnectRequestPacket(QSharedPointer<ReceivedMessage> message) {
    // This packet has been matched to a source node and they're asking not to be in the domain anymore
    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();

    auto localID = message->getSourceID();
    qDebug() << "Received a disconnect request from node with local ID" << localID;

    // we want to check what type this node was before going to kill it so that we can avoid sending the RemovedNode
    // packet to nodes that don't care about this type
    auto nodeToKill = limitedNodeList->nodeWithLocalID(localID);

    if (nodeToKill) {
        handleKillNode(nodeToKill);
    }
}

void DomainServer::handleKillNode(SharedNodePointer nodeToKill) {
    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();
    const QUuid& nodeUUID = nodeToKill->getUUID();

    limitedNodeList->killNodeWithUUID(nodeUUID);
}

void DomainServer::broadcastNodeDisconnect(const SharedNodePointer& disconnectedNode) {
    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();

    static auto removedNodePacket = NLPacket::create(PacketType::DomainServerRemovedNode, NUM_BYTES_RFC4122_UUID, true);

    removedNodePacket->reset();
    removedNodePacket->write(disconnectedNode->getUUID().toRfc4122());

    // broadcast out the DomainServerRemovedNode message
    limitedNodeList->eachMatchingNode([this, &disconnectedNode](const SharedNodePointer& otherNode) -> bool {
        // only send the removed node packet to nodes that care about the type of node this was
        return isInInterestSet(otherNode, disconnectedNode);
    }, [&limitedNodeList](const SharedNodePointer& otherNode){
        auto removedNodePacketCopy = NLPacket::createCopy(*removedNodePacket);
        limitedNodeList->sendPacket(std::move(removedNodePacketCopy), *otherNode);
    });
}

void DomainServer::processICEServerHeartbeatDenialPacket(QSharedPointer<ReceivedMessage> message) {
    static const int NUM_HEARTBEAT_DENIALS_FOR_KEYPAIR_REGEN = 3;

    if (++_numHeartbeatDenials > NUM_HEARTBEAT_DENIALS_FOR_KEYPAIR_REGEN) {
        qCDebug(domain_server_ice) << "Received" << NUM_HEARTBEAT_DENIALS_FOR_KEYPAIR_REGEN << "heartbeat denials from ice-server"
            << "- re-generating keypair now";

        // we've hit our threshold of heartbeat denials, trigger a keypair re-generation
        auto limitedNodeList = DependencyManager::get<LimitedNodeList>();
        DependencyManager::get<AccountManager>()->generateNewDomainKeypair(limitedNodeList->getSessionUUID());

        // reset our number of heartbeat denials
        _numHeartbeatDenials = 0;
    }

    // even though we can't get into this ice-server it is responding to us, so we reset our number of no-reply heartbeats
    _noReplyICEHeartbeats = 0;
}

void DomainServer::processICEServerHeartbeatACK(QSharedPointer<ReceivedMessage> message) {
    // we don't do anything with this ACK other than use it to tell us to keep talking to the same ice-server
    _noReplyICEHeartbeats = 0;

    if (!_connectedToICEServer) {
        _connectedToICEServer = true;
        sendICEServerAddressToMetaverseAPI();
        qCInfo(domain_server_ice) << "Connected to ice-server at" << _iceServerSocket;
    }
}

void DomainServer::handleKeypairChange() {
    if (_iceServerHeartbeatPacket) {
        // reset the payload size of the ice-server heartbeat packet - this causes the packet to be re-generated
        // the next time we go to send an ice-server heartbeat
        _iceServerHeartbeatPacket->setPayloadSize(0);

        // send a heartbeat to the ice server immediately
        sendHeartbeatToIceServer();
    }
}

void DomainServer::handleICEHostInfo(const QHostInfo& hostInfo) {
    // clear the ICE address lookup ID so that it can fire again
    _iceAddressLookupID = INVALID_ICE_LOOKUP_ID;

    // enumerate the returned addresses and collect only valid IPv4 addresses
    QList<QHostAddress> sanitizedAddresses = hostInfo.addresses();
    auto it = sanitizedAddresses.begin();
    while (it != sanitizedAddresses.end()) {
        if (!it->isNull() && it->protocol() == QAbstractSocket::IPv4Protocol) {
            ++it;
        } else {
            it = sanitizedAddresses.erase(it);
        }
    }

    if (hostInfo.error() != QHostInfo::NoError || sanitizedAddresses.empty()) {
        qCWarning(domain_server_ice) << "IP address lookup failed for" << _iceServerAddr << ":" << hostInfo.errorString();

        // if we don't have an ICE server to use yet, trigger a retry
        if (_iceServerSocket.isNull()) {
            const int ICE_ADDRESS_LOOKUP_RETRY_MS = 1000;

            QTimer::singleShot(ICE_ADDRESS_LOOKUP_RETRY_MS, this, SLOT(updateICEServerAddresses()));
        }

    } else {
        int countBefore = _iceServerAddresses.count();

        _iceServerAddresses = sanitizedAddresses;

        if (countBefore == 0) {
            qCInfo(domain_server_ice) << "Found" << _iceServerAddresses.count() << "ice-server IP addresses for" << _iceServerAddr;
        }

        if (_iceServerSocket.isNull()) {
            // we don't have a candidate ice-server yet, pick now (without triggering a host lookup since we just did one)
            randomizeICEServerAddress(false);
        }
    }
}

void DomainServer::randomizeICEServerAddress(bool shouldTriggerHostLookup) {
    if (shouldTriggerHostLookup) {
        updateICEServerAddresses();
    }

    // create a list by removing the already failed ice-server addresses
    auto candidateICEAddresses = _iceServerAddresses;

    auto it = candidateICEAddresses.begin();

    while (it != candidateICEAddresses.end()) {
        if (_failedIceServerAddresses.contains(*it)) {
            // we already tried this address and it failed, remove it from list of candidates
            it = candidateICEAddresses.erase(it);
        } else {
            // keep this candidate, it hasn't failed yet
            ++it;
        }
    }

    if (candidateICEAddresses.empty()) {
        // we ended up with an empty list since everything we've tried has failed
        // so clear the set of failed addresses and start going through them again

        qCWarning(domain_server_ice) <<
            "PAGE: All current ice-server addresses have failed - re-attempting all current addresses for"
            << _iceServerAddr;

        _failedIceServerAddresses.clear();
        candidateICEAddresses = _iceServerAddresses;
    }

    // of the list of available addresses that we haven't tried, pick a random one
    int maxIndex = candidateICEAddresses.size() - 1;
    int indexToTry = 0;

    if (maxIndex > 0) {
        static std::random_device randomDevice;
        static std::mt19937 generator(randomDevice());
        std::uniform_int_distribution<> distribution(0, maxIndex);

        indexToTry = distribution(generator);
    }

    _iceServerSocket = SockAddr { SocketType::UDP, candidateICEAddresses[indexToTry], ICE_SERVER_DEFAULT_PORT };
    qCInfo(domain_server_ice) << "Set candidate ice-server socket to" << _iceServerSocket;

    // clear our number of hearbeat denials, this should be re-set on ice-server change
    _numHeartbeatDenials = 0;

    // immediately fire an ICE heartbeat once we've picked a candidate ice-server
    sendHeartbeatToIceServer();

    // immediately send an update to the directory services API when our ice-server changes
    sendICEServerAddressToMetaverseAPI();
}

void DomainServer::setupGroupCacheRefresh() {
    const int REFRESH_GROUPS_INTERVAL_MSECS = 15 * MSECS_PER_SECOND;

    if (!_metaverseGroupCacheTimer) {
        // setup a timer to refresh this server's cached group details
        _metaverseGroupCacheTimer = new QTimer { this };
        connect(_metaverseGroupCacheTimer, &QTimer::timeout, &_gatekeeper, &DomainGatekeeper::refreshGroupsCache);
        _metaverseGroupCacheTimer->start(REFRESH_GROUPS_INTERVAL_MSECS);
    }
}



void DomainServer::processAvatarZonePresencePacket(QSharedPointer<ReceivedMessage> message) {
    // FIXME: this, and PacketType::AvatarZonePresence in general, appear to be unused/unimplemented
    QUuid avatarID = QUuid::fromRfc4122(message->readWithoutCopy(NUM_BYTES_RFC4122_UUID));
    //QUuid zoneID = QUuid::fromRfc4122(message->readWithoutCopy(NUM_BYTES_RFC4122_UUID));

    if (avatarID.isNull()) {
        qCWarning(domain_server) << "Ignoring null avatar presence";
        return;
    }
}
