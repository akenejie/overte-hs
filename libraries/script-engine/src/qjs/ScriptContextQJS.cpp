//
//  ScriptContextQJS.cpp
//  libraries/script-engine/src/qjs
//
//  Created for Overte by tomoya on 2026-08-16.
//  Copyright 2026 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//  SPDX-License-Identifier: Apache-2.0
//

#include "ScriptContextQJS.h"

#include <QtCore/QDebug>

#include "ScriptEngineQJS.h"
#include "ScriptValueQJSWrapper.h"

ScriptFunctionContextQJS::ScriptFunctionContextQJS(const QString& fileName, const QString& functionName,
                                                   FunctionType type, int lineNumber) :
    _fileName(fileName),
    _functionName(functionName),
    _functionType(type),
    _lineNumber(lineNumber) {
}

QString ScriptFunctionContextQJS::fileName() const {
    return _fileName;
}

QString ScriptFunctionContextQJS::functionName() const {
    return _functionName;
}

ScriptFunctionContext::FunctionType ScriptFunctionContextQJS::functionType() const {
    return _functionType;
}

int ScriptFunctionContextQJS::lineNumber() const {
    return _lineNumber;
}

ScriptContextQJS::ScriptContextQJS(ScriptEngineQJS* engine, JSValueConst thisObject, int argc, JSValueConst* argv,
                                   ScriptContextPointer parent) :
    _engine(engine),
    _engineHandle(engine->engineHandle()),
    _argc(argc),
    _argv(argv),
    _parentContext(parent) {
    if (JS_IsObject(thisObject) || JS_IsUndefined(thisObject) || JS_IsNull(thisObject)) {
        _thisObject = qjs::dupValue(_engineHandle, thisObject);
    }
}

ScriptContextQJS::~ScriptContextQJS() {
}

ScriptContextQJS* ScriptContextQJS::unwrap(ScriptContext* val) {
    if (!val) {
        return nullptr;
    }
    return dynamic_cast<ScriptContextQJS*>(val);
}

int ScriptContextQJS::argumentCount() const {
    return _argc;
}

ScriptValue ScriptContextQJS::argument(int index) const {
    if (_argv && index >= 0 && index < _argc) {
        return _engine->toScriptValue(qjs::dupValue(_engineHandle, _argv[index]));
    }
    return _engine->undefinedValue();
}

QStringList ScriptContextQJS::backtrace() const {
    return QStringList();
}

int ScriptContextQJS::currentLineNumber() const {
    return -1;
}

QString ScriptContextQJS::currentFileName() const {
    return QString();
}

ScriptValue ScriptContextQJS::callee() const {
    return _engine->undefinedValue();
}

ScriptEnginePointer ScriptContextQJS::engine() const {
    return _engine->shared_from_this();
}

ScriptFunctionContextPointer ScriptContextQJS::functionContext() const {
    return std::make_shared<ScriptFunctionContextQJS>(QString(), QString(), ScriptFunctionContext::NativeFunction, -1);
}

ScriptContextPointer ScriptContextQJS::parentContext() const {
    return _parentContext;
}

ScriptValue ScriptContextQJS::thisObject() const {
    if (_thisObject) {
        return _engine->toScriptValue(qjs::dupValue(_engineHandle, _thisObject->value()));
    }
    return _engine->undefinedValue();
}

JSValueConst ScriptContextQJS::thisValue() const {
    if (_thisObject) {
        return _thisObject->value();
    }
    return JS_UNDEFINED;
}

ScriptValue ScriptContextQJS::throwError(const QString& text) {
    JSContext* ctx = _engineHandle->context();
    JSValue error = JS_NewError(ctx);
    if (!JS_IsException(error)) {
        JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, text.toUtf8().constData()));
        // Ownership of the error value is transferred to the context.
        JS_Throw(ctx, error);
    }
    return _engine->undefinedValue();
}

ScriptValue ScriptContextQJS::throwValue(const ScriptValue& value) {
    JSContext* ctx = _engineHandle->context();
    ScriptValueQJSWrapper* unwrapped = ScriptValueQJSWrapper::unwrap(value);
    if (unwrapped) {
        JS_Throw(ctx, JS_DupValue(ctx, unwrapped->value()));
    }
    return _engine->undefinedValue();
}
