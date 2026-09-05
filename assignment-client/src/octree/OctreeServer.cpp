//
//  OctreeServer.cpp
//  assignment-client/src/octree
//
//  Created by Brad Hefta-Gaub on 9/16/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "OctreeServer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstdio>
#include <time.h>

#include <AccountManager.h>
#include <Gzip.h>
#include <LogHandler.h>
#include <shared/NetworkUtils.h>
#include <NumericalConstants.h>
#include <UUID.h>

#include "../AssignmentClient.h"

#include "OctreeQueryNode.h"
#include "OctreeServerConsts.h"
#include <QtCore/QStandardPaths>
#include <PathUtils.h>
#include <QtCore/QDir>

#include <OctreeDataUtils.h>
#include <ThreadHelpers.h>

Q_LOGGING_CATEGORY(octree_server, "hifi.octree-server")

int OctreeServer::_clientCount = 0;
const int MOVING_AVERAGE_SAMPLE_COUNTS = 1000;

float OctreeServer::SKIP_TIME = -1.0f; // use this for trackXXXTime() calls for non-times

SimpleMovingAverage OctreeServer::_averageLoopTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageInsideTime(MOVING_AVERAGE_SAMPLE_COUNTS);

SimpleMovingAverage OctreeServer::_averageEncodeTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageShortEncodeTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageLongEncodeTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageExtraLongEncodeTime(MOVING_AVERAGE_SAMPLE_COUNTS);
int OctreeServer::_extraLongEncode = 0;
int OctreeServer::_longEncode = 0;
int OctreeServer::_shortEncode = 0;
int OctreeServer::_noEncode = 0;

SimpleMovingAverage OctreeServer::_averageTreeWaitTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageTreeShortWaitTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageTreeLongWaitTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageTreeExtraLongWaitTime(MOVING_AVERAGE_SAMPLE_COUNTS);
int OctreeServer::_extraLongTreeWait = 0;
int OctreeServer::_longTreeWait = 0;
int OctreeServer::_shortTreeWait = 0;
int OctreeServer::_noTreeWait = 0;

SimpleMovingAverage OctreeServer::_averageTreeTraverseTime(MOVING_AVERAGE_SAMPLE_COUNTS);

SimpleMovingAverage OctreeServer::_averageNodeWaitTime(MOVING_AVERAGE_SAMPLE_COUNTS);

SimpleMovingAverage OctreeServer::_averageCompressAndWriteTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageShortCompressTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageLongCompressTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageExtraLongCompressTime(MOVING_AVERAGE_SAMPLE_COUNTS);
int OctreeServer::_extraLongCompress = 0;
int OctreeServer::_longCompress = 0;
int OctreeServer::_shortCompress = 0;
int OctreeServer::_noCompress = 0;

SimpleMovingAverage OctreeServer::_averagePacketSendingTime(MOVING_AVERAGE_SAMPLE_COUNTS);
int OctreeServer::_noSend = 0;

SimpleMovingAverage OctreeServer::_averageProcessWaitTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageProcessShortWaitTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageProcessLongWaitTime(MOVING_AVERAGE_SAMPLE_COUNTS);
SimpleMovingAverage OctreeServer::_averageProcessExtraLongWaitTime(MOVING_AVERAGE_SAMPLE_COUNTS);
int OctreeServer::_extraLongProcessWait = 0;
int OctreeServer::_longProcessWait = 0;
int OctreeServer::_shortProcessWait = 0;
int OctreeServer::_noProcessWait = 0;

static const QString PERSIST_FILE_DOWNLOAD_PATH = "/models.json.gz";
static const double NANOSECONDS_PER_SECOND = 1000000.0;;


void OctreeServer::resetSendingStats() {
    _averageLoopTime.reset();

    _averageEncodeTime.reset();
    _averageShortEncodeTime.reset();
    _averageLongEncodeTime.reset();
    _averageExtraLongEncodeTime.reset();
    _extraLongEncode = 0;
    _longEncode = 0;
    _shortEncode = 0;
    _noEncode = 0;

    _averageInsideTime.reset();
    _averageTreeWaitTime.reset();
    _averageTreeShortWaitTime.reset();
    _averageTreeLongWaitTime.reset();
    _averageTreeExtraLongWaitTime.reset();
    _extraLongTreeWait = 0;
    _longTreeWait = 0;
    _shortTreeWait = 0;
    _noTreeWait = 0;

    _averageTreeTraverseTime.reset();

    _averageNodeWaitTime.reset();

    _averageCompressAndWriteTime.reset();
    _averageShortCompressTime.reset();
    _averageLongCompressTime.reset();
    _averageExtraLongCompressTime.reset();
    _extraLongCompress = 0;
    _longCompress = 0;
    _shortCompress = 0;
    _noCompress = 0;

    _averagePacketSendingTime.reset();
    _noSend = 0;

    _averageProcessWaitTime.reset();
    _averageProcessShortWaitTime.reset();
    _averageProcessLongWaitTime.reset();
    _averageProcessExtraLongWaitTime.reset();
    _extraLongProcessWait = 0;
    _longProcessWait = 0;
    _shortProcessWait = 0;
    _noProcessWait = 0;
}

