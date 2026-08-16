//
//  ScriptValueQJSWrapper.h
//  libraries/script-engine/src/qjs
//
//  Created for Overte by tomoya on 2026-08-16.
//  Copyright 2026 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//  SPDX-License-Identifier: Apache-2.0
//

/// @addtogroup ScriptEngine
/// @{

#ifndef hifi_ScriptValueQJSWrapper_h
#define hifi_ScriptValueQJSWrapper_h

#include <memory>

#include <QtCore/QString>
#include <QtCore/QVariant>

#include "../ScriptValue.h"
#include "../quickjs/qjs_core.h"

class ScriptEngineQJS;
class ScriptValueIteratorQJS;

/// [QJS] Implements ScriptValue for QuickJS.
///
/// The JSValue is owned by a reference-counted QjsValueHandle which keeps the
/// owning engine (and therefore the QuickJS runtime) alive as long as any
/// value from that engine is still alive.
class ScriptValueQJSWrapper final : public ScriptValueProxy {
public:
    ScriptValueQJSWrapper(ScriptEngineQJS* engine, qjs::QjsValueHandlePointer handle);
    ScriptValueQJSWrapper(ScriptEngineQJS* engine, qjs::QjsEngineHandlePointer engineHandle, qjs::QjsValueHandlePointer handle);

    void enqueueRelease() override;
    void release() override;
    ScriptValueProxy* copy() const override;

    ScriptValue call(const ScriptValue& thisObject = ScriptValue(), const ScriptValueList& args = ScriptValueList()) override;
    ScriptValue call(const ScriptValue& thisObject, const ScriptValue& arguments) override;
    ScriptValue construct(const ScriptValueList& args = ScriptValueList()) override;
    ScriptValue construct(const ScriptValue& arguments) override;
    ScriptValue data() const override;
    ScriptEnginePointer engine() const override;
    bool equals(const ScriptValue& other) const override;
    bool isArray() const override;
    bool isBool() const override;
    bool isError() const override;
    bool isFunction() const override;
    bool isNumber() const override;
    bool isNull() const override;
    bool isObject() const override;
    bool isString() const override;
    bool isUndefined() const override;
    bool isValid() const override;
    bool isVariant() const override;
    ScriptValueIteratorPointer newIterator() const override;
    ScriptValue property(const QString& name, const ScriptValue::ResolveFlags& mode = ScriptValue::ResolvePrototype) const override;
    ScriptValue property(quint32 arrayIndex, const ScriptValue::ResolveFlags& mode = ScriptValue::ResolvePrototype) const override;
    ScriptValue prototype() const override;
    void setData(const ScriptValue& val) override;
    bool hasProperty(const QString& name) const override;
    void setProperty(const QString& name, const ScriptValue& value, const ScriptValue::PropertyFlags& flags = ScriptValue::KeepExistingFlags) override;
    void setProperty(quint32 arrayIndex, const ScriptValue& value, const ScriptValue::PropertyFlags& flags = ScriptValue::KeepExistingFlags) override;
    void setPrototype(const ScriptValue& prototype) override;
    bool strictlyEquals(const ScriptValue& other) const override;
    QList<QString> getPropertyNames() const override;

    bool toBool() const override;
    qint32 toInt32() const override;
    double toInteger() const override;
    double toNumber() const override;
    QString toString() const override;
    quint16 toUInt16() const override;
    quint32 toUInt32() const override;
    QVariant toVariant() const override;
    QObject* toQObject() const override;

    QString repr() const override;

    /// Borrowed JSValue held by this wrapper (never a JS_EXCEPTION).
    JSValueConst value() const;

    ScriptEngineQJS* enginePtr() const { return _engine; }

    static ScriptValueQJSWrapper* unwrap(const ScriptValue& val);

private:
    JSValueConst getPropertyLocal(JSAtom atom) const;

    ScriptEngineQJS* _engine;
    qjs::QjsEngineHandlePointer _engineHandle;
    qjs::QjsValueHandlePointer _handle;
};

#endif  // hifi_ScriptValueQJSWrapper_h

/// @}
