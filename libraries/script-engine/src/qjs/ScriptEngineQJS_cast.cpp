//
//  ScriptEngineQJS_cast.cpp
//  libraries/script-engine/src/qjs
//
//  Created for Overte by tomoya on 2026-08-16.
//  Copyright 2026 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//  SPDX-License-Identifier: Apache-2.0
//

#include "ScriptEngineQJS.h"

#include <functional>

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include "ScriptEngineLoggingQJS.h"
#include "ScriptObjectQJSProxy.h"
#include "ScriptValueQJSWrapper.h"
#include "../ScriptEngineCast.h"
#include "../ScriptValueIterator.h"

Q_DECLARE_METATYPE(ScriptValue);
Q_DECLARE_METATYPE(QVariantMap);

namespace {

QString jsValueToString(JSContext* ctx, JSValueConst value) {
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        return QString();
    }
    const char* str = JS_ToCString(ctx, value);
    if (!str) {
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
        JS_SetUncatchableException(ctx, 0);
        return QString();
    }
    QString result = QString::fromUtf8(str);
    JS_FreeCString(ctx, str);
    return result;
}

void clearException(JSContext* ctx) {
    if (JS_HasException(ctx)) {
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
        JS_SetUncatchableException(ctx, 0);
    }
}

void variantProxyFinalizer(JSRuntime* rt, JSValue val) {
    Q_UNUSED(rt);
    void* opaque = JS_GetAnyOpaque(val, nullptr);
    if (opaque) {
        delete static_cast<QVariant*>(opaque);
    }
}

/// Reads the bytes of an ArrayBuffer or typed array JS value.
bool readArrayBufferBytes(JSContext* ctx, JSValueConst value, QByteArray& bytes) {
    size_t size = 0;
    uint8_t* data = JS_GetArrayBuffer(ctx, &size, value);
    if (data) {
        bytes = QByteArray(reinterpret_cast<const char*>(data), static_cast<int>(size));
        return true;
    }
    clearException(ctx);

    size_t byteOffset = 0;
    size_t elementCount = 0;
    size_t bytesPerElement = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &elementCount, &bytesPerElement);
    if (JS_IsException(buffer)) {
        JS_FreeValue(ctx, buffer);
        clearException(ctx);
        return false;
    }
    size_t bufferSize = 0;
    uint8_t* bufferData = JS_GetArrayBuffer(ctx, &bufferSize, buffer);
    if (bufferData) {
        size_t byteLength = elementCount * bytesPerElement;
        bytes = QByteArray(reinterpret_cast<const char*>(bufferData + byteOffset), static_cast<int>(byteLength));
    }
    JS_FreeValue(ctx, buffer);
    if (!bufferData) {
        clearException(ctx);
        return false;
    }
    return true;
}

static ScriptValue StringListToScriptValue(ScriptEngine* engine, const QStringList& src) {
    int len = src.length();
    ScriptValue dest = engine->newArray(len);
    for (int idx = 0; idx < len; ++idx) {
        dest.setProperty(idx, engine->newValue(src.at(idx)));
    }
    return dest;
}

static bool StringListFromScriptValue(const ScriptValue& src, QStringList& dest) {
    if (!src.isArray()) {
        return false;
    }
    int len = static_cast<int>(src.property("length").toInteger());
    dest.clear();
    for (int idx = 0; idx < len; ++idx) {
        dest.append(src.property(idx).toString());
    }
    return true;
}

static ScriptValue VariantListToScriptValue(ScriptEngine* engine, const QVariantList& src) {
    int len = src.length();
    ScriptValue dest = engine->newArray(len);
    for (int idx = 0; idx < len; ++idx) {
        dest.setProperty(idx, engine->newVariant(src.at(idx)));
    }
    return dest;
}

