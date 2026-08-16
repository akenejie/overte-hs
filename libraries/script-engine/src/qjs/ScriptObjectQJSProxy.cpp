//
//  ScriptObjectQJSProxy.cpp
//  libraries/script-engine/src/qjs
//
//  Created for Overte by tomoya on 2026-08-16.
//  Copyright 2026 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//  SPDX-License-Identifier: Apache-2.0
//

#include "ScriptObjectQJSProxy.h"

#include <QtCore/QDebug>
#include <QtCore/QMetaType>
#include <QtCore/QPointer>
#include <QtCore/QThread>

#include "ScriptEngineLoggingQJS.h"
#include "ScriptEngineQJS.h"
#include "ScriptValueQJSWrapper.h"

Q_DECLARE_METATYPE(ScriptValue)

namespace qjs {
// JS_GetAnyOpaque is defined in quickjs.c but not declared in quickjs.h.
extern "C" void* JS_GetAnyOpaque(JSValueConst obj, JSClassID* class_id);
}

// Maximum number of arguments that can be passed to a QMetaMethod::invoke call.
static const int MAX_META_INVOKE_ARGS = 10;

/// State kept alive by a signal-handle JS object (the value returned when a
/// script reads `object.someSignal`). Holds a strong reference to the proxy so
/// the proxy (and its QObject) outlives the handle.
struct QjsSignalHandleState {
    ScriptEngineQJS* engine;
    qjs::QjsValueHandlePointer proxyLifetime;
    QjsSignalSlot* slot;
};

/// State owned by the proxy JS object. Freed by the class finalizer.
struct QjsQObjectProxyState {
    ScriptEngineQJS* engine;
    QPointer<QObject> qobject;
    bool ownsObject { false };
    ScriptEngine::QObjectWrapOptions wrapOptions;
    QHash<QByteArray, qjs::QjsValueHandlePointer> customProperties;
    QHash<QByteArray, QjsSignalSlot*> signalSlots;
};