void OctreeServer::trackEncodeTime(float time) {
    const float MAX_SHORT_TIME = 10.0f;
    const float MAX_LONG_TIME = 100.0f;

    if (time == SKIP_TIME) {
        _noEncode++;
    } else {
        if (time <= MAX_SHORT_TIME) {
            _shortEncode++;
            _averageShortEncodeTime.updateAverage(time);
        } else if (time <= MAX_LONG_TIME) {
            _longEncode++;
            _averageLongEncodeTime.updateAverage(time);
        } else {
            _extraLongEncode++;
            _averageExtraLongEncodeTime.updateAverage(time);
        }
        _averageEncodeTime.updateAverage(time);
    }
}

void OctreeServer::trackTreeWaitTime(float time) {
    const float MAX_SHORT_TIME = 10.0f;
    const float MAX_LONG_TIME = 100.0f;
    if (time == SKIP_TIME) {
        _noTreeWait++;
    } else {
        if (time <= MAX_SHORT_TIME) {
            _shortTreeWait++;
            _averageTreeShortWaitTime.updateAverage(time);
        } else if (time <= MAX_LONG_TIME) {
            _longTreeWait++;
            _averageTreeLongWaitTime.updateAverage(time);
        } else {
            _extraLongTreeWait++;
            _averageTreeExtraLongWaitTime.updateAverage(time);
        }
        _averageTreeWaitTime.updateAverage(time);
    }
}

void OctreeServer::trackCompressAndWriteTime(float time) {
    const float MAX_SHORT_TIME = 10.0f;
    const float MAX_LONG_TIME = 100.0f;
    if (time == SKIP_TIME) {
        _noCompress++;
    } else {
        if (time <= MAX_SHORT_TIME) {
            _shortCompress++;
            _averageShortCompressTime.updateAverage(time);
        } else if (time <= MAX_LONG_TIME) {
            _longCompress++;
            _averageLongCompressTime.updateAverage(time);
        } else {
            _extraLongCompress++;
            _averageExtraLongCompressTime.updateAverage(time);
        }
        _averageCompressAndWriteTime.updateAverage(time);
    }
}

void OctreeServer::trackPacketSendingTime(float time) {
    if (time == SKIP_TIME) {
        _noSend++;
    } else {
        _averagePacketSendingTime.updateAverage(time);
    }
}

void OctreeServer::trackProcessWaitTime(float time) {
    const float MAX_SHORT_TIME = 10.0f;
    const float MAX_LONG_TIME = 100.0f;
    if (time == SKIP_TIME) {
        _noProcessWait++;
    } else {
        if (time <= MAX_SHORT_TIME) {
            _shortProcessWait++;
            _averageProcessShortWaitTime.updateAverage(time);
        } else if (time <= MAX_LONG_TIME) {
            _longProcessWait++;
            _averageProcessLongWaitTime.updateAverage(time);
        } else {
            _extraLongProcessWait++;
            _averageProcessExtraLongWaitTime.updateAverage(time);
        }
        _averageProcessWaitTime.updateAverage(time);
    }
}

OctreeServer::OctreeServer(ReceivedMessage& message) :
    ThreadedAssignment(message),
    _argc(0),
    _argv(nullptr),
    _parsedArgV(nullptr),
    _statusPort(0),
    _packetsPerClientPerInterval(10),
    _packetsTotalPerInterval(DEFAULT_PACKETS_PER_INTERVAL),
    _tree(nullptr),
    _wantPersist(true),
    _debugSending(false),
    _debugReceiving(false),
    _verboseDebug(false),
    _octreeInboundPacketProcessor(nullptr),
    _persistManager(nullptr),
    _started(time(0)),
    _startedUSecs(usecTimestampNow())
{
    _averageLoopTime.updateAverage(0);
    qDebug() << "Octree server starting... [" << this << "]";
}

OctreeServer::~OctreeServer() {
    qDebug() << qPrintable(_safeServerName) << "server shutting down... [" << this << "]";
    if (_parsedArgV) {
        for (int i = 0; i < _argc; i++) {
            delete[] _parsedArgV[i];
        }
        delete[] _parsedArgV;
    }

    if (_octreeInboundPacketProcessor) {
        _octreeInboundPacketProcessor->terminating();
        _octreeInboundPacketProcessor->terminate();
        _octreeInboundPacketProcessor->deleteLater();
    }

    qDebug() << "Waiting for persist thread to come down";
    _persistThread.wait();

    // cleanup our tree here...
    qDebug() << qPrintable(_safeServerName) << "server START cleaning up octree... [" << this << "]";
    _tree.reset();
    qDebug() << qPrintable(_safeServerName) << "server DONE cleaning up octree... [" << this << "]";

    qDebug() << qPrintable(_safeServerName) << "server DONE shutting down... [" << this << "]";
}


void OctreeServer::setArguments(int argc, char** argv) {
    _argc = argc;
    _argv = const_cast<const char**>(argv);

    qDebug("OctreeServer::setArguments()");
    for (int i = 0; i < _argc; i++) {
        qDebug("_argv[%d]=%s", i, _argv[i]);
    }

}

void OctreeServer::parsePayload() {

    if (getPayload().size() > 0) {
        QString config(_payload);

        // Now, parse the config
        QStringList configList = config.split(" ");

        int argCount = configList.size() + 1;

        qDebug("OctreeServer::parsePayload()... argCount=%d",argCount);

        _parsedArgV = new char*[argCount];
        const char* dummy = "config-from-payload";
        _parsedArgV[0] = new char[strlen(dummy) + sizeof(char)];
        strcpy(_parsedArgV[0], dummy);

        for (int i = 1; i < argCount; i++) {
            QString configItem = configList.at(i-1);
            _parsedArgV[i] = new char[configItem.length() + sizeof(char)];
            strcpy(_parsedArgV[i], configItem.toLocal8Bit().constData());
            qDebug("OctreeServer::parsePayload()... _parsedArgV[%d]=%s", i, _parsedArgV[i]);
        }

        setArguments(argCount, _parsedArgV);
    }
}

