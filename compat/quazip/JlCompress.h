#pragma once
// Stub for headless build - quazip not available
#include <QString>
#include <QStringList>
class JlCompress {
public:
    static bool compressDir(const QString& fileCompressed, const QString& dir, bool includeRoot = true) { (void)fileCompressed; (void)dir; (void)includeRoot; return false; }
    static QStringList extractDir(const QString& fileCompressed, const QString& dir) { (void)fileCompressed; (void)dir; return {}; }
};