namespace {

bool methodIsExcluded(const QMetaMethod& method, ScriptEngine::QObjectWrapOptions wrapOptions) {
    if (method.access() == QMetaMethod::Private) {
        return true;
    }
    const QByteArray name = method.name();
    switch (method.methodType()) {
        case QMetaMethod::Signal:
            if (name == "destroyed" || name == "objectNameChanged") {
                return true;
            }
            break;
        case QMetaMethod::Slot:
            if (wrapOptions & ScriptEngine::ExcludeSlots) {
                return true;
            }
            if (name == "deleteLater") {
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

QjsQObjectProxyState* getProxyState(ScriptEngineQJS* engine, JSContext* ctx, JSValueConst obj) {
    if (!engine || !JS_IsObject(obj)) {
        return nullptr;
    }
    return reinterpret_cast<QjsQObjectProxyState*>(JS_GetOpaque(obj, engine->qobjectProxyClassId()));
}

int findPropertyIndex(const QObject* qobject, const QByteArray& name, ScriptEngine::QObjectWrapOptions wrapOptions) {
    const QMetaObject* mo = qobject->metaObject();
    int startIdx = wrapOptions & ScriptEngine::ExcludeSuperClassProperties ? mo->propertyOffset() : 0;
    int count = mo->propertyCount();
    for (int idx = startIdx; idx < count; ++idx) {
        if (mo->property(idx).name() == name) {
            return idx;
        }
    }
    if (qobject->dynamicPropertyNames().contains(name)) {
        return -2;  // dynamic property
    }
    return -1;
}

QList<QMetaMethod> findMethods(const QObject* qobject, const QByteArray& name, ScriptEngine::QObjectWrapOptions wrapOptions) {
    QList<QMetaMethod> result;
    const QMetaObject* mo = qobject->metaObject();
    int startIdx = wrapOptions & ScriptEngine::ExcludeSuperClassMethods ? mo->methodOffset() : 0;
    int count = mo->methodCount();
    for (int idx = startIdx; idx < count; ++idx) {
        QMetaMethod method = mo->method(idx);
        if (method.name() != name) {
            continue;
        }
        if (methodIsExcluded(method, wrapOptions)) {
            continue;
        }
        result.append(method);
    }
    return result;
}

/// Returns the QMetaMethod (a signal) matching @p name, preferring the
/// overload with the most parameters.
QMetaMethod findSignal(const QObject* qobject, const QByteArray& name, ScriptEngine::QObjectWrapOptions wrapOptions) {
    QMetaMethod best;
    const QMetaObject* mo = qobject->metaObject();
    int startIdx = wrapOptions & ScriptEngine::ExcludeSuperClassMethods ? mo->methodOffset() : 0;
    int count = mo->methodCount();
    for (int idx = startIdx; idx < count; ++idx) {
        QMetaMethod method = mo->method(idx);
        if (method.methodType() != QMetaMethod::Signal || method.name() != name) {
            continue;
        }
        if (methodIsExcluded(method, wrapOptions)) {
            continue;
        }
        if (!best.isValid() || method.parameterCount() > best.parameterCount()) {
            best = method;
        }
    }
    return best;
}

/// Converts a JSValue to a QVariant whose user type matches @p destTypeId.
bool jsValueToTypedVariant(ScriptEngineQJS* engine, JSValueConst value, int destTypeId, QVariant& dest) {
    if (destTypeId == qMetaTypeId<ScriptValue>()) {
        dest = QVariant::fromValue(engine->toScriptValueBorrowed(value));
        return true;
    }
    if (destTypeId == QMetaType::QObjectStar) {
        QObject* obj = ScriptObjectQJSProxy::unwrap(engine->context(), value);
        if (!obj && !JS_IsNull(value) && !JS_IsUndefined(value)) {
            return false;
        }
        dest = QVariant::fromValue(obj);
        return true;
    }

    const auto& customTypes = engine->customMarshalMap();
    auto customLookup = customTypes.constFind(destTypeId);
    if (customLookup != customTypes.cend() && customLookup.value().demarshalFunc) {
        ScriptValue wrapped = engine->toScriptValueBorrowed(value);
        QVariant demarshaled;
        if (!customLookup.value().demarshalFunc(wrapped, demarshaled)) {
            return false;
        }
        dest = demarshaled;
        return dest.isValid() && dest.userType() == destTypeId;
    }

    QVariant converted = engine->jsValueToVariant(value);
    if (!converted.isValid()) {
        return false;
    }
    if (converted.userType() == destTypeId || destTypeId == QMetaType::QVariant) {
        dest = converted;
        return true;
    }
    if (!converted.convert(destTypeId)) {
        return false;
    }
    dest = converted;
    return true;
}

QMetaMethod selectBestMethod(const QList<QMetaMethod>& methods, int argCount) {
    QMetaMethod best;
    for (const QMetaMethod& method : methods) {
        if (method.parameterCount() == argCount) {
            return method;
        }
        if (!best.isValid() && method.parameterCount() > argCount) {
            best = method;
        }
    }
    return best;
}

/// JS C-function invoked when a script calls a QObject method exposed on the proxy.
JSValue callQObjectMethod(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* func_data) {
    Q_UNUSED(this_val);
    ScriptEngineQJS* engine = reinterpret_cast<ScriptEngineQJS*>(JS_GetContextOpaque(ctx));
    QjsQObjectProxyState* state = nullptr;
    if (func_data) {
        state = getProxyState(engine, ctx, func_data[0]);
    }
    if (!state || !state->qobject) {
        return JS_ThrowTypeError(ctx, "Referencing deleted native object");
    }

    QObject* qobject = state->qobject;
    const QMetaObject* mo = qobject->metaObject();
    if (magic < 0 || magic >= mo->methodCount()) {
        return JS_ThrowTypeError(ctx, "Invalid native method");
    }
    const QByteArray methodName = mo->method(magic).name();
    QList<QMetaMethod> methods = findMethods(qobject, methodName, state->wrapOptions);
    QMetaMethod method = selectBestMethod(methods, argc);
    if (!method.isValid()) {
        return JS_ThrowTypeError(ctx, "Native call of %s failed: could not locate an overload with the requested arguments",
                                 methodName.constData());
    }

    int numParams = method.parameterCount();
    QVector<QVariant> variantArgs;
    variantArgs.reserve(numParams);
    QVector<QGenericArgument> genArgs(MAX_META_INVOKE_ARGS);

    for (int i = 0; i < numParams; ++i) {
        int typeId = method.parameterType(i);
        if (typeId == QMetaType::UnknownType) {
            return JS_ThrowTypeError(ctx, "Native method %s::%s has an unregistered parameter type",
                                     mo->className(), method.name().constData());
        }
        QVariant converted;
        if (!jsValueToTypedVariant(engine, argv[i], typeId, converted)) {
            return JS_ThrowTypeError(ctx, "Native call of %s::%s failed: Cannot convert parameter %d",
                                     mo->className(), method.name().constData(), i + 1);
        }
        variantArgs.append(converted);
        genArgs[i] = QGenericArgument(QMetaType::typeName(converted.userType()), const_cast<void*>(converted.constData()));
    }

    int returnTypeId = method.returnType();
    if (returnTypeId == QMetaType::Void || returnTypeId == QMetaType::UnknownType) {
        bool success = method.invoke(qobject, Qt::DirectConnection, genArgs[0], genArgs[1], genArgs[2], genArgs[3],
                                     genArgs[4], genArgs[5], genArgs[6], genArgs[7], genArgs[8], genArgs[9]);
        if (!success) {
            return JS_ThrowTypeError(ctx, "Unexpected: Native call of %s::%s failed", mo->className(), method.name().constData());
        }
        return JS_UNDEFINED;
    }

    QVariant returnVariant(returnTypeId, static_cast<void*>(nullptr));
    QGenericReturnArgument returnArg(QMetaType::typeName(returnTypeId), const_cast<void*>(returnVariant.constData()));
    bool success = method.invoke(qobject, Qt::DirectConnection, returnArg, genArgs[0], genArgs[1], genArgs[2], genArgs[3],
                                 genArgs[4], genArgs[5], genArgs[6], genArgs[7], genArgs[8], genArgs[9]);
    if (!success) {
        return JS_ThrowTypeError(ctx, "Unexpected: Native call of %s::%s failed", mo->className(), method.name().constData());
    }

    const auto& customTypes = engine->customMarshalMap();
    auto customLookup = customTypes.constFind(returnTypeId);
    if (customLookup != customTypes.cend() && customLookup.value().marshalFunc) {
        ScriptValue wrapped = customLookup.value().marshalFunc(engine, returnVariant.constData());
        return engine->scriptValueToJSValue(wrapped);
    }
    return engine->variantToJSValue(returnVariant);
}

QjsSignalSlot* getSignalSlot(ScriptEngineQJS* engine, JSContext* ctx, JSValueConst proxyValue, const QMetaMethod& signal) {
    QjsQObjectProxyState* state = getProxyState(engine, ctx, proxyValue);
    if (!state || !state->qobject) {
        return nullptr;
    }
    const QByteArray name = signal.name();
    QjsSignalSlot*& slot = state->signalSlots[name];
    if (!slot) {
        slot = new QjsSignalSlot(engine, state->qobject, signal);
    }
    return slot;
}

JSValue signalConnect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* func_data) {
    Q_UNUSED(this_val);
    Q_UNUSED(magic);
    ScriptEngineQJS* engine = reinterpret_cast<ScriptEngineQJS*>(JS_GetContextOpaque(ctx));
    if (!engine || !func_data || !JS_IsObject(func_data[0])) {
        return JS_UNDEFINED;
    }
    QjsSignalHandleState* handleState =
        reinterpret_cast<QjsSignalHandleState*>(JS_GetOpaque(func_data[0], engine->signalHandleClassId()));
    if (!handleState || !handleState->slot) {
        return JS_UNDEFINED;
    }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "signal.connect requires a function argument");
    }
    handleState->slot->connectCallback(qjs::dupValue(engine->engineHandle(), argv[0]));
    return JS_UNDEFINED;
}

JSValue signalDisconnect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* func_data) {
    Q_UNUSED(this_val);
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    Q_UNUSED(magic);
    ScriptEngineQJS* engine = reinterpret_cast<ScriptEngineQJS*>(JS_GetContextOpaque(ctx));
    if (!engine || !func_data || !JS_IsObject(func_data[0])) {
        return JS_UNDEFINED;
    }
    QjsSignalHandleState* handleState =
        reinterpret_cast<QjsSignalHandleState*>(JS_GetOpaque(func_data[0], engine->signalHandleClassId()));
    if (!handleState || !handleState->slot) {
        return JS_UNDEFINED;
    }
    handleState->slot->disconnectAll();
    return JS_UNDEFINED;
}