OctreeServer::UniqueSendThread OctreeServer::createSendThread(const SharedNodePointer& node) {
    auto sendThread = newSendThread(node);

    // we want to be notified when the thread finishes
    connect(sendThread.get(), &GenericThread::finished, this, &OctreeServer::removeSendThread);
    sendThread->initialize(true);

    return sendThread;
}

void OctreeServer::removeSendThread() {
    // If the object has been deleted since the event was queued, sender() will return nullptr
    if (auto sendThread = qobject_cast<OctreeSendThread*>(sender())) {
        // This deletes the unique_ptr, so sendThread is destructed after that line
        _sendThreads.erase(sendThread->getNodeUuid());
    }
}

void OctreeServer::handleOctreeQueryPacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    if (!_isFinished && !_isShuttingDown) {
        // If we got a query packet, then we're talking to an agent, and we
        // need to make sure we have it in our nodeList.
        auto nodeList = DependencyManager::get<NodeList>();
        nodeList->updateNodeWithDataFromPacket(message, senderNode);

        auto it = _sendThreads.find(senderNode->getUUID());
        if (it == _sendThreads.end()) {
            _sendThreads.emplace(senderNode->getUUID(), createSendThread(senderNode));
        } else if (it->second->isShuttingDown()) {
            _sendThreads.erase(it); // Remove right away and wait on thread to be

            _sendThreads.emplace(senderNode->getUUID(), createSendThread(senderNode));
        }
    }
}

void OctreeServer::handleOctreeDataNackPacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    // If we got a nack packet, then we're talking to an agent, and we
    // need to make sure we have it in our nodeList.
    OctreeQueryNode* nodeData = dynamic_cast<OctreeQueryNode*>(senderNode->getLinkedData());
    if (nodeData) {
        nodeData->parseNackPacket(*message);
    }
}

bool OctreeServer::readOptionBool(const QString& optionName, const QJsonObject& settingsSectionObject, bool& result) {
    result = false; // assume it doesn't exist
    bool optionAvailable = false;
    QString argName = "--" + optionName;
    bool argExists = cmdOptionExists(_argc, _argv, qPrintable(argName));
    if (argExists) {
        optionAvailable = true;
        result = argExists;
        qDebug() << "From payload arguments: " << qPrintable(argName) << ":" << result;
    } else if (settingsSectionObject.contains(optionName)) {
        optionAvailable = true;
        result = settingsSectionObject[optionName].toBool();
        qDebug() << "From domain settings: " << qPrintable(optionName) << ":" << result;
    }
    return optionAvailable;
}

bool OctreeServer::readOptionInt(const QString& optionName, const QJsonObject& settingsSectionObject, int& result) {
    bool optionAvailable = false;
    QString argName = "--" + optionName;
    const char* argValue = getCmdOption(_argc, _argv, qPrintable(argName));
    if (argValue) {
        optionAvailable = true;
        result = atoi(argValue);
        qDebug() << "From payload arguments: " << qPrintable(argName) << ":" << result;
    } else if (settingsSectionObject.contains(optionName)) {
        optionAvailable = true;
        result = settingsSectionObject[optionName].toString().toInt(&optionAvailable);
        if (optionAvailable) {
            qDebug() << "From domain settings: " << qPrintable(optionName) << ":" << result;
        }
    }
    return optionAvailable;
}

bool OctreeServer::readOptionInt64(const QString& optionName, const QJsonObject& settingsSectionObject, qint64& result) {
    bool optionAvailable = false;
    QString argName = "--" + optionName;
    const char* argValue = getCmdOption(_argc, _argv, qPrintable(argName));
    if (argValue) {
        optionAvailable = true;
        result = atoll(argValue);
        qDebug() << "From payload arguments: " << qPrintable(argName) << ":" << result;
    } else if (settingsSectionObject.contains(optionName)) {
        optionAvailable = true;
        result = settingsSectionObject[optionName].toString().toLongLong(&optionAvailable);
        if (optionAvailable) {
            qDebug() << "From domain settings: " << qPrintable(optionName) << ":" << result;
        }
    }
    return optionAvailable;
}

bool OctreeServer::readOptionString(const QString& optionName, const QJsonObject& settingsSectionObject, QString& result) {
    bool optionAvailable = false;
    QString argName = "--" + optionName;
    const char* argValue = getCmdOption(_argc, _argv, qPrintable(argName));
    if (argValue) {
        optionAvailable = true;
        result = QString(argValue);
        qDebug() << "From payload arguments: " << qPrintable(argName) << ":" << qPrintable(result);
    } else if (settingsSectionObject.contains(optionName)) {
        optionAvailable = true;
        result = settingsSectionObject[optionName].toString();
        qDebug() << "From domain settings: " << qPrintable(optionName) << ":" << qPrintable(result);
    }
    return optionAvailable;
}

