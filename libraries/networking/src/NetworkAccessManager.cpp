//
//  NetworkAccessManager.cpp
//  libraries/networking/src
//
//  Created by Clement on 7/1/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "NetworkAccessManager.h"

#include <QThreadStorage>

#include "AtpReply.h"
#include <QtNetwork/QNetworkProxy>

// The per-thread QNetworkAccessManager must not be destroyed when its owning
// thread finishes. QThreadStorage<QNetworkAccessManager*> owns the stored
// object and deletes it on thread exit, which tears down any QSslSocket/QSslKey
// backing it. On OpenSSL 3 that happens concurrently with the process-wide
// OpenSSL cleanup and causes a nondeterministic double-free (SIGSEGV) during
// shutdown. Store a small box instead whose destructor intentionally does not
// destroy the manager, so the manager simply leaks until process teardown.
struct NetworkAccessManagerBox {
    QNetworkAccessManager* manager;
};

QThreadStorage<NetworkAccessManagerBox*> networkAccessManagers;

QNetworkAccessManager& NetworkAccessManager::getInstance() {
    if (!networkAccessManagers.hasLocalData()) {
        networkAccessManagers.setLocalData(new NetworkAccessManagerBox{ new QNetworkAccessManager() });
    }

    return *networkAccessManagers.localData()->manager;
}

QNetworkReply* NetworkAccessManager::createRequest(Operation operation, const QNetworkRequest& request, QIODevice* device) {
    if (request.url().scheme() == "atp" && operation == GetOperation) {
        return new AtpReply(request.url());
        //auto url = request.url().toString();
        //return QNetworkAccessManager::createRequest(operation, request, device);
    } else {
        return QNetworkAccessManager::createRequest(operation, request, device);
    }
}