JSValue makeSignalHandle(ScriptEngineQJS* engine, JSContext* ctx, JSValueConst proxyValue, const QMetaMethod& signal) {
    QjsQObjectProxyState* state = getProxyState(engine, ctx, proxyValue);
    if (!state || !state->qobject) {
        return JS_UNDEFINED;
    }
    QjsSignalSlot* slot = getSignalSlot(engine, ctx, proxyValue, signal);
    if (!slot) {
        return JS_UNDEFINED;
    }

    QjsSignalHandleState* handleState = new QjsSignalHandleState();
    handleState->engine = engine;
    handleState->proxyLifetime = qjs::dupValue(engine->engineHandle(), proxyValue);
    handleState->slot = slot;

    JSValue handle = JS_NewObjectClass(ctx, engine->signalHandleClassId());
    if (JS_IsException(handle)) {
        delete handleState;
        return JS_EXCEPTION;
    }
    JS_SetOpaque(handle, handleState);

    JSValue data[1];
    data[0] = JS_DupValue(ctx, handle);
    JSValue connectFn = JS_NewCFunctionData(ctx, &signalConnect, 1, 0, 1, data);
    JS_FreeValue(ctx, data[0]);
    JS_SetPropertyStr(ctx, handle, "connect", connectFn);

    data[0] = JS_DupValue(ctx, handle);
    JSValue disconnectFn = JS_NewCFunctionData(ctx, &signalDisconnect, 0, 0, 1, data);
    JS_FreeValue(ctx, data[0]);
    JS_SetPropertyStr(ctx, handle, "disconnect", disconnectFn);

    return handle;
}

