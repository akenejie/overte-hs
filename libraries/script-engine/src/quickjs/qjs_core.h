//
//  qjs_core.h
//  libraries/script-engine/src/quickjs
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

#ifndef hifi_qjs_core_h
#define hifi_qjs_core_h

#include <chrono>
#include <cstdint>
#include <memory>

extern "C" {
#include "quickjs.h"
}

/// Abstract equality (==) between two values. Returns 1 for equal, 0 for not
/// equal, and -1 if a QuickJS exception is raised (the exception is thrown on
/// the context).
JS_BOOL JS_Equals(JSContext* ctx, JSValueConst op1, JSValueConst op2);

/// Returns JS_TRUE if @p value is null or undefined.
static inline JS_BOOL JS_IsNullOrUndefined(JSValueConst value) {
    return JS_IsNull(value) || JS_IsUndefined(value);
}

/// Returns JS_TRUE if @p atom is a string atom (rather than a numeric or symbol atom).
static inline JS_BOOL JS_AtomIsString(JSContext* ctx, JSAtom atom) {
    JSValue value = JS_AtomToValue(ctx, atom);
    JS_BOOL result = JS_IsString(value);
    JS_FreeValue(ctx, value);
    return result;
}

namespace qjs {

class QjsEngineHandle;
using QjsEngineHandlePointer = std::shared_ptr<QjsEngineHandle>;

/// Owns a QuickJS JSRuntime and JSContext for one script engine instance.
/// This class intentionally does not depend on Qt.
class QjsEngineHandle {
public:
    static QjsEngineHandlePointer create();

    ~QjsEngineHandle();

    QjsEngineHandle(const QjsEngineHandle&) = delete;
    QjsEngineHandle& operator=(const QjsEngineHandle&) = delete;

    JSRuntime* runtime() const { return _runtime; }
    JSContext* context() const { return _context; }

    /// Maximum wall-clock execution budget for a single evaluation in milliseconds.
    void setExecutionTimeLimit(int milliseconds);
    int executionTimeLimitMs() const { return _executionTimeLimitMs; }

    /// Request an abort of the currently running script. The interrupt handler
    /// is invoked by the engine shortly after this flag is set.
    void requestAbort() { _abortRequested = true; }
    bool abortRequested() const { return _abortRequested; }
    void clearAbortRequest() { _abortRequested = false; }

    /// Marks the beginning/end of a script evaluation; enables the time budget.
    void setEvaluating(bool evaluating);
    bool isEvaluating() const { return _evaluating; }

    void runGC();

    /// Re-anchors the C stack baseline used by the QuickJS stack-overflow check
    /// to the current thread's position. The runtime may be created on one thread
    /// and used on another, so QuickJS's default baseline (captured at
    /// JS_NewRuntime) is meaningless once evaluation crosses threads and the
    /// check spuriously reports "stack overflow". Call this before every
    /// top-level entry into the JS engine so only genuine >max-stack-size C
    /// recursion (not thread identity) can trip the check.
    void refreshStackTop();

    JSMemoryUsage memoryUsage() const;

    /// Returns true if the last evaluation was aborted by the interrupt handler.
    bool wasInterrupted() const { return _interrupted; }
    void resetInterrupted() { _interrupted = false; }

    /// Class ID of the generic "external data" JS class. Instances of this class
    /// hold a C++ pointer as opaque data; the pointer is released with a custom
    /// deleter when the object is garbage collected.
    JSClassID externalClassId() const { return _externalClassId; }
    bool hasExternalClass() const { return _externalClassId != 0; }

private:
    QjsEngineHandle();

    static int interruptHandler(JSRuntime* runtime, void* opaque);

    void registerExternalClass();

    JSRuntime* _runtime;
    JSContext* _context;
    JSClassID _externalClassId;
    int _executionTimeLimitMs;
    bool _abortRequested;
    bool _interrupted;
    bool _evaluating;
    std::chrono::steady_clock::time_point _evaluationStart;
};

/// Create a JS object holding an arbitrary C++ pointer. When the object is
/// garbage collected (or the runtime is destroyed), @p deleter is invoked
/// with @p data.
JSValue newExternalObject(JSContext* ctx, JSClassID classId, void* data, void (*deleter)(void*));

/// Retrieve the C++ pointer stored in an external object. Returns nullptr if
/// @p value is not an external object of the given class.
void* getExternalData(JSContext* ctx, JSClassID classId, JSValueConst value);

/// Pack a C++ pointer into two int32 JSValues for use with JS_NewCFunctionData.
void packPointerIntoValues(JSContext* ctx, const void* ptr, JSValue data[2]);

/// Reconstruct a C++ pointer from two int32 JSValues stored in func_data.
void* unpackPointerFromValues(const JSValue* funcData);

/// Reference-counted wrapper for a JSValue that is owned by this handle.
/// The JSValue is free'd when the last QjsValueHandle reference is released.
/// Keeps the owning engine alive as long as values are alive.
class QjsValueHandle {
public:
    /// Takes ownership of @p value (a value previously returned by a JS_* call).
    QjsValueHandle(QjsEngineHandlePointer engine, JSValue value);
    ~QjsValueHandle();

    QjsValueHandle(const QjsValueHandle&) = delete;
    QjsValueHandle& operator=(const QjsValueHandle&) = delete;

    JSContext* context() const { return _engine->context(); }
    JSRuntime* runtime() const { return _engine->runtime(); }
    const JSValue& value() const { return _value; }
    QjsEngineHandlePointer engineHandle() const { return _engine; }

private:
    QjsEngineHandlePointer _engine;
    JSValue _value;
};

using QjsValueHandlePointer = std::shared_ptr<QjsValueHandle>;

/// Create an owned handle for a borrowed value by duplicating it.
QjsValueHandlePointer dupValue(QjsEngineHandlePointer engine, JSValueConst value);

/// Create an owned handle for an uninitialized (empty) value.
QjsValueHandlePointer newEmptyValue(QjsEngineHandlePointer engine);

}  // namespace qjs

#endif  // hifi_qjs_core_h

/// @}