void OctreeServer::readConfiguration() {
    // if the assignment had a payload, read and parse that
    if (getPayload().size() > 0) {
        parsePayload();
    }

    const QJsonObject& settingsObject = DependencyManager::get<NodeList>()->getDomainHandler().getSettingsObject();

    commonParseSettingsObject(settingsObject);

    QString settingsKey = getMyDomainSettingsKey();
    QJsonObject settingsSectionObject = settingsObject[settingsKey].toObject();
    _settings = settingsSectionObject; // keep this for later

    if (!readOptionString(QString("statusHost"), settingsSectionObject, _statusHost) || _statusHost.isEmpty()) {
        _statusHost = getGuessedLocalAddress().toString();
    }
    qDebug("statusHost=%s", qPrintable(_statusHost));

    if (readOptionInt(QString("statusPort"), settingsSectionObject, _statusPort)) {
        qDebug() << "statusPort=" << _statusPort;
    } else {
        qDebug() << "statusPort= DISABLED";
    }

    readOptionBool(QString("verboseDebug"), settingsSectionObject, _verboseDebug);
    qDebug("verboseDebug=%s", debug::valueOf(_verboseDebug));

    readOptionBool(QString("debugSending"), settingsSectionObject, _debugSending);
    qDebug("debugSending=%s", debug::valueOf(_debugSending));

    readOptionBool(QString("debugReceiving"), settingsSectionObject, _debugReceiving);
    qDebug("debugReceiving=%s", debug::valueOf(_debugReceiving));

    readOptionBool(QString("debugTimestampNow"), settingsSectionObject, _debugTimestampNow);
    qDebug() << "debugTimestampNow=" << _debugTimestampNow;

    bool noPersist;
    readOptionBool(QString("NoPersist"), settingsSectionObject, noPersist);
    _wantPersist = !noPersist;
    qDebug() << "wantPersist=" << _wantPersist;

    if (_wantPersist) {
        if (!readOptionString("persistFilePath", settingsSectionObject, _persistFilePath)
            && !readOptionString("persistFilename", settingsSectionObject, _persistFilePath)) {
            _persistFilePath = getMyDefaultPersistFilename();
        }

        QDir persistPath { _persistFilePath };

        if (persistPath.isRelative()) {
            // if the domain settings passed us a relative path, make an absolute path that is relative to the
            // default data directory
            _persistAbsoluteFilePath = QDir(PathUtils::getAppDataFilePath("entities/")).absoluteFilePath(_persistFilePath);
        } else {
            _persistAbsoluteFilePath = persistPath.absolutePath();
        }

        qDebug() << "persistFilePath=" << _persistFilePath;
        qDebug() << "persisAbsoluteFilePath=" << _persistAbsoluteFilePath;

        _persistAsFileType = "json.gz";

        _persistInterval = OctreePersistThread::DEFAULT_PERSIST_INTERVAL;
        int result { -1 };
        readOptionInt(QString("persistInterval"), settingsSectionObject, result);
        if (result != -1) {
            _persistInterval = std::chrono::milliseconds(result);
        }

        qDebug() << "persistInterval=" << _persistInterval.count();

        readOptionBool(QString("persistFileDownload"), settingsSectionObject, _persistFileDownload);
        qDebug() << "persistFileDownload=" << _persistFileDownload;

    } else {
        qDebug("persistFilename= DISABLED");
    }

    // Debug option to demonstrate that the server's local time does not
    // need to be in sync with any other network node. This forces clock
    // skew for the individual server node
    qint64 clockSkew;
    if (readOptionInt64(QString("clockSkew"), settingsSectionObject, clockSkew)) {
        usecTimestampNowForceClockSkew(clockSkew);
        qDebug() << "clockSkew=" << clockSkew;
    }

    // Check to see if the user passed in a command line option for setting packet send rate
    int packetsPerSecondPerClientMax = -1;
    if (readOptionInt(QString("packetsPerSecondPerClientMax"), settingsSectionObject, packetsPerSecondPerClientMax)) {
        _packetsPerClientPerInterval = packetsPerSecondPerClientMax / INTERVALS_PER_SECOND;
        if (_packetsPerClientPerInterval < 1) {
            _packetsPerClientPerInterval = 1;
        }
    }
    qDebug("packetsPerSecondPerClientMax=%d _packetsPerClientPerInterval=%d",
                    packetsPerSecondPerClientMax, _packetsPerClientPerInterval);

    // Check to see if the user passed in a command line option for setting packet send rate
    int packetsPerSecondTotalMax = -1;
    if (readOptionInt(QString("packetsPerSecondTotalMax"), settingsSectionObject, packetsPerSecondTotalMax)) {
        _packetsTotalPerInterval = packetsPerSecondTotalMax / INTERVALS_PER_SECOND;
        if (_packetsTotalPerInterval < 1) {
            _packetsTotalPerInterval = 1;
        }
    }
    qDebug("packetsPerSecondTotalMax=%d _packetsTotalPerInterval=%d",
                    packetsPerSecondTotalMax, _packetsTotalPerInterval);


    readAdditionalConfiguration(settingsSectionObject);
}

void OctreeServer::run() {
    _safeServerName = getMyServerName();

    // Before we do anything else, create our tree...
    OctreeElement::resetPopulationStatistics();
    _tree = createTree();
    _tree->setIsServer(true);

    qDebug() << "Waiting for connection to domain to request settings from domain-server.";

    // wait until we have the domain-server settings, otherwise we bail
    DomainHandler& domainHandler = DependencyManager::get<NodeList>()->getDomainHandler();
    connect(&domainHandler, &DomainHandler::settingsReceived, this, &OctreeServer::domainSettingsRequestComplete);
    connect(&domainHandler, &DomainHandler::settingsReceiveFail, this, &OctreeServer::domainSettingsRequestFailed);

    // use common init to setup common timers and logging
    commonInit(getMyLoggingServerTargetName(), getMyNodeType());
}