/// JS function bound to a QObject method (identified by its meta-object index
/// stored in the C-function magic value).
JSValue makeMethodFunction(ScriptEngineQJS* engine, JSContext* ctx, JSValueConst proxyValue, int methodIndex) {
    JSValue data[1];
    data[0] = JS_DupValue(ctx, proxyValue);
    JSValue fn = JS_NewCFunctionData(ctx, &callQObjectMethod, 0, methodIndex, 1, data);
    JS_FreeValue(ctx, data[0]);
    return fn;
}

}  // namespace

int ScriptObjectQJSProxy::getOwnProperty(JSContext* ctx, JSPropertyDescriptor* desc, JSValueConst obj, JSAtom prop) {
    desc->flags = 0;
    desc->value = JS_UNDEFINED;
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;

    ScriptEngineQJS* engine = reinterpret_cast<ScriptEngineQJS*>(JS_GetContextOpaque(ctx));
    QjsQObjectProxyState* state = getProxyState(engine, ctx, obj);
    if (!state || !state->qobject) {
        return 0;
    }
    QObject* qobject = state->qobject;

    const char* nameStr = JS_AtomToCString(ctx, prop);
    if (!nameStr) {
        return 0;
    }
    QByteArray name(nameStr);
    JS_FreeCString(ctx, nameStr);

    // 1. QObject properties (read through to the QObject)
    int propertyIndex = findPropertyIndex(qobject, name, state->wrapOptions);
    if (propertyIndex != -1) {
        QVariant valueVariant;
        if (propertyIndex == -2) {
            valueVariant = qobject->property(name.constData());
        } else {
            QMetaProperty metaProperty = qobject->metaObject()->property(propertyIndex);
            if (!metaProperty.isScriptable()) {
                return 0;
            }
            valueVariant = metaProperty.read(qobject);
        }
        desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
        if (propertyIndex != -2) {
            QMetaProperty metaProperty = qobject->metaObject()->property(propertyIndex);
            if (metaProperty.isWritable()) {
                desc->flags |= JS_PROP_WRITABLE;
            }
        }
        desc->value = valueVariant.isValid() ? engine->variantToJSValue(valueVariant) : JS_UNDEFINED;
        return 1;
    }

    // 2. QObject methods (exposed as callable JS functions)
    QList<QMetaMethod> methods = findMethods(qobject, name, state->wrapOptions);
    if (!methods.isEmpty()) {
        desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
        desc->value = makeMethodFunction(engine, ctx, obj, methods.first().methodIndex());
        return 1;
    }

    // 3. Signals (exposed as signal-handle objects with connect/disconnect)
    QMetaMethod signal = findSignal(qobject, name, state->wrapOptions);
    if (signal.isValid()) {
        desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
        desc->value = makeSignalHandle(engine, ctx, obj, signal);
        return 1;
    }

    // 4. Custom properties stored by script assignment
    auto customLookup = state->customProperties.constFind(name);
    if (customLookup != state->customProperties.cend()) {
        desc->flags = JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
        desc->value = JS_DupValue(ctx, customLookup.value()->value());
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
        return 1;
    }

    return 0;
}

