//
//  qjs_core.cpp
//  libraries/script-engine/src/quickjs
//
//  Created for Overte by tomoya on 2026-08-16.
//  Copyright 2026 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//  SPDX-License-Identifier: Apache-2.0
//

#include "qjs_core.h"

// JS_GetAnyOpaque is defined in quickjs.c but not declared in quickjs.h.
extern "C" void* JS_GetAnyOpaque(JSValueConst obj, JSClassID* class_id);

JS_BOOL JS_Equals(JSContext* ctx, JSValueConst op1, JSValueConst op2) {
    // Reimplements the ECMAScript abstract equality algorithm (x == y) using
    // the public QuickJS API. Values are normalized (converted) to a common
    // type, then compared; numbers/strings/bigints compare by value and
    // objects by identity via JS_StrictEq.
    JSValue x = JS_DupValue(ctx, op1);
    JSValue y = JS_DupValue(ctx, op2);
    JS_BOOL result = -1;
    for (;;) {
        int tx = JS_VALUE_GET_NORM_TAG(x);
        int ty = JS_VALUE_GET_NORM_TAG(y);
        if (tx == ty) {
            result = JS_StrictEq(ctx, x, y);
            break;
        }
        if ((tx == JS_TAG_NULL && ty == JS_TAG_UNDEFINED) ||
            (tx == JS_TAG_UNDEFINED && ty == JS_TAG_NULL)) {
            result = 1;
            break;
        }
        if (tx == JS_TAG_BOOL) {
            int b = JS_ToBool(ctx, x);
            if (b < 0) {
                break;
            }
            JSValue nx = JS_NewInt32(ctx, b);
            JS_FreeValue(ctx, x);
            x = nx;
            continue;
        }
        if (ty == JS_TAG_BOOL) {
            int b = JS_ToBool(ctx, y);
            if (b < 0) {
                break;
            }
            JSValue ny = JS_NewInt32(ctx, b);
            JS_FreeValue(ctx, y);
            y = ny;
            continue;
        }
        if (JS_IsNumber(x) && JS_IsString(y)) {
            double d;
            if (JS_ToFloat64(ctx, &d, y) < 0) {
                break;
            }
            JSValue ny = JS_NewFloat64(ctx, d);
            JS_FreeValue(ctx, y);
            y = ny;
            continue;
        }
        if (JS_IsString(x) && JS_IsNumber(y)) {
            double d;
            if (JS_ToFloat64(ctx, &d, x) < 0) {
                break;
            }
            JSValue nx = JS_NewFloat64(ctx, d);
            JS_FreeValue(ctx, x);
            x = nx;
            continue;
        }
        // object vs number/string: convert the object to a primitive. The
        // conversion hint matches the other operand's type.
        if (tx == JS_TAG_OBJECT && (JS_IsNumber(y) || JS_IsString(y))) {
            if (JS_IsString(y)) {
                const char* s = JS_ToCString(ctx, x);
                if (!s) {
                    break;
                }
                JSValue nx = JS_NewString(ctx, s);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, x);
                x = nx;
            } else {
                double d;
                if (JS_ToFloat64(ctx, &d, x) < 0) {
                    break;
                }
                JSValue nx = JS_NewFloat64(ctx, d);
                JS_FreeValue(ctx, x);
                x = nx;
            }
            continue;
        }
        if (ty == JS_TAG_OBJECT && (JS_IsNumber(x) || JS_IsString(x))) {
            if (JS_IsString(x)) {
                const char* s = JS_ToCString(ctx, y);
                if (!s) {
                    break;
                }
                JSValue ny = JS_NewString(ctx, s);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, y);
                y = ny;
            } else {
                double d;
                if (JS_ToFloat64(ctx, &d, y) < 0) {
                    break;
                }
                JSValue ny = JS_NewFloat64(ctx, d);
                JS_FreeValue(ctx, y);
                y = ny;
            }
            continue;
        }
        result = 0;
        break;
    }
    JS_FreeValue(ctx, x);
    JS_FreeValue(ctx, y);
    return result;
}

