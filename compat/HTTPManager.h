#pragma once
// Stub for headless build
#include <QObject>
#include <QString>
#include <QUrl>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QByteArray>
#include <QBuffer>
#include <QMap>
#include <QThread>
#include <QStringList>
#include <QPair>
#include <QHash>
#include <QNetworkAccessManager>

typedef QHash<QByteArray, QByteArray> Headers;
typedef QPair<Headers, QByteArray> FormData;

class HTTPConnection : public QObject {
    Q_OBJECT
public:
    enum StatusCode {
        StatusCode200 = 200,
        StatusCode400 = 400,
        StatusCode404 = 404,
        StatusCode500 = 500
    };

    HTTPConnection(QObject* p = nullptr) : QObject(p) {}
    virtual ~HTTPConnection() {}
    void sendResponse(const QByteArray& data, const QString& contentType = QString()) { (void)data; (void)contentType; }
    void sendRedirect(const QString& location) { (void)location; }
    bool respond(StatusCode code, const QByteArray& data = QByteArray(), const QString& contentType = QString(), const Headers& extraHeaders = {}) {
        (void)code; (void)data; (void)contentType; (void)extraHeaders; return true;
    }
    QUrl getUrl() const { return QUrl(); }
    QString getRequest() const { return QString(); }
    QString getHeader(const QString& name) const { (void)name; return QString(); }
    void parseContentType() {}

    // Additional stubs needed by DomainServerSettingsManager
    QByteArray requestContent() const { return QByteArray(); }
    QNetworkAccessManager::Operation requestOperation() const { return QNetworkAccessManager::GetOperation; }
    QList<FormData> parseFormData() const { return {}; }
};

// Not inheriting from QObject to avoid diamond inheritance with DomainMetadata
class HTTPRequestHandler {
public:
    HTTPRequestHandler() {}
    virtual ~HTTPRequestHandler() {}
    virtual bool handleHTTPRequest(HTTPConnection* connection, const QUrl& url, bool isSubRequest = false) {
        (void)connection; (void)url; (void)isSubRequest; return false;
    }
};

class HTTPManager : public QObject {
    Q_OBJECT
public:
    HTTPManager(int port, const QString& documentRoot, QObject* parent = nullptr) : QObject(parent) { (void)port; (void)documentRoot; }
    virtual ~HTTPManager() = default;
    bool start() { return true; }
    void stop() {}
signals:
    void settingsFileRequested(QJsonObject& settings);
    void settingsValueChanged(const QUrl& url, const QJsonObject& value);
    void settingsValueRemove(const QUrl& url, const QStringList& keys);
};