int ScriptObjectQJSProxy::setProperty(JSContext* ctx, JSValueConst obj, JSAtom prop, JSValueConst value,
                                      JSValueConst receiver, int flags) {
    Q_UNUSED(receiver);
    ScriptEngineQJS* engine = reinterpret_cast<ScriptEngineQJS*>(JS_GetContextOpaque(ctx));
    QjsQObjectProxyState* state = getProxyState(engine, ctx, obj);
    if (!state || !state->qobject) {
        return 0;
    }
    QObject* qobject = state->qobject;

    const char* nameStr = JS_AtomToCString(ctx, prop);
    QByteArray name(nameStr);
    JS_FreeCString(ctx, nameStr);

    // 1. QObject property write-through
    int propertyIndex = findPropertyIndex(qobject, name, state->wrapOptions);
    if (propertyIndex != -1) {
        if (propertyIndex == -2) {
            QVariant valueVariant;
            if (jsValueToTypedVariant(engine, value, QMetaType::QVariant, valueVariant)) {
                qobject->setProperty(name.constData(), valueVariant);
            }
            return 1;
        }
        QMetaProperty metaProperty = qobject->metaObject()->property(propertyIndex);
        if (!metaProperty.isScriptable() || !metaProperty.isWritable()) {
            return 1;
        }
        QVariant valueVariant;
        if (jsValueToTypedVariant(engine, value, metaProperty.userType(), valueVariant)) {
            metaProperty.write(qobject, valueVariant);
        }
        return 1;
    }

    // 2. Signal assignment: obj.signalName = function  =>  connect
    QMetaMethod signal = findSignal(qobject, name, state->wrapOptions);
    if (signal.isValid()) {
        if (JS_IsFunction(ctx, value)) {
            QjsSignalSlot* slot = getSignalSlot(engine, ctx, obj, signal);
            if (slot) {
                slot->connectCallback(qjs::dupValue(engine->engineHandle(), value));
            }
        } else if (JS_IsNull(value) || JS_IsUndefined(value)) {
            QjsSignalSlot* slot = getSignalSlot(engine, ctx, obj, signal);
            if (slot) {
                slot->disconnectAll();
            }
        }
        return 1;
    }

    // 3. QObject methods are not assignable
    if (!findMethods(qobject, name, state->wrapOptions).isEmpty()) {
        return 1;
    }

    // 4. Auto-create a dynamic property if requested
    if (state->wrapOptions & ScriptEngine::AutoCreateDynamicProperties) {
        QVariant valueVariant;
        if (jsValueToTypedVariant(engine, value, QMetaType::QVariant, valueVariant)) {
            qobject->setProperty(name.constData(), valueVariant);
            return 1;
        }
    }

    // 5. Store in the side table
    state->customProperties[name] = qjs::dupValue(engine->engineHandle(), value);
    return 1;
}

