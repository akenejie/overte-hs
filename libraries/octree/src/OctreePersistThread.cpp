//
//  OctreePersistThread.cpp
//  libraries/octree/src
//
//  Created by Brad Hefta-Gaub on 8/21/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "OctreePersistThread.h"

#include <chrono>
#include <thread>

#include <cstdio>
#include <fstream>
#include <time.h>

#include <QDateTime>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QRegExp>

#include <NumericalConstants.h>
#include <PerfStat.h>
#include <PathUtils.h>
#include <Gzip.h>

#include "OctreeLogging.h"
#include "OctreeUtils.h"
#include "OctreeDataUtils.h"

constexpr std::chrono::seconds OctreePersistThread::DEFAULT_PERSIST_INTERVAL { 30 };
constexpr std::chrono::milliseconds TIME_BETWEEN_PROCESSING { 10 };

constexpr int MAX_OCTREE_REPLACEMENT_BACKUP_FILES_COUNT { 20 };
constexpr int64_t MAX_OCTREE_REPLACEMENT_BACKUP_FILES_SIZE_BYTES { 50 * 1000 * 1000 };
static const QString REPLACEMENT_FILE_EXTENSION = ".replace";

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

OctreePersistThread::OctreePersistThread(OctreePointer tree, const QString& filename, std::chrono::milliseconds persistInterval,
                                         bool debugTimestampNow, QString persistAsFileType) :
    _tree(tree),
    _filename(filename),
    _persistInterval(persistInterval),
    _lastPersistCheck(std::chrono::steady_clock::now()),
    _initialLoadComplete(false),
    _loadTimeUSecs(0),
    _debugTimestampNow(debugTimestampNow),
    _lastTimeDebug(0),
    _persistAsFileType(persistAsFileType)
{
    // in case the persist filename has an extension that doesn't match the file type
    QString sansExt = fileNameWithoutExtension(_filename, PERSIST_EXTENSIONS);
    _filename = sansExt + "." + _persistAsFileType;
}

void OctreePersistThread::start() {
    cleanupOldReplacementBackups();

    // Check for .replace file (placed by asset-server or external tool).
    QString replacementFilename = _filename + REPLACEMENT_FILE_EXTENSION;
    if (QFile::exists(replacementFilename)) {
        qCDebug(octree) << "Found replacement file" << replacementFilename << "- applying";

        QFile replacementFile(replacementFilename);
        if (replacementFile.open(QIODevice::ReadOnly)) {
            QByteArray replacementData = replacementFile.readAll();
            replacementFile.close();

            // Remove the replacement file first to avoid re-applying on next restart.
            if (replacementFile.remove()) {
                OctreeUtils::RawEntityData data;
                if (data.readOctreeDataInfoFromData(replacementData)) {
                    data.resetIdAndVersion();
                    auto gzippedData = data.toGzippedByteArray();
                    replaceData(gzippedData);
                    qCDebug(octree) << "Applied replacement file, reset ID and version";
                } else {
                    // Not a valid header, write raw replacement data.
                    replaceData(replacementData);
                    qCDebug(octree) << "Applied replacement file (no valid header)";
                }
            } else {
                qCWarning(octree) << "Failed to remove replacement file" << replacementFilename;
            }
        } else {
            qCWarning(octree) << "Failed to open replacement file" << replacementFilename;
        }
    }

    // Load tree from the local persist file.
    qCDebug(octree) << "Loading octree from" << _filename;

    OctreeUtils::RawOctreeData data;
    QByteArray cachedJSONData;
    QFile file(_filename);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray jsonData(file.readAll());
        file.close();
        if (!gunzip(jsonData, cachedJSONData)) {
            cachedJSONData = jsonData;
        }

        if (data.readOctreeDataInfoFromData(cachedJSONData)) {
            qCDebug(octree) << "Current octree data: ID(" << data.id << ") DataVersion(" << data.dataVersion << ")";
        } else {
            cachedJSONData.clear();
            qCWarning(octree) << "No octree data found in file";
        }
    } else {
        qCWarning(octree) << "Couldn't access file" << _filename << file.errorString();
    }

    quint64 loadStarted = usecTimestampNow();

    if (!data.id.isNull()) {
        qDebug() << "Setting entity version info to:" << data.id << data.dataVersion;
        _tree->setOctreeVersionInfo(data.id, data.dataVersion);
    }

    bool persistentFileRead;
    _tree->withWriteLock([&] {
        PerformanceWarning warn(true, "Loading Octree File", true);

        if (cachedJSONData.isEmpty()) {
            persistentFileRead = _tree->readFromFile(_filename.toLocal8Bit().constData());
        } else {
            QDataStream jsonStream(cachedJSONData);
            persistentFileRead = _tree->readFromStream(-1, jsonStream);
        }
        _tree->pruneTree();
    });

    quint64 loadDone = usecTimestampNow();
    _loadTimeUSecs = loadDone - loadStarted;

    _tree->clearDirtyBit();

    unsigned long nodeCount = OctreeElement::getNodeCount();
    unsigned long internalNodeCount = OctreeElement::getInternalNodeCount();
    unsigned long leafNodeCount = OctreeElement::getLeafNodeCount();
    qCDebug(octree, "Nodes after loading scene %lu nodes %lu internal %lu leaves", nodeCount, internalNodeCount, leafNodeCount);

    _initialLoadComplete = true;
    _lastPersistCheck = std::chrono::steady_clock::now();

    sendLatestEntityDataToDS();

    QTimer::singleShot(TIME_BETWEEN_PROCESSING.count(), this, &OctreePersistThread::process);

    emit loadCompleted();
}