static bool VariantListFromScriptValue(const ScriptValue& src, QVariantList& dest) {
    if (!src.isArray()) {
        return false;
    }
    int len = static_cast<int>(src.property("length").toInteger());
    dest.clear();
    for (int idx = 0; idx < len; ++idx) {
        dest.append(src.property(idx).toVariant());
    }
    return true;
}

static ScriptValue VariantMapToScriptValue(ScriptEngine* engine, const QVariantMap& src) {
    ScriptValue dest = engine->newObject();
    for (QVariantMap::const_iterator iter = src.cbegin(); iter != src.cend(); ++iter) {
        dest.setProperty(iter.key(), engine->newVariant(iter.value()));
    }
    return dest;
}

static bool VariantMapFromScriptValue(const ScriptValue& src, QVariantMap& dest) {
    dest.clear();
    ScriptValueIteratorPointer iter = src.newIterator();
    while (iter->hasNext()) {
        iter->next();
        dest.insert(iter->name(), iter->value().toVariant());
    }
    return true;
}

static ScriptValue VariantHashToScriptValue(ScriptEngine* engine, const QVariantHash& src) {
    ScriptValue dest = engine->newObject();
    for (QVariantHash::const_iterator iter = src.cbegin(); iter != src.cend(); ++iter) {
        dest.setProperty(iter.key(), engine->newVariant(iter.value()));
    }
    return dest;
}

static bool VariantHashFromScriptValue(const ScriptValue& src, QVariantHash& dest) {
    dest.clear();
    ScriptValueIteratorPointer iter = src.newIterator();
    while (iter->hasNext()) {
        iter->next();
        dest.insert(iter->name(), iter->value().toVariant());
    }
    return true;
}

static ScriptValue JsonValueToScriptValue(ScriptEngine* engine, const QJsonValue& src) {
    return engine->newVariant(src.toVariant());
}

static bool JsonValueFromScriptValue(const ScriptValue& src, QJsonValue& dest) {
    dest = QJsonValue::fromVariant(src.toVariant());
    return true;
}

static ScriptValue JsonObjectToScriptValue(ScriptEngine* engine, const QJsonObject& src) {
    QVariantMap map = src.toVariantMap();
    ScriptValue dest = engine->newObject();
    for (QVariantMap::const_iterator iter = map.cbegin(); iter != map.cend(); ++iter) {
        dest.setProperty(iter.key(), engine->newVariant(iter.value()));
    }
    return dest;
}

static bool JsonObjectFromScriptValue(const ScriptValue& src, QJsonObject& dest) {
    QVariantMap map;
    ScriptValueIteratorPointer iter = src.newIterator();
    while (iter->hasNext()) {
        iter->next();
        map.insert(iter->name(), iter->value().toVariant());
    }
    dest = QJsonObject::fromVariantMap(map);
    return true;
}

static ScriptValue JsonArrayToScriptValue(ScriptEngine* engine, const QJsonArray& src) {
    QVariantList list = src.toVariantList();
    int len = list.length();
    ScriptValue dest = engine->newArray(len);
    for (int idx = 0; idx < len; ++idx) {
        dest.setProperty(idx, engine->newVariant(list.at(idx)));
    }
    return dest;
}

static bool JsonArrayFromScriptValue(const ScriptValue& src, QJsonArray& dest) {
    if (!src.isArray()) {
        return false;
    }
    QVariantList list;
    int len = static_cast<int>(src.property("length").toInteger());
    for (int idx = 0; idx < len; ++idx) {
        list.append(src.property(idx).toVariant());
    }
    dest = QJsonArray::fromVariantList(list);
    return true;
}

}  // namespace