int ScriptObjectQJSProxy::getOwnPropertyNames(JSContext* ctx, JSPropertyEnum** ptab, uint32_t* plen, JSValueConst obj) {
    *ptab = nullptr;
    *plen = 0;
    ScriptEngineQJS* engine = reinterpret_cast<ScriptEngineQJS*>(JS_GetContextOpaque(ctx));
    QjsQObjectProxyState* state = getProxyState(engine, ctx, obj);
    if (!state || !state->qobject) {
        return 0;
    }
    QObject* qobject = state->qobject;

    QList<QByteArray> names;
    const QMetaObject* mo = qobject->metaObject();
    int propStartIdx = state->wrapOptions & ScriptEngine::ExcludeSuperClassProperties ? mo->propertyOffset() : 0;
    for (int idx = propStartIdx; idx < mo->propertyCount(); ++idx) {
        QMetaProperty metaProperty = mo->property(idx);
        if (metaProperty.isScriptable()) {
            names.append(QByteArray(metaProperty.name()));
        }
    }
    for (const QByteArray& name : qobject->dynamicPropertyNames()) {
        names.append(name);
    }
    if (!(state->wrapOptions & ScriptEngine::SkipMethodsInEnumeration)) {
        int methodStartIdx = state->wrapOptions & ScriptEngine::ExcludeSuperClassMethods ? mo->methodOffset() : 0;
        for (int idx = methodStartIdx; idx < mo->methodCount(); ++idx) {
            QMetaMethod method = mo->method(idx);
            if (!methodIsExcluded(method, state->wrapOptions)) {
                names.append(method.name());
            }
        }
    }
    for (const QByteArray& name : state->customProperties.keys()) {
        names.append(name);
    }
    if (names.isEmpty()) {
        return 0;
    }

    // De-duplicate while preserving a stable order.
    QSet<QByteArray> unique;
    QList<QByteArray> ordered;
    for (const QByteArray& name : names) {
        if (!unique.contains(name)) {
            unique.insert(name);
            ordered.append(name);
        }
    }

    JSPropertyEnum* tab = static_cast<JSPropertyEnum*>(js_malloc_rt(JS_GetRuntime(ctx), sizeof(JSPropertyEnum) * ordered.size()));
    if (!tab) {
        return -1;
    }
    uint32_t count = 0;
    for (const QByteArray& name : ordered) {
        tab[count].atom = JS_NewAtom(ctx, name.constData());
        tab[count].is_enumerable = true;
        ++count;
    }
    *ptab = tab;
    *plen = count;
    return 0;
}

ScriptValue ScriptObjectQJSProxy::newQObject(ScriptEngineQJS* engine, QObject* object, ScriptEngine::ValueOwnership ownership,
                                             const ScriptEngine::QObjectWrapOptions& options) {
    if (!engine || !object) {
        return engine ? engine->undefinedValue() : ScriptValue();
    }
    JSContext* ctx = engine->context();
    QjsQObjectProxyState* state = new QjsQObjectProxyState();
    state->engine = engine;
    state->qobject = object;
    state->ownsObject = (ownership == ScriptEngine::ScriptOwnership);
    state->wrapOptions = options;

    JSValue obj = JS_NewObjectClass(ctx, engine->qobjectProxyClassId());
    if (JS_IsException(obj)) {
        delete state;
        return engine->undefinedValue();
    }
    JS_SetOpaque(obj, state);
    return engine->toScriptValue(obj);
}

QObject* ScriptObjectQJSProxy::unwrap(JSContext* ctx, JSValueConst value) {
    if (!JS_IsObject(value)) {
        return nullptr;
    }
    ScriptEngineQJS* engine = reinterpret_cast<ScriptEngineQJS*>(JS_GetContextOpaque(ctx));
    QjsQObjectProxyState* state = getProxyState(engine, ctx, value);
    if (!state) {
        return nullptr;
    }
    return state->qobject;
}

void ScriptObjectQJSProxy::finalizeSignalHandle(JSRuntime* rt, JSValue val) {
    Q_UNUSED(rt);
    void* opaque = qjs::JS_GetAnyOpaque(val, nullptr);
    if (!opaque) {
        return;
    }
    delete reinterpret_cast<QjsSignalHandleState*>(opaque);
}

