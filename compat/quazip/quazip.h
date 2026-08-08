#pragma once
// Stub for headless build - quazip not available
#include <QString>
#include <QDateTime>

#define UNZ_OK 0
#define UNZ_END_OF_LIST_OF_FILE (-100)

class QuaZip {
public:
    enum Mode { mdNotOpen, mdUnzip, mdCreate, mdAppend, mdAdd };
    QuaZip(const QString& fileName = QString(), Mode mode = mdNotOpen) { (void)fileName; (void)mode; }
    QuaZip(class QFile* file, QObject* parent = nullptr) { (void)file; (void)parent; }
    QuaZip(class QBuffer* buffer, QObject* parent = nullptr) { (void)buffer; (void)parent; }
    virtual ~QuaZip() {}
    bool open(Mode mode) { (void)mode; return true; }
    void close() {}
    QString getFileName() const { return QString(); }
    int getFileCount() const { return 0; }
    bool setCurrentFile(const QString&) { return true; }
    bool hasCurrentFile() const { return true; }
    bool goToFirstFile() { return true; }
    bool goToNextFile() { return false; }
    bool getCurrentFileInfo(QString* name = nullptr, quint32* = nullptr, QDateTime* = nullptr, quint32* = nullptr, quint32* = nullptr, quint32* = nullptr, quint32* = nullptr, quint32* = nullptr) { if (name) *name = QString(); return true; }
    QByteArray readFile() { return QByteArray(); }
    bool getFileInfo(quint32*) const { return true; }
    int getZipError() const { return UNZ_OK; }
};