void OctreeServer::domainSettingsRequestComplete() {
    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    packetReceiver.registerListener(PacketType::OctreeDataNack,
        PacketReceiver::makeSourcedListenerReference<OctreeServer>(this, &OctreeServer::handleOctreeDataNackPacket));
    packetReceiver.registerListener(getMyQueryMessageType(),
        PacketReceiver::makeSourcedListenerReference<OctreeServer>(this, &OctreeServer::handleOctreeQueryPacket));

    qCDebug(octree_server) << "Received domain settings";

    readConfiguration();

    // if we want Persistence, set up the local file and persist thread
    if (_wantPersist) {
        static const QString ENTITY_PERSIST_EXTENSION = ".json.gz";

        // force the persist file to end with .json.gz
        if (!_persistAbsoluteFilePath.endsWith(ENTITY_PERSIST_EXTENSION, Qt::CaseInsensitive)) {
            _persistAbsoluteFilePath += ENTITY_PERSIST_EXTENSION;
        } else {
            // make sure the casing of .json.gz is correct
            _persistAbsoluteFilePath.replace(ENTITY_PERSIST_EXTENSION, ENTITY_PERSIST_EXTENSION, Qt::CaseInsensitive);
        }

        if (!QFile::exists(_persistAbsoluteFilePath)) {
            qDebug() << "Persist file does not exist, checking for existence of persist file next to application";

            static const QString OLD_DEFAULT_PERSIST_FILENAME = "resources/models.json.gz";
            QString oldResourcesDirectory = QCoreApplication::applicationDirPath();

            // This is the old persist path, based on the current persist filename, which could
            // be a custom filename set by the user.
            auto oldPersistPath = QDir(oldResourcesDirectory).absoluteFilePath(_persistFilePath);

            // This is the old default persist path.
            auto oldDefaultPersistPath = QDir(oldResourcesDirectory).absoluteFilePath(OLD_DEFAULT_PERSIST_FILENAME);

            qDebug() << "Checking for existing persist file at " << oldPersistPath << " and " << oldDefaultPersistPath;

            QString pathToCopyFrom;
            bool shouldCopy = false;

            if (QFile::exists(oldPersistPath)) {
                shouldCopy = true;
                pathToCopyFrom = oldPersistPath;
            } else if (QFile::exists(oldDefaultPersistPath)) {
                shouldCopy = true;
                pathToCopyFrom = oldDefaultPersistPath;
            }

            QDir persistFileDirectory { QDir::cleanPath(_persistAbsoluteFilePath + "/..") };

            if (!persistFileDirectory.exists()) {
                qDebug() << "Creating data directory " << persistFileDirectory.absolutePath();
                persistFileDirectory.mkpath(".");
            }

            if (shouldCopy) {
                qDebug() << "Old persist file found, copying from " << pathToCopyFrom << " to " << _persistAbsoluteFilePath;

                QFile::copy(pathToCopyFrom, _persistAbsoluteFilePath);
            } else {
                qDebug() << "No existing persist file found";
            }
        }

        auto persistFileDirectory = QFileInfo(_persistAbsoluteFilePath).absolutePath();

        // now set up PersistThread
        _persistManager = new OctreePersistThread(_tree, _persistAbsoluteFilePath, _persistInterval, _debugTimestampNow,
                                                 _persistAsFileType);
        _persistManager->moveToThread(&_persistThread);
        connect(&_persistThread, &QThread::finished, _persistManager, &QObject::deleteLater);
        connect(&_persistThread, &QThread::started, _persistManager, [this] {
            setThreadName("OctreePersistThread");
            _persistManager->start();
        });
        connect(_persistManager, &OctreePersistThread::loadCompleted, this, [this]() {
            beginRunning();
        });
        _persistThread.start();
    } else {
        beginRunning();
    }
}

static FILE* crashDbgFile = nullptr;

static void crashDbg(const char* msg) {
    if (!crashDbgFile) {
        crashDbgFile = fopen("crashdbg.log", "a");
    }
    if (crashDbgFile) {
        fprintf(crashDbgFile, "[CRASH-DBG] %s\n", msg);
        fflush(crashDbgFile);
    }
}

