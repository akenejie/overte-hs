//
//  ScriptEngine.cpp
//  libraries/script-engine/src
//
//  Created by Brad Hefta-Gaub on 12/14/13.
//  Copyright 2013 High Fidelity, Inc.
//  Copyright 2020 Vircadia contributors.
//  Copyright 2023 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//  SPDX-License-Identifier: Apache-2.0
//

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#include "ScriptEngine.h"

#include <QThread>

#include "ScriptEngineLogging.h"
#include "ScriptValue.h"
#include "qjs/ScriptEngineQJS.h"

ScriptEnginePointer newScriptEngine(ScriptManager* manager) {
    // QJS (QuickJS) is the script engine. It is self-contained and does not
    // depend on a specific build configuration or runtime environment.
    return newScriptEngineQJS(manager);
}

ScriptValue makeScopedHandlerObject(const ScriptValue& scopeOrCallback, const ScriptValue& methodOrName) {
    auto engine = scopeOrCallback.engine();
    if (!engine) {
        return scopeOrCallback;
    }
    ScriptValue scope;
    ScriptValue callback = scopeOrCallback;
    if (scopeOrCallback.isObject()) {
        if (methodOrName.isString()) {
            scope = scopeOrCallback;
            callback = scope.property(methodOrName.toString());
        } else if (methodOrName.isFunction()) {
            scope = scopeOrCallback;
            callback = methodOrName;
        } else if (!methodOrName.isValid()) {
            // instantiate from an existing scoped handler object
            if (scopeOrCallback.property("callback").isFunction()) {
                scope = scopeOrCallback.property("scope");
                callback = scopeOrCallback.property("callback");
            }
        }
    }
    auto handler = engine->newObject();
    handler.setProperty("scope", scope);
    handler.setProperty("callback", callback);
    return handler;
}

ScriptValue callScopedHandlerObject(const ScriptValue& handler, const ScriptValue& err, const ScriptValue& result) {
    return handler.property("callback").call(handler.property("scope"), ScriptValueList({ err, result }));
}

bool ScriptEngine::IS_THREADSAFE_INVOCATION(const QString& method) {
    QThread* thread = this->thread();
    if (QThread::currentThread() == thread) {
        return true;
    }
    qCCritical(scriptengine) << QString("Scripting::%1 @ %2 -- ignoring thread-unsafe call from %3")
                                    .arg(method)
                                    .arg(thread ? thread->objectName() : "(!thread)")
                                    .arg(QThread::currentThread()->objectName());
    qCDebug(scriptengine) << "(please resolve on the calling side by using invokeMethod, executeOnScriptThread, etc.)";
    Q_ASSERT(false);
    return false;
}
