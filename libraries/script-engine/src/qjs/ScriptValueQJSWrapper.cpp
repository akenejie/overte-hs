//
//  ScriptValueQJSWrapper.cpp
//  libraries/script-engine/src/qjs
//
//  Created for Overte by tomoya on 2026-08-16.
//  Copyright 2026 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//  SPDX-License-Identifier: Apache-2.0
//

#include "ScriptValueQJSWrapper.h"

#include <QtCore/QDebug>

#include "ScriptEngineQJS.h"
#include "ScriptEngineLoggingQJS.h"
#include "ScriptValueIteratorQJS.h"
#include "../ScriptEngineLogging.h"

ScriptValueQJSWrapper::ScriptValueQJSWrapper(ScriptEngineQJS* engine, qjs::QjsValueHandlePointer handle) :
    _engine(engine),
    _engineHandle(engine->engineHandle()),
    _handle(std::move(handle)) {
}

ScriptValueQJSWrapper::ScriptValueQJSWrapper(ScriptEngineQJS* engine, qjs::QjsEngineHandlePointer engineHandle,
                                             qjs::QjsValueHandlePointer handle) :
    _engine(engine),
    _engineHandle(std::move(engineHandle)),
    _handle(std::move(handle)) {
}

void ScriptValueQJSWrapper::enqueueRelease() {
    delete this;
}

void ScriptValueQJSWrapper::release() {
    delete this;
}

ScriptValueProxy* ScriptValueQJSWrapper::copy() const {
    if (_handle) {
        return new ScriptValueQJSWrapper(_engine, _engineHandle, qjs::dupValue(_engineHandle, _handle->value()));
    }
    return new ScriptValueQJSWrapper(_engine, _engineHandle, qjs::newEmptyValue(_engineHandle));
}

JSValueConst ScriptValueQJSWrapper::value() const {
    if (_handle) {
        return _handle->value();
    }
    return JS_UNDEFINED;
}

ScriptValueQJSWrapper* ScriptValueQJSWrapper::unwrap(const ScriptValue& val) {
    return dynamic_cast<ScriptValueQJSWrapper*>(val.ptr());
}

ScriptEnginePointer ScriptValueQJSWrapper::engine() const {
    if (_engine) {
        return _engine->shared_from_this();
    }
    return ScriptEnginePointer();
}

ScriptValue ScriptValueQJSWrapper::call(const ScriptValue& thisObject, const ScriptValueList& args) {
    JSContext* ctx = _engine->context();
    JSValueConst thisVal;
    if (ScriptValueQJSWrapper* unwrappedThis = unwrap(thisObject)) {
        thisVal = unwrappedThis->value();
    } else if (thisObject.isValid()) {
        JSValue convertedThis = _engine->scriptValueToJSValue(thisObject);
        JSValue resultValue = JS_Call(ctx, value(), convertedThis, 0, nullptr);
        JS_FreeValue(ctx, convertedThis);
        return _engine->wrapCallResult(resultValue, "Function call");
    } else {
        thisVal = _engine->globalObjectValue();
    }

    int argc = static_cast<int>(args.size());
    QVector<JSValue> argValues;
    argValues.reserve(argc);
    for (const ScriptValue& arg : args) {
        argValues.append(_engine->scriptValueToJSValue(arg));
    }

    JSValue resultValue = JS_Call(ctx, value(), thisVal, argc, argValues.isEmpty() ? nullptr : argValues.data());
    for (JSValue& argValue : argValues) {
        JS_FreeValue(ctx, argValue);
    }
    return _engine->wrapCallResult(resultValue, "Function call");
}

ScriptValue ScriptValueQJSWrapper::call(const ScriptValue& thisObject, const ScriptValue& arguments) {
    ScriptValueList args;
    ScriptValue lengthValue = arguments.property("length");
    int length = lengthValue.isValid() ? static_cast<int>(lengthValue.toInteger()) : 0;
    if (length > 100000) {
        length = 0;
    }
    for (int i = 0; i < length; ++i) {
        args.append(arguments.property(static_cast<quint32>(i)));
    }
    return call(thisObject, args);
}