void OctreeServer::beginRunning() {
    crashDbg("beginRunning: start");
    auto nodeList = DependencyManager::get<NodeList>();
    crashDbg("beginRunning: got nodeList");

    // we need to ask the DS about agents so we can ping/reply with them
    nodeList->addSetOfNodeTypesToNodeInterestSet({ NodeType::Agent, NodeType::EntityScriptServer,
        NodeType::AvatarMixer, NodeType::AudioMixer });
    crashDbg("beginRunning: added interest set");

    beforeRun(); // after payload has been processed
    crashDbg("beginRunning: beforeRun done");

    connect(nodeList.data(), &NodeList::nodeAdded, this, &OctreeServer::nodeAdded);
    connect(nodeList.data(), &NodeList::nodeKilled, this, &OctreeServer::nodeKilled);

    nodeList->linkedDataCreateCallback = [this](Node* node) {
        auto queryNodeData = createOctreeQueryNode();
        queryNodeData->init();
        node->setLinkedData(std::move(queryNodeData));
    };

    srand((unsigned)time(0));

    // set up our OctreeServerPacketProcessor
    _octreeInboundPacketProcessor = new OctreeInboundPacketProcessor(this);
    crashDbg("beginRunning: OctreeInboundPacketProcessor created");
    _octreeInboundPacketProcessor->initialize(true);
    crashDbg("beginRunning: OctreeInboundPacketProcessor initialized");

    // Convert now to tm struct for local timezone
    tm* localtm = localtime(&_started);
    const int MAX_TIME_LENGTH = 128;
    char localBuffer[MAX_TIME_LENGTH] = { 0 };
    char utcBuffer[MAX_TIME_LENGTH] = { 0 };
    strftime(localBuffer, MAX_TIME_LENGTH, "%m/%d/%Y %X", localtm);
    // Convert now to tm struct for UTC
    tm* gmtm = gmtime(&_started);
    if (gmtm) {
        strftime(utcBuffer, MAX_TIME_LENGTH, " [%m/%d/%Y %X UTC]", gmtm);
    }

    qDebug() << "Now running... started at: " << localBuffer << utcBuffer;
    crashDbg("beginRunning: end");
}

void OctreeServer::nodeAdded(SharedNodePointer node) {
    // we might choose to use this notifier to track clients in a pending state
    qDebug() << qPrintable(_safeServerName) << "server added node:" << *node;
}

void OctreeServer::nodeKilled(SharedNodePointer node) {
    quint64 start  = usecTimestampNow();

    // Shutdown send thread
    auto it = _sendThreads.find(node->getUUID());
    if (it != _sendThreads.end()) {
        auto& sendThread = *it->second;
        sendThread.setIsShuttingDown();
    }

    // calling this here since nodeKilled slot in ReceivedPacketProcessor can't be triggered by signals yet!!
    _octreeInboundPacketProcessor->nodeKilled(node);

    qDebug() << qPrintable(_safeServerName) << "server killed node:" << *node;
    OctreeQueryNode* nodeData = dynamic_cast<OctreeQueryNode*>(node->getLinkedData());
    if (nodeData) {
        nodeData->nodeKilled(); // tell our node data and sending threads that we'd like to shut down
    } else {
        qDebug() << qPrintable(_safeServerName) << "server node missing linked data node:" << *node;
    }

    quint64 end  = usecTimestampNow();
    quint64 usecsElapsed = (end - start);
    if (usecsElapsed > 1000) {
        qDebug() << qPrintable(_safeServerName) << "server nodeKilled() took: " << usecsElapsed << " usecs for node:" << *node;
    }

    trackViewerGone(node->getUUID());
}

void OctreeServer::aboutToFinish() {
    qDebug() << qPrintable(_safeServerName) << "server STARTING about to finish...";

    _isShuttingDown = true;

    qDebug() << qPrintable(_safeServerName) << "inform Octree Inbound Packet Processor that we are shutting down...";

    // we're going down - set the NodeList linkedDataCallback to nullptr so we do not create any more OctreeQueryNode objects.
    // This ensures that we don't get any more newly connecting nodes
    DependencyManager::get<NodeList>()->linkedDataCreateCallback = nullptr;

    if (_octreeInboundPacketProcessor) {
        _octreeInboundPacketProcessor->terminating();
    }

    // Shut down all the send threads
    for (auto& it : _sendThreads) {
        auto& sendThread = *it.second;
        sendThread.setIsShuttingDown();
        sendThread.terminate();
    }

    // Clear will destruct all the unique_ptr to OctreeSendThreads which will call the GenericThread's dtor
    // which waits on the thread to be done before returning
    _sendThreads.clear(); // Cleans up all the send threads.

    if (_persistManager) {
        _persistThread.quit();
    }

    qDebug() << qPrintable(_safeServerName) << "server ENDING about to finish...";
}

QString OctreeServer::getUptime() {
    QString formattedUptime;
    quint64 now  = usecTimestampNow();
    const int USECS_PER_MSEC = 1000;
    quint64 msecsElapsed = (now - _startedUSecs) / USECS_PER_MSEC;
    const int MSECS_PER_SEC = 1000;
    const int SECS_PER_MIN = 60;
    const int MIN_PER_HOUR = 60;
    const int MSECS_PER_MIN = MSECS_PER_SEC * SECS_PER_MIN;

    float seconds = (msecsElapsed % MSECS_PER_MIN)/(float)MSECS_PER_SEC;
    int minutes = (msecsElapsed/(MSECS_PER_MIN)) % MIN_PER_HOUR;
    int hours = (msecsElapsed/(MSECS_PER_MIN * MIN_PER_HOUR));

    if (hours > 0) {
        formattedUptime += QString("%1 hour").arg(hours);
        if (hours > 1) {
            formattedUptime += QString("s");
        }
    }
    if (minutes > 0) {
        if (hours > 0) {
            formattedUptime += QString(" ");
        }
        formattedUptime += QString("%1 minute").arg(minutes);
        if (minutes > 1) {
            formattedUptime += QString("s");
        }
    }
    if (seconds > 0) {
        if (hours > 0 || minutes > 0) {
            formattedUptime += QString(" ");
        }
        formattedUptime += QString("%1 seconds").arg((double)seconds, 0, 'f', 3);
    }
    return formattedUptime;
}