void ScriptEngineQJS::registerSystemTypes() {
    scriptRegisterMetaType<QStringList, StringListToScriptValue, StringListFromScriptValue>(static_cast<ScriptEngine*>(this));
    scriptRegisterMetaType<QVariantList, VariantListToScriptValue, VariantListFromScriptValue>(this);
    scriptRegisterMetaType<QVariantMap, VariantMapToScriptValue, VariantMapFromScriptValue>(this);
    scriptRegisterMetaType<QVariantHash, VariantHashToScriptValue, VariantHashFromScriptValue>(this);
    scriptRegisterMetaType<QJsonValue, JsonValueToScriptValue, JsonValueFromScriptValue>(this);
    scriptRegisterMetaType<QJsonObject, JsonObjectToScriptValue, JsonObjectFromScriptValue>(this);
    scriptRegisterMetaType<QJsonArray, JsonArrayToScriptValue, JsonArrayFromScriptValue>(this);
}

void ScriptEngineQJS::registerVariantProxyClass() {
    JS_NewClassID(&_variantProxyClassId);
    JSClassDef classDef = {};
    classDef.class_name = "OverteVariant";
    classDef.finalizer = &variantProxyFinalizer;
    if (JS_NewClass(runtime(), _variantProxyClassId, &classDef) < 0) {
        qCWarning(scriptengine_qjs) << "Failed to register variant proxy class";
        _variantProxyClassId = 0;
        return;
    }
    JSValue proto = JS_NewObject(context());
    JS_SetClassProto(context(), _variantProxyClassId, proto);
}

void ScriptEngineQJS::setDefaultPrototype(int metaTypeId, const ScriptValue& prototype) {
    if (ScriptValueQJSWrapper::unwrap(prototype)) {
        _customPrototypes.insert(metaTypeId, prototype);
    }
}

void ScriptEngineQJS::registerCustomType(int type, ScriptEngine::MarshalFunction marshalFunc,
                                         ScriptEngine::DemarshalFunction demarshalFunc) {
    CustomMarshal& customType = _customTypes.insert(type, CustomMarshal()).value();
    customType.demarshalFunc = demarshalFunc;
    customType.marshalFunc = marshalFunc;
}

ScriptValue ScriptEngineQJS::create(int type, const void* ptr) {
    if (!ptr) {
        return undefinedValue();
    }
    switch (type) {
        case QMetaType::UnknownType:
        case QMetaType::Void:
            return undefinedValue();
        case QMetaType::Nullptr:
            return nullValue();
        case QMetaType::Bool:
            return newValue(*static_cast<const bool*>(ptr));
        case QMetaType::Int:
        case QMetaType::Long:
        case QMetaType::Short:
            return newValue(*static_cast<const int*>(ptr));
        case QMetaType::UInt:
        case QMetaType::UShort:
        case QMetaType::ULong:
            return newValue(*static_cast<const uint*>(ptr));
        case QMetaType::LongLong:
            return toScriptValue(JS_NewInt64(context(), *static_cast<const qlonglong*>(ptr)));
        case QMetaType::ULongLong:
            return toScriptValue(JS_NewInt64(context(), static_cast<qlonglong>(*static_cast<const qulonglong*>(ptr))));
        case QMetaType::Float:
        case QMetaType::Double:
            return newValue(*static_cast<const double*>(ptr));
        case QMetaType::QString:
            return newValue(*static_cast<const QString*>(ptr));
        case QMetaType::QByteArray:
            return newValue(QString::fromUtf8(static_cast<const QByteArray*>(ptr)->constData()));
        case QMetaType::QVariant:
            return newVariant(*static_cast<const QVariant*>(ptr));
        case QMetaType::QObjectStar: {
            QObject* obj = *static_cast<QObject* const*>(ptr);
            if (!obj) {
                return nullValue();
            }
            return newQObject(obj, QtOwnership);
        }
        case QMetaType::QDateTime:
            return toScriptValue(JS_NewFloat64(context(), static_cast<const QDateTime*>(ptr)->currentMSecsSinceEpoch()));
        case QMetaType::QDate:
            return toScriptValue(JS_NewFloat64(context(), static_cast<const QDate*>(ptr)->startOfDay().currentMSecsSinceEpoch()));
        default:
            break;
    }

    // do we have a registered handler for this type?
    {
        auto it = _customTypes.find(type);
        if (it != _customTypes.end() && it->marshalFunc) {
            return it->marshalFunc(this, ptr);
        }
    }

    // check to see if this is a pointer to a QObject-derived class
    if (QMetaType::typeFlags(type) & (QMetaType::PointerToQObject | QMetaType::TrackingPointerToQObject)) {
        QObject* obj = *static_cast<QObject* const*>(ptr);
        if (!obj) {
            return nullValue();
        }
        return newQObject(obj, QtOwnership);
    }

    // generic variant fallback; round-trips through jsValueToVariant
    QVariant var = QVariant(type, ptr);
    if (var.isValid()) {
        return newVariant(var);
    }

    qCDebug(scriptengine_qjs) << "ScriptEngineQJS::create failed for " << QMetaType::typeName(type);
    return undefinedValue();
}