void ScriptObjectQJSProxy::finalizer(JSRuntime* rt, JSValue val) {
    Q_UNUSED(rt);
    void* opaque = qjs::JS_GetAnyOpaque(val, nullptr);
    if (!opaque) {
        return;
    }
    QjsQObjectProxyState* state = reinterpret_cast<QjsQObjectProxyState*>(opaque);

    // Detach signal connections before anything is deleted so that no Qt
    // signal delivery reaches this proxy while it is being torn down.
    for (QjsSignalSlot* slot : state->signalSlots) {
        slot->disconnectAll();
        delete slot;
    }
    state->signalSlots.clear();

    QObject* qobject = state->qobject;
    if (state->ownsObject && qobject) {
        if (qobject->thread() == QThread::currentThread()) {
            delete qobject;
        } else {
            qobject->deleteLater();
        }
    }

    delete state;
}

void ScriptObjectQJSProxy::gcMark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func) {
    Q_UNUSED(rt);
    Q_UNUSED(val);
    Q_UNUSED(mark_func);
    // The JS values referenced by the state (custom properties, callbacks)
    // are reference-counted via QjsValueHandle, so there is nothing to mark.
}

QjsSignalSlot::QjsSignalSlot(ScriptEngineQJS* engine, QObject* qobject, const QMetaMethod& signal) :
    _engine(engine),
    _engineWeak(engine->weak_from_this()),
    _qobject(qobject),
    _signal(signal) {
    if (engine) {
        engine->registerSignalSlot(this);
    }
}

QjsSignalSlot::~QjsSignalSlot() {
    // The engine may already be destroyed (e.g. during runtime teardown), in
    // which case the weak pointer no longer resolves and the registry is gone.
    if (auto engine = _engineWeak.lock()) {
        engine->unregisterSignalSlot(this);
    }
    disconnectAll();
}

bool QjsSignalSlot::connectCallback(const qjs::QjsValueHandlePointer& callback) {
    if (!callback || !_qobject) {
        return false;
    }
    _callbacks.append(callback);
    if (!_connected) {
        // Qt's internal index-based connect. The method index is chosen so
        // that qt_metacall() reduces it to 0 when the signal fires.
        _connection = QMetaObject::connect(_qobject, _signal.methodIndex(), this,
                                           QObject::staticMetaObject.methodCount());
        _connected = bool(_connection);
        if (!_connected) {
            _callbacks.removeLast();
            return false;
        }
    }
    return true;
}

void QjsSignalSlot::disconnectAll() {
    if (_connected) {
        QObject::disconnect(_connection);
        _connected = false;
    }
    _callbacks.clear();
}

int QjsSignalSlot::qt_metacall(QMetaObject::Call call, int id, void** args) {
    id = QObject::qt_metacall(call, id, args);
    if (id != 0 || call != QMetaObject::InvokeMetaMethod) {
        return id;
    }

    // A signal we connected to has fired.
    int numParams = _signal.parameterCount();
    QList<JSValue> builtArgs;
    for (int i = 0; i < numParams; ++i) {
        int typeId = _signal.parameterType(i);
        if (typeId == QMetaType::UnknownType) {
            builtArgs.append(JS_UNDEFINED);
            continue;
        }
        QVariant var(typeId, args[i + 1]);
        builtArgs.append(_engine->variantToJSValue(var));
    }

    JSContext* ctx = _engine->context();
    for (const qjs::QjsValueHandlePointer& callback : _callbacks) {
        if (!callback) {
            continue;
        }
        JSValueConst functionValue = callback->value();
        if (!JS_IsFunction(ctx, functionValue)) {
            continue;
        }
        QVector<JSValueConst> argv;
        argv.reserve(builtArgs.size());
        for (const JSValue& arg : builtArgs) {
            argv.append(arg);
        }
        JSValue thisValue = _engine->globalObjectValue();
        JSValue result = JS_Call(ctx, functionValue, thisValue, static_cast<int>(argv.size()), argv.data());
        if (JS_IsException(result)) {
            _engine->clearPendingException();
        }
        JS_FreeValue(ctx, result);
    }
    for (const JSValue& arg : builtArgs) {
        JS_FreeValue(ctx, arg);
    }
    return 0;
}