double OctreeServer::getUptimeSeconds() {
    return (usecTimestampNow() - _startedUSecs) / NANOSECONDS_PER_SECOND;
}

QString OctreeServer::getFileLoadTime() {
    QString result;
    if (isInitialLoadComplete()) {

        const int USECS_PER_MSEC = 1000;
        const int MSECS_PER_SEC = 1000;
        const int SECS_PER_MIN = 60;
        const int MIN_PER_HOUR = 60;
        const int MSECS_PER_MIN = MSECS_PER_SEC * SECS_PER_MIN;

        quint64 msecsElapsed = getLoadElapsedTime() / USECS_PER_MSEC;;
        float seconds = (msecsElapsed % MSECS_PER_MIN)/(float)MSECS_PER_SEC;
        int minutes = (msecsElapsed/(MSECS_PER_MIN)) % MIN_PER_HOUR;
        int hours = (msecsElapsed/(MSECS_PER_MIN * MIN_PER_HOUR));

        if (hours > 0) {
            result += QString("%1 hour").arg(hours);
            if (hours > 1) {
                result += QString("s");
            }
        }
        if (minutes > 0) {
            if (hours > 0) {
                result += QString(" ");
            }
            result += QString("%1 minute").arg(minutes);
            if (minutes > 1) {
                result += QString("s");
            }
        }
        if (seconds >= 0) {
            if (hours > 0 || minutes > 0) {
                result += QString(" ");
            }
            result += QString("%1 seconds").arg((double)seconds, 0, 'f', 3);
        }
    } else {
        result = "Not yet loaded...";
    }
    return result;
}

double OctreeServer::getFileLoadTimeSeconds() {
    return getLoadElapsedTime() / NANOSECONDS_PER_SECOND;
}

QString OctreeServer::getConfiguration() {
    QString result;
    for (int i = 1; i < _argc; i++) {
        result += _argv[i] + QString(" ");
    }
    return result;
}

QString OctreeServer::getStatusLink() {
    QString result;
    if (_statusPort > 0) {
        QString detailedStats= QString("http://") +  _statusHost + QString(":%1").arg(_statusPort);
        result = "<a href='" + detailedStats + "'>"+detailedStats+"</a>";
    } else {
        result = "Status port not enabled.";
    }
    return result;
}

void OctreeServer::sendStatsPacket() {
    // Stats Array 1
    QJsonObject threadsStats;
    quint64 oneSecondAgo = usecTimestampNow() - USECS_PER_SECOND;
    threadsStats["1. processing"] = (double)howManyThreadsDidProcess(oneSecondAgo);
    threadsStats["2. packetDistributor"] = (double)howManyThreadsDidPacketDistributor(oneSecondAgo);
    threadsStats["3. handlePacektSend"] = (double)howManyThreadsDidHandlePacketSend(oneSecondAgo);
    threadsStats["4. writeDatagram"] = (double)howManyThreadsDidCallWriteDatagram(oneSecondAgo);

    QJsonObject statsArray1;
    statsArray1["1. configuration"] = getConfiguration();
    statsArray1["2. detailed_stats_url"] = getStatusLink();
    statsArray1["3. uptime"] = getUptime();
    statsArray1["4. persistFileLoadTime"] = getFileLoadTime();
    statsArray1["5. clients"] = getCurrentClientCount();
    statsArray1["6. threads"] = threadsStats;
    statsArray1["uptime_seconds"] = getUptimeSeconds();
    statsArray1["persistFileLoadTime_seconds"] = getFileLoadTimeSeconds();

    // Octree Stats
    QJsonObject octreeStats;
    octreeStats["1. elementCount"] = (double)OctreeElement::getNodeCount();
    octreeStats["2. internalElementCount"] = (double)OctreeElement::getInternalNodeCount();
    octreeStats["3. leafElementCount"] = (double)OctreeElement::getLeafNodeCount();

    // Stats Object 2
    QJsonObject dataObject1;
    dataObject1["1. totalPackets"] = (double)OctreeSendThread::_totalPackets;
    dataObject1["2. totalBytes"] = (double)OctreeSendThread::_totalBytes;
    dataObject1["3. totalBytesWasted"] = (double)OctreeSendThread::_totalWastedBytes;
    dataObject1["4. totalBytesOctalCodes"] = (double)OctreePacketData::getTotalBytesOfOctalCodes();
    dataObject1["5. totalBytesBitMasks"] = (double)OctreePacketData::getTotalBytesOfBitMasks();
    dataObject1["6. totalBytesBitMasks"] = (double)OctreePacketData::getTotalBytesOfColor();

    QJsonObject timingArray1;
    timingArray1["1. avgLoopTime"] = getAverageLoopTime();
    timingArray1["2. avgInsideTime"] = getAverageInsideTime();
    timingArray1["3. avgTreeTraverseTime"] = getAverageTreeTraverseTime();
    timingArray1["4. avgEncodeTime"] = getAverageEncodeTime();
    timingArray1["5. avgCompressAndWriteTime"] = getAverageCompressAndWriteTime();
    timingArray1["6. avgSendTime"] = getAveragePacketSendingTime();
    timingArray1["7. nodeWaitTime"] = getAverageNodeWaitTime();

    QJsonObject statsObject2;
    statsObject2["data"] = dataObject1;
    statsObject2["timing"] = timingArray1;

    QJsonObject dataArray2;
    QJsonObject timingArray2;

    // Stats Object 3
    if (_octreeInboundPacketProcessor) {
        dataArray2["1. packetQueue"] = (double)_octreeInboundPacketProcessor->packetsToProcessCount();
        dataArray2["2. totalPackets"] = (double)_octreeInboundPacketProcessor->getTotalPacketsProcessed();
        dataArray2["3. totalElements"] = (double)_octreeInboundPacketProcessor->getTotalElementsProcessed();

        timingArray2["1. avgTransitTimePerPacket"] = (double)_octreeInboundPacketProcessor->getAverageTransitTimePerPacket();
        timingArray2["2. avgProcessTimePerPacket"] = (double)_octreeInboundPacketProcessor->getAverageProcessTimePerPacket();
        timingArray2["3. avgLockWaitTimePerPacket"] = (double)_octreeInboundPacketProcessor->getAverageLockWaitTimePerPacket();
        timingArray2["4. avgProcessTimePerElement"] = (double)_octreeInboundPacketProcessor->getAverageProcessTimePerElement();
        timingArray2["5. avgLockWaitTimePerElement"] = (double)_octreeInboundPacketProcessor->getAverageLockWaitTimePerElement();
    }

    QJsonObject statsObject3;
    statsObject3["data"] = dataArray2;
    statsObject3["timing"] = timingArray2;

    // Merge everything
    QJsonObject jsonArray;
    jsonArray["1. misc"] = statsArray1;
    jsonArray["2. octree"] = octreeStats;
    jsonArray["3. outbound"] = statsObject2;
    jsonArray["4. inbound"] = statsObject3;

    QJsonObject statsObject;
    statsObject[QString(getMyServerName()) + "Server"] = jsonArray;
    addPacketStatsAndSendStatsPacket(statsObject);
}

