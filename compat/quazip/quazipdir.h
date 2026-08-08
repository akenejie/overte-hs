#pragma once
// Stub for headless build - quazip not available
#include "quazip.h"
#include <QDir>
#include <QStringList>

class QuaZipDir {
public:
    QuaZipDir(QuaZip* zip = nullptr) { (void)zip; }
    explicit QuaZipDir(QuaZip& zip) { (void)zip; }
    QuaZipDir(QuaZip* zip, const QString& dir) { (void)zip; (void)dir; }
    virtual ~QuaZipDir() {}
    bool exists() const { return true; }
    bool exists(const QString&) const { return true; }
    bool cd(const QString&) { return true; }
    bool cdUp() { return true; }
    QString current() const { return QString(); }
    QStringList entryList(QDir::Filters filters = QDir::NoFilter) const { (void)filters; return {}; }
    QString filePath(const QString& fileName) const { return fileName; }
    QByteArray readFile(const QString&) { return QByteArray(); }
};