QString OctreePersistThread::getPersistFileMimeType() const {
    if (_persistAsFileType == "json") {
        return "application/json";
    } if (_persistAsFileType == "json.gz") {
        return "application/zip";
    }
    return "";
}

void OctreePersistThread::replaceData(QByteArray data) {
    backupCurrentFile();

    QFile currentFile { _filename };
    if (currentFile.open(QIODevice::WriteOnly)) {
        currentFile.write(data);
        qDebug() << "Wrote replacement data";
    } else {
        qWarning() << "Failed to write replacement data";
    }
}

// Return true if current file is backed up successfully or doesn't exist.
bool OctreePersistThread::backupCurrentFile() {
    // first take the current models file and move it to a different filename, appended with the timestamp
    QFile currentFile { _filename };
    if (currentFile.exists()) {
        static const QString FILENAME_TIMESTAMP_FORMAT = "yyyyMMdd-hhmmss";
        auto backupFileName = _filename + ".backup." + QDateTime::currentDateTime().toString(FILENAME_TIMESTAMP_FORMAT);

        if (currentFile.rename(backupFileName)) {
            qDebug() << "Moved previous models file to" << backupFileName;
            return true;
        } else {
            qWarning() << "Could not backup previous models file to" << backupFileName << "- removing replacement models file";
            return false;
        }
    }
    return true;
}

void OctreePersistThread::process() {
    _tree->preUpdate();
    _tree->update();

    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastPersist = now - _lastPersistCheck;

    if (timeSinceLastPersist > _persistInterval) {
        _lastPersistCheck = now;
        persist();
    }

    QTimer::singleShot(TIME_BETWEEN_PROCESSING.count(), this, &OctreePersistThread::process);
}

void OctreePersistThread::aboutToFinish() {
    qCDebug(octree) << "Persist thread about to finish...";
    persist();
    qCDebug(octree) << "Persist thread done with about to finish...";
}

QByteArray OctreePersistThread::getPersistFileContents() const {
    QByteArray fileContents;
    QFile file(_filename);
    if (file.open(QIODevice::ReadOnly)) {
        fileContents = file.readAll();
    }
    return fileContents;
}