namespace qjs {

static void externalDataFinalizer(JSRuntime* runtime, JSValue obj) {
    JSClassID unusedClassId;
    void* opaque = JS_GetAnyOpaque(obj, &unusedClassId);
    if (!opaque) {
        return;
    }
    // The deleter is stored at the start of the allocation block.
    struct ExternalBlock {
        void (*deleter)(void*);
        void* data;
    };
    ExternalBlock* block = reinterpret_cast<ExternalBlock*>(opaque);
    if (block->deleter) {
        block->deleter(block->data);
    }
    js_free_rt(runtime, block);
}

QjsEngineHandle::QjsEngineHandle() :
    _runtime(nullptr),
    _context(nullptr),
    _externalClassId(0),
    _executionTimeLimitMs(0),
    _abortRequested(false),
    _interrupted(false),
    _evaluating(false) {
}

QjsEngineHandle::~QjsEngineHandle() {
    if (_context) {
        JS_FreeContext(_context);
        _context = nullptr;
    }
    if (_runtime) {
        JS_FreeRuntime(_runtime);
        _runtime = nullptr;
    }
}

QjsEngineHandlePointer QjsEngineHandle::create() {
    QjsEngineHandlePointer result(new QjsEngineHandle());
    result->_runtime = JS_NewRuntime();
    if (!result->_runtime) {
        return QjsEngineHandlePointer();
    }
    JS_SetInterruptHandler(result->_runtime, QjsEngineHandle::interruptHandler, result.get());
    result->_context = JS_NewContext(result->_runtime);
    if (!result->_context) {
        return QjsEngineHandlePointer();
    }
    result->registerExternalClass();
    return result;
}

void QjsEngineHandle::registerExternalClass() {
    JS_NewClassID(&_externalClassId);
    JSClassDef classDef = {};
    classDef.class_name = "OverteExternalData";
    classDef.finalizer = externalDataFinalizer;
    if (JS_NewClass(_runtime, _externalClassId, &classDef) < 0) {
        _externalClassId = 0;
        return;
    }
    // The prototype must be defined so that JS_NewObjectClass works on the class.
    JSValue proto = JS_NewObject(_context);
    JS_SetClassProto(_context, _externalClassId, proto);
}

void QjsEngineHandle::setExecutionTimeLimit(int milliseconds) {
    _executionTimeLimitMs = milliseconds;
}

void QjsEngineHandle::setEvaluating(bool evaluating) {
    if (evaluating && !_evaluating) {
        _evaluationStart = std::chrono::steady_clock::now();
    }
    _evaluating = evaluating;
}

void QjsEngineHandle::runGC() {
    JS_RunGC(_runtime);
}

void QjsEngineHandle::refreshStackTop() {
    JS_UpdateStackTop(_runtime);
}

JSMemoryUsage QjsEngineHandle::memoryUsage() const {
    JSMemoryUsage result;
    memset(&result, 0, sizeof(result));
    JS_ComputeMemoryUsage(_runtime, &result);
    return result;
}

int QjsEngineHandle::interruptHandler(JSRuntime* runtime, void* opaque) {
    QjsEngineHandle* engine = static_cast<QjsEngineHandle*>(opaque);
    if (engine->_abortRequested) {
        engine->_interrupted = true;
        return 1;
    }
    if (engine->_executionTimeLimitMs > 0 && engine->_evaluating) {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - engine->_evaluationStart).count();
        if (elapsedMs > engine->_executionTimeLimitMs) {
            engine->_interrupted = true;
            return 1;
        }
    }
    return 0;
}

QjsValueHandle::QjsValueHandle(QjsEngineHandlePointer engine, JSValue value) :
    _engine(std::move(engine)),
    _value(value) {
}

QjsValueHandle::~QjsValueHandle() {
    if (_engine) {
        JS_FreeValue(_engine->context(), _value);
    }
}

QjsValueHandlePointer dupValue(QjsEngineHandlePointer engine, JSValueConst value) {
    JSValue dup = JS_DupValue(engine->context(), value);
    return std::make_shared<QjsValueHandle>(std::move(engine), dup);
}

QjsValueHandlePointer newEmptyValue(QjsEngineHandlePointer engine) {
    return std::make_shared<QjsValueHandle>(std::move(engine), JS_UNDEFINED);
}

JSValue newExternalObject(JSContext* ctx, JSClassID classId, void* data, void (*deleter)(void*)) {
    struct ExternalBlock {
        void (*deleter)(void*);
        void* data;
    };
    ExternalBlock* block = reinterpret_cast<ExternalBlock*>(js_malloc_rt(JS_GetRuntime(ctx), sizeof(ExternalBlock)));
    if (!block) {
        return JS_EXCEPTION;
    }
    block->deleter = deleter;
    block->data = data;
    JSValue obj = JS_NewObjectClass(ctx, classId);
    if (JS_IsException(obj)) {
        js_free_rt(JS_GetRuntime(ctx), block);
        return obj;
    }
    JS_SetOpaque(obj, block);
    return obj;
}

void* getExternalData(JSContext* ctx, JSClassID classId, JSValueConst value) {
    void* opaque = JS_GetOpaque(value, classId);
    if (!opaque) {
        return nullptr;
    }
    struct ExternalBlock {
        void (*deleter)(void*);
        void* data;
    };
    ExternalBlock* block = reinterpret_cast<ExternalBlock*>(opaque);
    return block->data;
}

void packPointerIntoValues(JSContext* ctx, const void* ptr, JSValue data[2]) {
    uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
    data[0] = JS_NewInt32(ctx, static_cast<int32_t>(address & 0xFFFFFFFFu));
    data[1] = JS_NewInt32(ctx, static_cast<int32_t>(address >> 32));
}

void* unpackPointerFromValues(const JSValue* funcData) {
    uintptr_t low = static_cast<uint32_t>(JS_VALUE_GET_INT(funcData[0]));
    uintptr_t high = static_cast<uint32_t>(JS_VALUE_GET_INT(funcData[1]));
    return reinterpret_cast<void*>((high << 32) | low);
}

}  // namespace qjs
