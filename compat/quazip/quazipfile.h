#pragma once
// Stub for headless build - quazip not available
#include "quazip.h"
#include <QString>

class QuaZipNewInfo {
public:
    QuaZipNewInfo(const QString& name) : name(name) {}
    QuaZipNewInfo(const QString& name, const QString& file) : name(name) { (void)file; }
    QString name;
    QDateTime dateTime;
};

class QuaZipFile {
public:
    QuaZipFile(QuaZip* zip = nullptr) { (void)zip; }
    QuaZipFile(QuaZip& zip) { (void)zip; }
    virtual ~QuaZipFile() {}
    bool open(int mode, const QString& password = QString()) { (void)mode; (void)password; return true; }
    bool open(int mode, const QuaZipNewInfo& info) { (void)mode; (void)info; return true; }
    void close() {}
    qint64 write(const QByteArray& data) { (void)data; return 0; }
    QByteArray readAll() { return QByteArray(); }
    int getZipError() const { return UNZ_OK; }
};