void OctreePersistThread::cleanupOldReplacementBackups() {
    QRegExp filenameRegex { ".*\\.backup\\.\\d{8}-\\d{6}$" };
    QFileInfo persistFile { _filename };
    QDir backupDir { persistFile.absolutePath() };
    backupDir.setSorting(QDir::SortFlag::Time);
    backupDir.setFilter(QDir::Filter::Files);
    qDebug() << "Scanning backups for cleanup:" << backupDir.absolutePath();

    int count = 0;
    int64_t totalSize = 0;
    for (auto fileInfo : backupDir.entryInfoList()) {
        auto absPath = fileInfo.absoluteFilePath();
        qDebug() << "  Found:" << absPath;
        if (filenameRegex.exactMatch(absPath)) {
            if (count >= MAX_OCTREE_REPLACEMENT_BACKUP_FILES_COUNT || totalSize > MAX_OCTREE_REPLACEMENT_BACKUP_FILES_SIZE_BYTES) {
                qDebug() << "  Removing:" << absPath;
                QFile backup(absPath);
                if (backup.remove()) {
                    qDebug() << "  Removed backup:" << absPath;
                } else {
                    qWarning() << "  Failed to remove backup:" << absPath;
                }
            }
            totalSize += fileInfo.size();
            count++;
        }
    }
    qDebug() << "Found" << count << "backups";
}

void OctreePersistThread::persist() {
    if (_tree->isDirty() && _initialLoadComplete) {

        _tree->withWriteLock([&] {
            qCDebug(octree) << "pruning Octree before saving...";
            _tree->pruneTree();
            qCDebug(octree) << "DONE pruning Octree before saving...";
        });

        _tree->incrementPersistDataVersion();

        qCDebug(octree) << "Saving Octree data to:" << _filename;
        if (_tree->writeToFile(_filename.toLocal8Bit().constData(), nullptr, _persistAsFileType)) {
            _tree->clearDirtyBit(); // tree is clean after saving
            qCDebug(octree) << "DONE persisting Octree data to" << _filename;
        } else {
            qCWarning(octree) << "Failed to persist Octree data to" << _filename;
        }

        sendLatestEntityDataToDS();
    }
}

void OctreePersistThread::sendLatestEntityDataToDS() {
    qDebug() << "Sending latest entity data to DS";
    crashDbg("sendLatestEntityDataToDS: start");
    auto nodeList = DependencyManager::get<NodeList>();
    crashDbg("sendLatestEntityDataToDS: got nodeList");
    const DomainHandler& domainHandler = nodeList->getDomainHandler();
    crashDbg(qPrintable(QString("sendLatestEntityDataToDS: got domainHandler, sockAddr=%1").arg(domainHandler.getSockAddr().toString())));

    QByteArray data;
    crashDbg("sendLatestEntityDataToDS: calling toJSON...");

    // Skip serialization when the tree has no entities (root is a leaf = empty tree).
    // This avoids running the QuickJS-based JSON serializer unnecessarily and prevents
    // a cross-thread STATUS_STACK_BUFFER_OVERRUN crash when the helper script engine
    // is called from a thread different from the one that created it.
    auto root = _tree->getRoot();
    if (root && root->isLeaf() && !root->hasContent()) {
        crashDbg("sendLatestEntityDataToDS: tree is empty, skipping serialization");
        qDebug() << "Entity tree is empty, skipping serialization to domain server";
        return;
    }

    if (_tree->toJSON(&data, nullptr, true)) {
        crashDbg(qPrintable(QString("sendLatestEntityDataToDS: toJSON done, data.size=%1").arg(data.size())));
        auto message = NLPacketList::create(PacketType::OctreeDataPersist, QByteArray(), true, true);
        crashDbg("sendLatestEntityDataToDS: NLPacketList created");
        message->write(data);
        crashDbg("sendLatestEntityDataToDS: data written, calling sendPacketList...");
        nodeList->sendPacketList(std::move(message), domainHandler.getSockAddr());
        crashDbg("sendLatestEntityDataToDS: sendPacketList done");
    } else {
        crashDbg("sendLatestEntityDataToDS: toJSON FAILED");
        qCWarning(octree) << "Failed to persist octree to DS";
    }
    crashDbg("sendLatestEntityDataToDS: end");
}
