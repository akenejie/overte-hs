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

#include "HelperScriptEngine.h"

#include "ScriptEngine.h"

HelperScriptEngine::HelperScriptEngine() {
    std::lock_guard<std::mutex> lock(_scriptEngineLock);
    _scriptEngineThread.reset(new QThread());
    _scriptEngineThread->start();
    _scriptEngine = newScriptEngine();
    if (!_scriptEngine) {
        // Failure to create a scripting engine is unexpected; all helper
        // operations become no-ops so the entity tree can still be used for
        // packet-based entity networking.
        return;
    }
    _scriptEngine->setThread(_scriptEngineThread.get());
}

HelperScriptEngine::~HelperScriptEngine() {
    {
        std::lock_guard<std::mutex> lock(_scriptEngineLock);
        if (_scriptEngine) {
            _scriptEngine.reset();
        }
    }
    if (_scriptEngineThread) {
        _scriptEngineThread->quit();
        _scriptEngineThread->wait();
    }
}