QVariant ScriptEngineQJS::convert(const ScriptValue& value, int type) {
    ScriptValueQJSWrapper* unwrapped = ScriptValueQJSWrapper::unwrap(value);
    if (!unwrapped) {
        return QVariant();
    }
    QVariant result;
    if (!castValueToVariant(unwrapped->value(), type, result)) {
        return QVariant();
    }
    return result;
}

bool ScriptEngineQJS::castValueToVariant(JSValueConst value, int destTypeId, QVariant& dest) {
    JSContext* ctx = context();
    if (!ctx) {
        return false;
    }

    // If we're not particularly interested in a specific type, try to detect
    // if we're dealing with a wrapped QObject of a registered type.
    if (destTypeId == QMetaType::UnknownType) {
        QObject* obj = ScriptObjectQJSProxy::unwrap(ctx, value);
        if (obj) {
            for (const QMetaObject* metaObject = obj->metaObject(); metaObject; metaObject = metaObject->superClass()) {
                QByteArray typeName = QByteArray(metaObject->className()) + "*";
                int typeId = QMetaType::type(typeName.constData());
                if (typeId != QMetaType::UnknownType) {
                    destTypeId = typeId;
                    break;
                }
            }
        }
    }

    if (destTypeId == qMetaTypeId<ScriptValue>()) {
        dest = QVariant::fromValue(ScriptValue(toScriptValueBorrowed(value)));
        return true;
    }

    // do we have a registered handler for this type?
    {
        auto it = _customTypes.find(destTypeId);
        if (it != _customTypes.end() && it->demarshalFunc) {
            dest = QVariant();
            ScriptValue wrappedVal = toScriptValueBorrowed(value);
            bool success = it->demarshalFunc(wrappedVal, dest);
            if (!success) {
                dest = QVariant();
            }
            return success;
        }
    }

    std::function<bool(JSValueConst, QVariant&)> convertValue = [this](JSValueConst v, QVariant& d) {
        return castValueToVariant(v, QMetaType::UnknownType, d);
    };

    switch (destTypeId) {
        case QMetaType::UnknownType:
            if (JS_IsUndefined(value)) {
                dest = QVariant();
                return true;
            }
            if (JS_IsNull(value)) {
                dest = QVariant::fromValue(nullptr);
                return true;
            }
            if (JS_IsBool(value)) {
                int boolResult = JS_ToBool(ctx, value);
                if (boolResult < 0) {
                    clearException(ctx);
                    return false;
                }
                dest = QVariant::fromValue(boolResult != 0);
                return true;
            }
            if (JS_IsString(value)) {
                dest = QVariant::fromValue(jsValueToString(ctx, value));
                return true;
            }
            if (JS_IsNumber(value)) {
                double number = 0.0;
                if (JS_ToFloat64(ctx, &number, value) < 0) {
                    clearException(ctx);
                    return false;
                }
                dest = QVariant::fromValue(number);
                return true;
            }
            {
                QObject* obj = ScriptObjectQJSProxy::unwrap(ctx, value);
                if (obj) {
                    dest = QVariant::fromValue(obj);
                    return true;
                }
            }
            {
                QVariant var = unwrapVariantProxy(value);
                if (var.isValid()) {
                    dest = var;
                    return true;
                }
            }
            if (JS_IsArray(ctx, value)) {
                JSValue lengthValue = JS_GetPropertyStr(ctx, value, "length");
                uint32_t length = 0;
                if (JS_IsException(lengthValue)) {
                    JS_FreeValue(ctx, lengthValue);
                    clearException(ctx);
                    return false;
                }
                JS_ToUint32(ctx, &length, lengthValue);
                JS_FreeValue(ctx, lengthValue);

                QVariantList list;
                for (uint32_t i = 0; i < length; ++i) {
                    JSValue item = JS_GetPropertyUint32(ctx, value, i);
                    if (JS_IsException(item)) {
                        JS_FreeValue(ctx, item);
                        clearException(ctx);
                        continue;
                    }
                    QVariant itemVariant;
                    if (convertValue(item, itemVariant)) {
                        list.append(itemVariant);
                    }
                    JS_FreeValue(ctx, item);
                }
                dest = QVariant(list);
                return true;
            }
            if (JS_IsObject(value)) {
                JSPropertyEnum* props = nullptr;
                uint32_t count = 0;
                if (JS_GetOwnPropertyNames(ctx, &props, &count, value, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                    clearException(ctx);
                    return false;
                }
                QVariantMap map;
                for (uint32_t i = 0; i < count; ++i) {
                    const char* name = JS_AtomToCString(ctx, props[i].atom);
                    if (name) {
                        JSValue propValue = JS_GetProperty(ctx, value, props[i].atom);
                        if (JS_IsException(propValue)) {
                            JS_FreeValue(ctx, propValue);
                            clearException(ctx);
                        } else {
                            QVariant propVariant;
                            if (convertValue(propValue, propVariant)) {
                                map.insert(QString::fromUtf8(name), propVariant);
                            }
                            JS_FreeValue(ctx, propValue);
                        }
                        JS_FreeCString(ctx, name);
                    }
                    JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
                dest = QVariant(map);
                return true;
            }
            qCDebug(scriptengine_qjs) << "Conversion to variant failed. Value type:" << jsValueToString(ctx, value);
            return false;
        case QMetaType::Bool: {
            int boolResult = JS_ToBool(ctx, value);
            if (boolResult < 0) {
                clearException(ctx);
                return false;
            }
            dest = QVariant::fromValue(boolResult != 0);
            return true;
        }
        case QMetaType::QDateTime:
        case QMetaType::QDate: {
            double timeMs = 0.0;
            if (JS_ToFloat64(ctx, &timeMs, value) < 0) {
                clearException(ctx);
                return false;
            }
            dest = QVariant::fromValue(QDateTime::fromMSecsSinceEpoch(timeMs));
            return true;
        }
        case QMetaType::UInt:
        case QMetaType::ULong: {
            if (JS_IsArray(ctx, value) || JS_IsObject(value)) {
                return false;
            }
            uint32_t result = 0;
            if (JS_ToUint32(ctx, &result, value) < 0) {
                clearException(ctx);
                return false;
            }
            dest = QVariant::fromValue(result);
            return true;
        }
        case QMetaType::Int:
        case QMetaType::Long:
        case QMetaType::Short: {
            if (JS_IsArray(ctx, value) || JS_IsObject(value)) {
                return false;
            }
            int32_t result = 0;
            if (JS_ToInt32(ctx, &result, value) < 0) {
                clearException(ctx);
                return false;
            }
            dest = QVariant::fromValue(result);
            return true;
        }
        case QMetaType::Double:
        case QMetaType::Float:
        case QMetaType::ULongLong:
        case QMetaType::LongLong: {
            if (JS_IsArray(ctx, value) || JS_IsObject(value)) {
                return false;
            }
            double number = 0.0;
            if (JS_ToFloat64(ctx, &number, value) < 0) {
                clearException(ctx);
                return false;
            }
            dest = QVariant::fromValue(number);
            return true;
        }
        case QMetaType::QString:
            dest = QVariant::fromValue(jsValueToString(ctx, value));
            return true;
        case QMetaType::QByteArray: {
            QByteArray bytes;
            if (readArrayBufferBytes(ctx, value, bytes)) {
                dest = QVariant::fromValue(bytes);
                return true;
            }
            dest = QVariant::fromValue(jsValueToString(ctx, value).toUtf8());
            return true;
        }
        case QMetaType::UShort: {
            if (JS_IsArray(ctx, value) || JS_IsObject(value)) {
                return false;
            }
            uint32_t result = 0;
            if (JS_ToUint32(ctx, &result, value) < 0) {
                clearException(ctx);
                return false;
            }
            dest = QVariant::fromValue(static_cast<quint16>(result));
            return true;
        }
        case QMetaType::QObjectStar: {
            QObject* obj = ScriptObjectQJSProxy::unwrap(ctx, value);
            dest = QVariant::fromValue(obj);
            return true;
        }
        case QMetaType::QVariant:
            return castValueToVariant(value, QMetaType::UnknownType, dest);
        default: {
            // check to see if this is a pointer to a QObject-derived object
            if (QMetaType::typeFlags(destTypeId) & (QMetaType::PointerToQObject | QMetaType::TrackingPointerToQObject)) {
                QObject* obj = ScriptObjectQJSProxy::unwrap(ctx, value);
                if (!obj) {
                    return false;
                }
                const QMetaObject* destMeta = QMetaType::metaObjectForType(destTypeId);
                Q_ASSERT(destMeta);
                obj = destMeta->cast(obj);
                if (!obj) {
                    return false;
                }
                dest = QVariant::fromValue(obj);
                return true;
            }
            // check if it's a variant proxy holding the exact type
            {
                QVariant var = unwrapVariantProxy(value);
                if (var.isValid() && var.userType() == destTypeId) {
                    dest = var;
                    return true;
                }
            }
            // last chance: convert generically, then via QVariant::convert
            {
                QVariant generic;
                if (castValueToVariant(value, QMetaType::UnknownType, generic)) {
                    dest = generic;
                    if (dest.userType() == destTypeId) {
                        return true;
                    }
                    return dest.convert(destTypeId);
                }
            }
            qCDebug(scriptengine_qjs) << "Conversion to variant failed. Destination type:" << QMetaType::typeName(destTypeId);
            return false;
        }
    }
}

QVariant ScriptEngineQJS::jsValueToVariant(JSValueConst value) {
    QVariant result;
    if (!castValueToVariant(value, QMetaType::UnknownType, result)) {
        return QVariant();
    }
    return result;
}

QObject* ScriptEngineQJS::jsValueToQObject(JSValueConst value) {
    return ScriptObjectQJSProxy::unwrap(context(), value);
}

JSValue ScriptEngineQJS::variantToJSValue(const QVariant& value) {
    JSContext* ctx = context();
    int valTypeId = value.userType();

    if (valTypeId == qMetaTypeId<ScriptValue>()) {
        // this is a wrapped ScriptValue, so just unwrap it and call it good
        ScriptValue innerVal = value.value<ScriptValue>();
        return scriptValueToJSValue(innerVal);
    }

    // do we have a registered handler for this type?
    {
        auto it = _customTypes.find(valTypeId);
        if (it != _customTypes.end() && it->marshalFunc) {
            Q_ASSERT(value.constData() != nullptr);
            ScriptValue wrappedVal = it->marshalFunc(this, value.constData());
            return scriptValueToJSValue(wrappedVal);
        }
    }

    switch (valTypeId) {
        case QMetaType::UnknownType:
        case QMetaType::Void:
            return JS_DupValue(ctx, JS_UNDEFINED);
        case QMetaType::Nullptr:
            return JS_DupValue(ctx, JS_NULL);
        case QMetaType::Bool:
            return JS_NewBool(ctx, value.toBool());
        case QMetaType::Int:
        case QMetaType::Long:
        case QMetaType::Short:
            return JS_NewInt32(ctx, value.toInt());
        case QMetaType::UInt:
        case QMetaType::UShort:
        case QMetaType::ULong:
            return JS_NewUint32(ctx, value.toUInt());
        case QMetaType::LongLong:
            return JS_NewInt64(ctx, value.toLongLong());
        case QMetaType::ULongLong:
            return JS_NewInt64(ctx, static_cast<qlonglong>(value.toULongLong()));
        case QMetaType::Float:
        case QMetaType::Double:
            return JS_NewFloat64(ctx, value.toDouble());
        case QMetaType::QString:
            return JS_NewString(ctx, value.toString().toUtf8().constData());
        case QMetaType::QByteArray: {
            QByteArray bytes = value.toByteArray();
            return JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        }
        case QMetaType::QVariant:
            return variantToJSValue(value.value<QVariant>());
        case QMetaType::QObjectStar: {
            QObject* obj = value.value<QObject*>();
            if (obj == nullptr) {
                return JS_DupValue(ctx, JS_NULL);
            }
            ScriptValue wrapped = ScriptObjectQJSProxy::newQObject(this, obj, QtOwnership, QObjectWrapOptions());
            return scriptValueToJSValue(wrapped);
        }
        case QMetaType::QDateTime:
            return JS_NewFloat64(ctx, value.value<QDateTime>().currentMSecsSinceEpoch());
        case QMetaType::QDate:
            return JS_NewFloat64(ctx, value.value<QDate>().startOfDay().currentMSecsSinceEpoch());
        default:
            break;
    }

    // check to see if this is a pointer to a QObject-derived class
    if (QMetaType::typeFlags(valTypeId) & (QMetaType::PointerToQObject | QMetaType::TrackingPointerToQObject)) {
        QObject* obj = value.value<QObject*>();
        if (obj == nullptr) {
            return JS_DupValue(ctx, JS_NULL);
        }
        ScriptValue wrapped = ScriptObjectQJSProxy::newQObject(this, obj, QtOwnership, QObjectWrapOptions());
        return scriptValueToJSValue(wrapped);
    }

    auto makeVariantProxy = [this, ctx](const QVariant& v, const ScriptValue& prototype) {
        QVariant* stored = new QVariant(v);
        JSValue obj = JS_NewObjectClass(ctx, _variantProxyClassId);
        if (JS_IsException(obj)) {
            delete stored;
            return obj;
        }
        JS_SetOpaque(obj, stored);
        if (prototype.isValid() && prototype.isObject()) {
            JS_SetPrototype(ctx, obj, scriptValueToJSValue(prototype));
            if (JS_HasException(ctx)) {
                clearException(ctx);
            }
        }
        return obj;
    };

    // have we set a prototype for this type?
    {
        auto it = _customPrototypes.find(valTypeId);
        if (it != _customPrototypes.end()) {
            return makeVariantProxy(value, it.value());
        }
    }

    // generic variant proxy so the value can round-trip
    if (value.isValid()) {
        return makeVariantProxy(value, ScriptValue());
    }

    qCDebug(scriptengine_qjs) << "ScriptEngineQJS::variantToJSValue failed for " << QMetaType::typeName(valTypeId);
    return JS_DupValue(ctx, JS_UNDEFINED);
}

QVariant ScriptEngineQJS::unwrapVariantProxy(JSValueConst value) const {
    if (!_variantProxyClassId || !JS_IsObject(value)) {
        return QVariant();
    }
    QVariant* variant = static_cast<QVariant*>(JS_GetOpaque(value, _variantProxyClassId));
    if (variant) {
        return *variant;
    }
    return QVariant();
}
