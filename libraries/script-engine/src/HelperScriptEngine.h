//
//  HelperScriptEngine.h
//  libraries/script-engine/src/HelperScriptEngine.h
//
//  Created by dr Karol Suprynowicz on 2024/04/28.
//  Copyright 2024 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#ifndef overte_HelperScriptEngine_h
#define overte_HelperScriptEngine_h

#include <mutex>
#include "QThread"

#include "ScriptEngine.h"

/**
 * @brief Provides a wrapper around script engine that does not have ScriptManager
 *
 * HelperScriptEngine is used for performing smaller tasks, like for example conversions between entity
 * properties and JSON data.
 * For thread safety the script engine lives on a dedicated thread. All accesses through
 * HelperScriptEngine::run() or HelperScriptEngine::runWithResult() dispatch to that thread
 * when called from a different thread, ensuring the QuickJS engine is never used cross-thread.
 *
 */


class HelperScriptEngine {
public:
    HelperScriptEngine();
    ~HelperScriptEngine();

    template <typename F>
    inline void run(F&& f) {
        std::lock_guard<std::mutex> guard(_scriptEngineLock);
        if (!_scriptEngine) {
            return;
        }
        if (QThread::currentThread() == _scriptEngine->thread()) {
            f();
        } else {
            QMetaObject::invokeMethod(_scriptEngine.get(), [&f]() {
                f();
            }, Qt::BlockingQueuedConnection);
        }
    }

    template <typename T, typename F>
    inline T runWithResult(F&& f) {
        T result;
        {
            std::lock_guard<std::mutex> guard(_scriptEngineLock);
            if (!_scriptEngine) {
                return result;
            }
            if (QThread::currentThread() == _scriptEngine->thread()) {
                result = f();
            } else {
                QMetaObject::invokeMethod(_scriptEngine.get(), [&result, &f]() {
                    result = f();
                }, Qt::BlockingQueuedConnection);
            }
        }
        return result;
    }

    /**
     * @brief Returns pointer to the script engine
     *
     * This function should be used only inside HelperScriptEngine::run() or HelperScriptEngine::runWithResult()
     */
    ScriptEngine* get() { return _scriptEngine.get(); };
    ScriptEnginePointer getShared() { return _scriptEngine; };
private:
    std::mutex _scriptEngineLock;
    ScriptEnginePointer _scriptEngine { nullptr };
    std::shared_ptr<QThread> _scriptEngineThread { nullptr };
};

#endif  //overte_HelperScriptEngine_h
