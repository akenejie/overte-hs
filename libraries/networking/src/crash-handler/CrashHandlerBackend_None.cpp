//
//  CrashHandler_None.cpp
//  interface/src
//
//  Created by Clement Brisset on 01/19/18.
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#if !defined(HAS_CRASHPAD) && !defined(HAS_BREAKPAD)

#include "CrashHandler.h"

#include <assert.h>

#include <QDebug>


Q_LOGGING_CATEGORY(crash_handler, "overte.crash_handler")

bool startCrashHandler(std::string appPath, std::string crashURL, std::string crashToken) {
    qCDebug(crash_handler) << "No crash handler available.";
    return false;
}

void setCrashAnnotation(std::string name, std::string value) {
}

void startCrashHookMonitor(QCoreApplication* app) {
}

void setCrashReportingEnabled(bool value) {

}
#endif
