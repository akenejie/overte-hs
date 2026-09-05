//
//  main.cpp
//  assignment-client/src
//
//  Created by Stephen Birarda on 8/22/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#include <BuildInfo.h>
#include <SharedUtil.h>

#include "AssignmentClientApp.h"
#include <crash-handler/CrashHandler.h>


#ifdef OVERTE_MULTICALL_APPLET
int assignmentClientMain(int argc, char* argv[])
#else
int main(int argc, char* argv[])
#endif
{
    setupHifiApplication(BuildInfo::ASSIGNMENT_CLIENT_NAME);

    AssignmentClientApp app(argc, argv);
    auto &ch = CrashHandler::getInstance();
    ch.startMonitor(&app);


    int acReturn = app.exec();
    qDebug() << "assignment-client process" <<  app.applicationPid() << "exiting with status code" << acReturn;

    qInfo() << "Quitting.";
    return acReturn;
}