ScriptValue ScriptValueQJSWrapper::construct(const ScriptValueList& args) {
    JSContext* ctx = _engine->context();
    if (!JS_IsFunction(ctx, value())) {
        qCWarning(scriptengine_qjs) << "ScriptValueQJSWrapper::construct: value is not a function";
        return _engine->undefinedValue();
    }
    int argc = static_cast<int>(args.size());
    QVector<JSValue> argValues;
    argValues.reserve(argc);
    for (const ScriptValue& arg : args) {
        argValues.append(_engine->scriptValueToJSValue(arg));
    }
    JSValue resultValue = JS_CallConstructor(ctx, value(), argc, argValues.isEmpty() ? nullptr : argValues.data());
    for (JSValue& argValue : argValues) {
        JS_FreeValue(ctx, argValue);
    }
    return _engine->wrapCallResult(resultValue, "Constructor call");
}

ScriptValue ScriptValueQJSWrapper::construct(const ScriptValue& arguments) {
    ScriptValueList args;
    ScriptValue lengthValue = arguments.property("length");
    int length = lengthValue.isValid() ? static_cast<int>(lengthValue.toInteger()) : 0;
    if (length > 100000) {
        length = 0;
    }
    for (int i = 0; i < length; ++i) {
        args.append(arguments.property(static_cast<quint32>(i)));
    }
    return construct(args);
}

ScriptValue ScriptValueQJSWrapper::data() const {
    return property("__data");
}

void ScriptValueQJSWrapper::setData(const ScriptValue& val) {
    setProperty("__data", val);
}

JSValueConst ScriptValueQJSWrapper::getPropertyLocal(JSAtom atom) const {
    JSContext* ctx = _engine->context();
    JSValue result = JS_UNDEFINED;
    JSPropertyDescriptor desc;
    int ret = JS_GetOwnProperty(ctx, &desc, value(), atom);
    if (ret < 0) {
        _engine->clearPendingException();
        return JS_UNDEFINED;
    }
    if (ret) {
        if (desc.flags & JS_PROP_HAS_GET) {
            result = desc.getter;
        } else {
            result = desc.value;
        }
        JS_FreeValue(ctx, desc.value);
        JS_FreeValue(ctx, desc.getter);
        JS_FreeValue(ctx, desc.setter);
        return result;
    }
    return JS_UNDEFINED;
}

ScriptValue ScriptValueQJSWrapper::property(const QString& name, const ScriptValue::ResolveFlags& mode) const {
    JSContext* ctx = _engine->context();
    if (!_handle || JS_IsNull(value()) || JS_IsUndefined(value())) {
        return _engine->undefinedValue();
    }
    JSAtom atom = JS_NewAtom(ctx, name.toUtf8().constData());
    if (atom == JS_ATOM_NULL) {
        return _engine->undefinedValue();
    }
    JSValue result;
    if (mode == ScriptValue::ResolveLocal) {
        result = getPropertyLocal(atom);
    } else {
        result = JS_GetProperty(ctx, value(), atom);
    }
    JS_FreeAtom(ctx, atom);
    if (JS_IsException(result)) {
        _engine->clearPendingException();
        return _engine->undefinedValue();
    }
    if (JS_IsUndefined(result) || JS_IsNull(result)) {
        JS_FreeValue(ctx, result);
        return _engine->undefinedValue();
    }
    return _engine->toScriptValue(qjs::QjsValueHandlePointer(new qjs::QjsValueHandle(_engineHandle, result)));
}

ScriptValue ScriptValueQJSWrapper::property(quint32 arrayIndex, const ScriptValue::ResolveFlags& mode) const {
    JSContext* ctx = _engine->context();
    if (!_handle || JS_IsNull(value()) || JS_IsUndefined(value())) {
        return _engine->undefinedValue();
    }
    JSValue result;
    if (mode == ScriptValue::ResolveLocal) {
        JSAtom atom = JS_NewAtomUInt32(ctx, arrayIndex);
        if (atom == JS_ATOM_NULL) {
            return _engine->undefinedValue();
        }
        result = getPropertyLocal(atom);
        JS_FreeAtom(ctx, atom);
    } else {
        result = JS_GetPropertyUint32(ctx, value(), arrayIndex);
    }
    if (JS_IsException(result)) {
        _engine->clearPendingException();
        return _engine->undefinedValue();
    }
    if (JS_IsUndefined(result) || JS_IsNull(result)) {
        JS_FreeValue(ctx, result);
        return _engine->undefinedValue();
    }
    return _engine->toScriptValue(qjs::QjsValueHandlePointer(new qjs::QjsValueHandle(_engineHandle, result)));
}

