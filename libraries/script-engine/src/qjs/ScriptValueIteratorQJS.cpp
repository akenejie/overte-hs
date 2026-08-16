//
//  ScriptValueIteratorQJS.cpp
//  libraries/script-engine/src/qjs
//
//  Created for Overte by tomoya on 2026-08-16.
//  Copyright 2026 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//  SPDX-License-Identifier: Apache-2.0
//

#include "ScriptValueIteratorQJS.h"

#include "ScriptEngineQJS.h"
#include "ScriptValueQJSWrapper.h"

ScriptValueIteratorQJS::ScriptValueIteratorQJS(ScriptEngineQJS* engine, JSValueConst value) :
    _engine(engine),
    _engineHandle(engine->engineHandle()),
    _index(-1) {
    JSContext* ctx = _engineHandle->context();
    if (!JS_IsObject(value)) {
        return;
    }
    _value = qjs::dupValue(_engineHandle, value);
    JSPropertyEnum* tab = nullptr;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, value, 0) < 0) {
        _engine->clearPendingException();
        return;
    }
    for (uint32_t i = 0; i < len; ++i) {
        JSAtom atom = tab[i].atom;
        if (JS_AtomIsString(ctx, atom)) {
            const char* str = JS_AtomToCString(ctx, atom);
            if (str) {
                _names.append(QString::fromUtf8(str));
                JS_FreeCString(ctx, str);
            }
        }
    }
    JS_FreePropertyEnum(ctx, tab, len);
}

ScriptValue::PropertyFlags ScriptValueIteratorQJS::flags() const {
    return ScriptValue::PropertyFlags();
}

bool ScriptValueIteratorQJS::hasNext() const {
    return (_index + 1) < _names.size();
}

QString ScriptValueIteratorQJS::name() const {
    if (_index >= 0 && _index < _names.size()) {
        return _names[_index];
    }
    return QString();
}

void ScriptValueIteratorQJS::next() {
    if (hasNext()) {
        ++_index;
    }
}

ScriptValue ScriptValueIteratorQJS::value() const {
    if (_index >= 0 && _index < _names.size()) {
        return ScriptValue(new ScriptValueQJSWrapper(_engine, _engineHandle, qjs::dupValue(_engineHandle, _value->value()))).property(_names[_index]);
    }
    return _engine->undefinedValue();
}