QMap<OctreeSendThread*, quint64> OctreeServer::_threadsDidProcess;
QMap<OctreeSendThread*, quint64> OctreeServer::_threadsDidPacketDistributor;
QMap<OctreeSendThread*, quint64> OctreeServer::_threadsDidHandlePacketSend;
QMap<OctreeSendThread*, quint64> OctreeServer::_threadsDidCallWriteDatagram;

QMutex OctreeServer::_threadsDidProcessMutex;
QMutex OctreeServer::_threadsDidPacketDistributorMutex;
QMutex OctreeServer::_threadsDidHandlePacketSendMutex;
QMutex OctreeServer::_threadsDidCallWriteDatagramMutex;


void OctreeServer::didProcess(OctreeSendThread* thread) {
    QMutexLocker locker(&_threadsDidProcessMutex);
    _threadsDidProcess[thread] = usecTimestampNow();
}

void OctreeServer::didPacketDistributor(OctreeSendThread* thread) {
    QMutexLocker locker(&_threadsDidPacketDistributorMutex);
    _threadsDidPacketDistributor[thread] = usecTimestampNow();
}

void OctreeServer::didHandlePacketSend(OctreeSendThread* thread) {
    QMutexLocker locker(&_threadsDidHandlePacketSendMutex);
    _threadsDidHandlePacketSend[thread] = usecTimestampNow();
}

void OctreeServer::didCallWriteDatagram(OctreeSendThread* thread) {
    QMutexLocker locker(&_threadsDidCallWriteDatagramMutex);
    _threadsDidCallWriteDatagram[thread] = usecTimestampNow();
}


void OctreeServer::stopTrackingThread(OctreeSendThread* thread) {
    {
        QMutexLocker locker(&_threadsDidProcessMutex);
        _threadsDidProcess.remove(thread);
    }
    {
        QMutexLocker locker(&_threadsDidPacketDistributorMutex);
        _threadsDidPacketDistributor.remove(thread);
    }
    {
        QMutexLocker locker(&_threadsDidHandlePacketSendMutex);
        _threadsDidHandlePacketSend.remove(thread);
    }
    {
        QMutexLocker locker(&_threadsDidCallWriteDatagramMutex);
        _threadsDidCallWriteDatagram.remove(thread);
    }
}

int howManyThreadsDidSomething(QMutex& mutex, QMap<OctreeSendThread*, quint64>& something, quint64 since) {
    int count = 0;
    if (mutex.tryLock()) {
        if (since == 0) {
            count = something.size();
        } else {
            QMap<OctreeSendThread*, quint64>::const_iterator i = something.constBegin();
            while (i != something.constEnd()) {
                if (i.value() > since) {
                    count++;
                }
                ++i;
            }
        }
        mutex.unlock();
    }
    return count;
}


int OctreeServer::howManyThreadsDidProcess(quint64 since) {
    return howManyThreadsDidSomething(_threadsDidProcessMutex, _threadsDidProcess, since);
}

int OctreeServer::howManyThreadsDidPacketDistributor(quint64 since) {
    return howManyThreadsDidSomething(_threadsDidPacketDistributorMutex, _threadsDidPacketDistributor, since);
}

int OctreeServer::howManyThreadsDidHandlePacketSend(quint64 since) {
    return howManyThreadsDidSomething(_threadsDidHandlePacketSendMutex, _threadsDidHandlePacketSend, since);
}

int OctreeServer::howManyThreadsDidCallWriteDatagram(quint64 since) {
    return howManyThreadsDidSomething(_threadsDidCallWriteDatagramMutex, _threadsDidCallWriteDatagram, since);
}