ScriptValue ScriptValueQJSWrapper::prototype() const {
    JSContext* ctx = _engine->context();
    if (!_handle || !JS_IsObject(value())) {
        return _engine->undefinedValue();
    }
    JSValue result = JS_GetPrototype(ctx, value());
    if (JS_IsException(result)) {
        _engine->clearPendingException();
        return _engine->undefinedValue();
    }
    if (JS_IsNull(result) || JS_IsUndefined(result)) {
        JS_FreeValue(ctx, result);
        return _engine->undefinedValue();
    }
    return _engine->toScriptValue(qjs::QjsValueHandlePointer(new qjs::QjsValueHandle(_engineHandle, result)));
}

void ScriptValueQJSWrapper::setPrototype(const ScriptValue& prototype) {
    JSContext* ctx = _engine->context();
    if (!_handle || !JS_IsObject(value())) {
        return;
    }
    ScriptValueQJSWrapper* unwrappedPrototype = unwrap(prototype);
    if (!unwrappedPrototype) {
        return;
    }
    JSValue protoVal = unwrappedPrototype->value();
    if (!JS_IsObject(protoVal) && !JS_IsNull(protoVal)) {
        return;
    }
    if (JS_SetPrototype(ctx, value(), protoVal) < 0) {
        _engine->clearPendingException();
    }
}

bool ScriptValueQJSWrapper::hasProperty(const QString& name) const {
    JSContext* ctx = _engine->context();
    if (!_handle || !JS_IsObject(value())) {
        return false;
    }
    JSAtom atom = JS_NewAtom(ctx, name.toUtf8().constData());
    if (atom == JS_ATOM_NULL) {
        return false;
    }
    int ret = JS_HasProperty(ctx, value(), atom);
    JS_FreeAtom(ctx, atom);
    if (ret < 0) {
        _engine->clearPendingException();
        return false;
    }
    return ret > 0;
}

void ScriptValueQJSWrapper::setProperty(const QString& name, const ScriptValue& value, const ScriptValue::PropertyFlags& flags) {
    JSContext* ctx = _engine->context();
    if (!_handle || !JS_IsObject(this->value())) {
        return;
    }
    JSValue unwrapped = _engine->scriptValueToJSValue(value);
    JSAtom atom = JS_NewAtom(ctx, name.toUtf8().constData());
    if (atom != JS_ATOM_NULL) {
        JS_SetProperty(ctx, this->value(), atom, unwrapped);
        JS_FreeAtom(ctx, atom);
    }
    if (JS_HasException(ctx)) {
        _engine->clearPendingException();
    }
}

void ScriptValueQJSWrapper::setProperty(quint32 arrayIndex, const ScriptValue& value, const ScriptValue::PropertyFlags& flags) {
    JSContext* ctx = _engine->context();
    if (!_handle || !JS_IsObject(this->value())) {
        return;
    }
    JSValue unwrapped = _engine->scriptValueToJSValue(value);
    JS_SetPropertyUint32(ctx, this->value(), arrayIndex, unwrapped);
    if (JS_HasException(ctx)) {
        _engine->clearPendingException();
    }
}

bool ScriptValueQJSWrapper::strictlyEquals(const ScriptValue& other) const {
    ScriptValueQJSWrapper* unwrappedOther = unwrap(other);
    if (!unwrappedOther) {
        return false;
    }
    JSContext* ctx = _engine->context();
    return JS_StrictEq(ctx, value(), unwrappedOther->value());
}

bool ScriptValueQJSWrapper::equals(const ScriptValue& other) const {
    ScriptValueQJSWrapper* unwrappedOther = unwrap(other);
    if (!unwrappedOther) {
        return false;
    }
    JSContext* ctx = _engine->context();
    int ret = JS_Equals(ctx, value(), unwrappedOther->value());
    if (ret < 0) {
        _engine->clearPendingException();
        return false;
    }
    return ret > 0;
}

QList<QString> ScriptValueQJSWrapper::getPropertyNames() const {
    QList<QString> names;
    JSContext* ctx = _engine->context();
    if (!_handle || !JS_IsObject(value())) {
        return names;
    }
    JSPropertyEnum* tab = nullptr;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, value(), 0) < 0) {
        _engine->clearPendingException();
        return names;
    }
    for (uint32_t i = 0; i < len; ++i) {
        JSAtom atom = tab[i].atom;
        if (JS_AtomIsString(ctx, atom)) {
            const char* str = JS_AtomToCString(ctx, atom);
            if (str) {
                names.append(QString::fromUtf8(str));
                JS_FreeCString(ctx, str);
            }
        }
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return names;
}

bool ScriptValueQJSWrapper::toBool() const {
    JSContext* ctx = _engine->context();
    int result = JS_ToBool(ctx, value());
    if (result < 0) {
        _engine->clearPendingException();
        return false;
    }
    return result != 0;
}

