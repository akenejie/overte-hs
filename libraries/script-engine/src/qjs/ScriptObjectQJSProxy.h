//
//  ScriptObjectQJSProxy.h
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

#ifndef hifi_ScriptObjectQJSProxy_h
#define hifi_ScriptObjectQJSProxy_h

#include <memory>

#include <QtCore/QHash>
#include <QtCore/QMetaMethod>
#include <QtCore/QObject>

#include "../ScriptEngine.h"
#include "../quickjs/qjs_core.h"

class ScriptEngineQJS;
class QjsSignalSlot;

/// @brief A JS object that forwards property reads/writes and method calls to a QObject.
///
/// The proxy is a QuickJS exotic object:
///   - get_own_property: resolves QObject properties / methods / signals and side-table properties
///   - set_property: writes QObject properties, connects signals, stores side-table properties
///   - get_own_property_names: enumerates QObject members + side-table properties
///
/// Script-assigned properties that are not QObject members are stored in a side table
/// kept on the proxy state so that enumeration and reads stay consistent.
class ScriptObjectQJSProxy {
public:
    /// Creates (or reuses) a JS value wrapping @p object.
    static ScriptValue newQObject(ScriptEngineQJS* engine, QObject* object, ScriptEngine::ValueOwnership ownership,
                                  const ScriptEngine::QObjectWrapOptions& options);

    /// Returns the QObject wrapped by @p value, or nullptr if @p value is not a proxy.
    static QObject* unwrap(JSContext* ctx, JSValueConst value);

    // QuickJS exotic methods
    static int getOwnProperty(JSContext* ctx, JSPropertyDescriptor* desc, JSValueConst obj, JSAtom prop);
    static int getOwnPropertyNames(JSContext* ctx, JSPropertyEnum** ptab, uint32_t* plen, JSValueConst obj);
    static int setProperty(JSContext* ctx, JSValueConst obj, JSAtom prop, JSValueConst value, JSValueConst receiver, int flags);

private:
    static void finalizer(JSRuntime* rt, JSValue val);
    static void gcMark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);
    static void finalizeSignalHandle(JSRuntime* rt, JSValue val);

    friend class ScriptEngineQJS;
};

/// @brief A QObject that receives a QObject signal connection and dispatches to JS callbacks.
///
/// This uses Qt's internal index-based connect (QMetaObject::connect) with the absolute
/// method index equal to QObject::staticMetaObject.methodCount(). On activation,
/// QObject::qt_metacall reduces that index to 0, which this override detects.
class QjsSignalSlot final : public QObject {
public:
    QjsSignalSlot(ScriptEngineQJS* engine, QObject* qobject, const QMetaMethod& signal);
    ~QjsSignalSlot() override;

    /// Registers a JS callback. Returns true if the Qt connection was established.
    bool connectCallback(const qjs::QjsValueHandlePointer& callback);

    void disconnectAll();

    const QMetaMethod& signal() const { return _signal; }
    bool isConnected() const { return _connected; }
    bool hasCallbacks() const { return !_callbacks.isEmpty(); }

protected:
    int qt_metacall(QMetaObject::Call call, int id, void** args) override;

private:
    ScriptEngineQJS* _engine;
    std::weak_ptr<ScriptEngineQJS> _engineWeak;
    QObject* _qobject;
    QMetaMethod _signal;
    QMetaObject::Connection _connection;
    QList<qjs::QjsValueHandlePointer> _callbacks;
    bool _connected { false };
};

#endif  // hifi_ScriptObjectQJSProxy_h

/// @}