qint32 ScriptValueQJSWrapper::toInt32() const {
    JSContext* ctx = _engine->context();
    int32_t result = 0;
    if (JS_ToInt32(ctx, &result, value()) < 0) {
        _engine->clearPendingException();
        return 0;
    }
    return result;
}

double ScriptValueQJSWrapper::toInteger() const {
    JSContext* ctx = _engine->context();
    double result = 0.0;
    if (JS_ToFloat64(ctx, &result, value()) < 0) {
        _engine->clearPendingException();
        return 0.0;
    }
    return static_cast<double>(static_cast<int64_t>(result));
}

double ScriptValueQJSWrapper::toNumber() const {
    JSContext* ctx = _engine->context();
    double result = 0.0;
    if (JS_ToFloat64(ctx, &result, value()) < 0) {
        _engine->clearPendingException();
        return 0.0;
    }
    return result;
}

QString ScriptValueQJSWrapper::toString() const {
    JSContext* ctx = _engine->context();
    const char* str = JS_ToCString(ctx, value());
    if (!str) {
        _engine->clearPendingException();
        return QString();
    }
    QString result = QString::fromUtf8(str);
    JS_FreeCString(ctx, str);
    return result;
}

quint16 ScriptValueQJSWrapper::toUInt16() const {
    JSContext* ctx = _engine->context();
    uint32_t result = 0;
    if (JS_ToUint32(ctx, &result, value()) < 0) {
        _engine->clearPendingException();
        return 0;
    }
    return static_cast<quint16>(result);
}

quint32 ScriptValueQJSWrapper::toUInt32() const {
    JSContext* ctx = _engine->context();
    uint32_t result = 0;
    if (JS_ToUint32(ctx, &result, value()) < 0) {
        _engine->clearPendingException();
        return 0;
    }
    return result;
}

QVariant ScriptValueQJSWrapper::toVariant() const {
    return _engine->jsValueToVariant(value());
}

QObject* ScriptValueQJSWrapper::toQObject() const {
    return _engine->jsValueToQObject(value());
}

bool ScriptValueQJSWrapper::isArray() const {
    return JS_IsArray(_engine->context(), value()) > 0;
}

bool ScriptValueQJSWrapper::isBool() const {
    return JS_IsBool(value());
}

bool ScriptValueQJSWrapper::isError() const {
    return JS_IsError(_engine->context(), value()) > 0;
}

bool ScriptValueQJSWrapper::isFunction() const {
    return JS_IsFunction(_engine->context(), value()) > 0;
}

bool ScriptValueQJSWrapper::isNumber() const {
    return JS_IsNumber(value());
}

bool ScriptValueQJSWrapper::isNull() const {
    return JS_IsNull(value());
}

bool ScriptValueQJSWrapper::isObject() const {
    return JS_IsObject(value());
}

bool ScriptValueQJSWrapper::isString() const {
    return JS_IsString(value());
}

bool ScriptValueQJSWrapper::isUndefined() const {
    return JS_IsUndefined(value());
}

bool ScriptValueQJSWrapper::isValid() const {
    return _handle != nullptr && !JS_IsNullOrUndefined(value());
}
bool ScriptValueQJSWrapper::isVariant() const {
    return false;
}

ScriptValueIteratorPointer ScriptValueQJSWrapper::newIterator() const {
    return std::make_shared<ScriptValueIteratorQJS>(_engine, value());
}

QString ScriptValueQJSWrapper::repr() const {
    if (isUndefined()) {
        return "undefined";
    }
    if (isNull()) {
        return "null";
    }
    if (isString()) {
        return toString();
    }
    if (isBool()) {
        return toBool() ? "true" : "false";
    }
    if (isNumber()) {
        return toString();
    }
    if (isFunction()) {
        return "function";
    }
    if (isArray()) {
        int length = static_cast<int>(property("length").toInteger());
        QStringList items;
        for (int i = 0; i < length; ++i) {
            items.append(property(static_cast<quint32>(i)).repr());
        }
        return QString("Array(%1) [%2]").arg(length).arg(items.join(", "));
    }
    if (isObject()) {
        const QList<QString> names = getPropertyNames();
        if (names.isEmpty()) {
            return "{}";
        }
        QStringList items;
        for (const QString& name : names) {
            items.append(QString("%1: %2").arg(name, property(name).repr()));
        }
        return QString("{ %1 }").arg(items.join(", "));
    }
    return toString();
}
