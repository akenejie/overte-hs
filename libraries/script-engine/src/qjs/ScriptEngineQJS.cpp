//
//  ScriptEngineQJS.cpp
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

#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QThread>

#include "ScriptContextQJS.h"
#include "ScriptEngineLoggingQJS.h"
#include "ScriptObjectQJSProxy.h"
#include "ScriptProgramQJS.h"
#include "ScriptValueQJSWrapper.h"
#include "../ScriptManager.h"

namespace {

QString jsValueToString(JSContext* ctx, JSValueConst value) {
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        return QString();
    }
    const char* str = JS_ToCString(ctx, value);
    if (!str) {
        // A failed conversion leaves an exception pending; consume it.
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
        JS_SetUncatchableException(ctx, 0);
        return QString();
    }
    QString result = QString::fromUtf8(str);
    JS_FreeCString(ctx, str);
    return result;
}

QString readErrorPropertyString(JSContext* ctx, JSValueConst error, const char* name) {
    JSValue value = JS_GetPropertyStr(ctx, error, name);
    if (JS_IsException(value)) {
        JS_FreeValue(ctx, value);
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
        JS_SetUncatchableException(ctx, 0);
        return QString();
    }
    QString result = jsValueToString(ctx, value);
    JS_FreeValue(ctx, value);
    return result;
}

int readErrorPropertyInt(JSContext* ctx, JSValueConst error, const char* name) {
    JSValue value = JS_GetPropertyStr(ctx, error, name);
    if (JS_IsException(value)) {
        JS_FreeValue(ctx, value);
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
        JS_SetUncatchableException(ctx, 0);
        return -1;
    }
    int32_t result = -1;
    if (JS_ToInt32(ctx, &result, value) < 0) {
        result = -1;
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
        JS_SetUncatchableException(ctx, 0);
    }
    JS_FreeValue(ctx, value);
    return result;
}

QStringList readErrorBacktrace(JSContext* ctx, JSValueConst error) {
    JSValue stack = JS_GetPropertyStr(ctx, error, "stack");
    if (JS_IsException(stack)) {
        JS_FreeValue(ctx, stack);
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
        JS_SetUncatchableException(ctx, 0);
        return QStringList();
    }
    QString stackText = jsValueToString(ctx, stack);
    JS_FreeValue(ctx, stack);
    QStringList result;
    for (const QString& line : stackText.split('\n')) {
        QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            result << trimmed;
        }
    }
    return result;
}

}  // namespace

ScriptEngineQJS::ScriptEngineQJS(ScriptManager* manager) :
    ScriptEngine(manager),
    _thread(QThread::currentThread()),
    _processEventsInterval(0),
    _qobjectProxyClassId(0),
    _signalHandleClassId(0),
    _variantProxyClassId(0),
    _evaluatingCounter(0) {
    _engineHandle = qjs::QjsEngineHandle::create();
    if (!_engineHandle) {
        qCCritical(scriptengine_qjs) << "Failed to create QuickJS runtime";
        return;
    }

    JSContext* ctx = context();
    JS_SetContextOpaque(ctx, this);

    registerQObjectProxyClass();
    registerVariantProxyClass();

    JSValue global = JS_GetGlobalObject(ctx);
    _globalObjectHandle = std::make_shared<qjs::QjsValueHandle>(_engineHandle, global);

    // The base context is pushed once and stays at the bottom of the stack.
    pushContext(globalObjectValue(), 0, nullptr, ScriptContextPointer());

    registerSystemTypes();

    qCDebug(scriptengine_qjs) << "Created new QJS script engine";
}

ScriptEngineQJS::~ScriptEngineQJS() {
    disconnectSignalProxies();
    _signalSlots.clear();
    _customPrototypes.clear();
    _customTypes.clear();
    _contexts.clear();
    _uncaughtException.reset();
    _globalObjectHandle.reset();
    qCDebug(scriptengine_qjs) << "QJS script engine destroyed";
}

ScriptEnginePointer newScriptEngineQJS(ScriptManager* manager) {
    return std::make_shared<ScriptEngineQJS>(manager);
}

std::unique_ptr<ScriptEngine::ScriptEngineScopeGuard> ScriptEngineQJS::getScopeGuard() {
    return std::make_unique<ScriptEngineScopeGuardQJS>();
}

void ScriptEngineQJS::abortEvaluation() {
    if (_engineHandle) {
        _engineHandle->requestAbort();
    }
}

void ScriptEngineQJS::clearExceptions() {
    _uncaughtException.reset();
    clearPendingException();
}

ScriptContext* ScriptEngineQJS::currentContext() const {
    if (_contexts.isEmpty()) {
        return nullptr;
    }
    return _contexts.last().get();
}

ScriptValue ScriptEngineQJS::evaluate(const QString& program, const QString& fileName) {
    refreshStackTop();
    JSContext* ctx = context();
    QString effectiveFileName = fileName.isEmpty() ? "<anonymous>" : fileName;
    beginEvaluation();
    _currentScriptURLs.push_back(effectiveFileName);
    QByteArray programBytes = program.toUtf8();
    QByteArray fileNameBytes = effectiveFileName.toUtf8();
    ScriptValue result = handleEvaluationResult(
        JS_Eval(ctx, programBytes.constData(), programBytes.size(), fileNameBytes.constData(), JS_EVAL_TYPE_GLOBAL),
        "evaluate");
    _currentScriptURLs.pop_back();
    endEvaluation();
    return result;
}

ScriptValue ScriptEngineQJS::evaluate(const ScriptProgramPointer& program) {
    ScriptProgramQJS* scriptProgram = ScriptProgramQJS::unwrap(program);
    if (!scriptProgram) {
        qCCritical(scriptengine_qjs) << "Cannot evaluate a non-QJS ScriptProgram in a QJS engine";
        return undefinedValue();
    }
    JSContext* ctx = context();
    QString fileName = scriptProgram->fileName().isEmpty() ? "<anonymous>" : scriptProgram->fileName();
    refreshStackTop();
    beginEvaluation();
    _currentScriptURLs.push_back(fileName);
    QByteArray sourceBytes = scriptProgram->sourceCode().toUtf8();
    QByteArray fileNameBytes = fileName.toUtf8();
    ScriptValue result = handleEvaluationResult(
        JS_Eval(ctx, sourceBytes.constData(), sourceBytes.size(), fileNameBytes.constData(), JS_EVAL_TYPE_GLOBAL),
        "evaluate");
    _currentScriptURLs.pop_back();
    endEvaluation();
    return result;
}

ScriptValue ScriptEngineQJS::evaluateInClosure(const ScriptValue& locals, const ScriptProgramPointer& program) {
    ScriptProgramQJS* scriptProgram = ScriptProgramQJS::unwrap(program);
    if (!scriptProgram) {
        qCCritical(scriptengine_qjs) << "Cannot evaluate a non-QJS ScriptProgram in a QJS engine";
        return undefinedValue();
    }
    JSContext* ctx = context();
    JSValueConst thisVal = globalObjectValue();
    if (ScriptValueQJSWrapper* localsUnwrapped = ScriptValueQJSWrapper::unwrap(locals)) {
        thisVal = localsUnwrapped->value();
    }
    QString fileName = scriptProgram->fileName().isEmpty() ? "<anonymous>" : scriptProgram->fileName();
    refreshStackTop();
    beginEvaluation();
    _currentScriptURLs.push_back(fileName);
    QByteArray sourceBytes = scriptProgram->sourceCode().toUtf8();
    QByteArray fileNameBytes = fileName.toUtf8();
    ScriptValue result = handleEvaluationResult(
        JS_EvalThis(ctx, thisVal, sourceBytes.constData(), sourceBytes.size(), fileNameBytes.constData(), JS_EVAL_TYPE_GLOBAL),
        "evaluateInClosure");
    _currentScriptURLs.pop_back();
    endEvaluation();
    return result;
}

ScriptValue ScriptEngineQJS::globalObject() {
    return toScriptValueBorrowed(globalObjectValue());
}

bool ScriptEngineQJS::hasUncaughtException() const {
    return _uncaughtException != nullptr;
}

bool ScriptEngineQJS::isEvaluating() const {
    return _evaluatingCounter > 0;
}

ScriptValue ScriptEngineQJS::checkScriptSyntax(ScriptProgramPointer program) {
    ScriptProgramQJS* scriptProgram = ScriptProgramQJS::unwrap(program);
    if (!scriptProgram) {
        qCCritical(scriptengine_qjs) << "checkScriptSyntax called with a non-QJS ScriptProgram";
        return nullValue();
    }
    ScriptSyntaxCheckResultPointer syntaxCheck = scriptProgram->checkSyntax();
    if (!syntaxCheck) {
        return nullValue();
    }
    if (syntaxCheck->state() != ScriptSyntaxCheckResult::Valid) {
        JSContext* ctx = context();
        ScriptValue error = newObject();
        error.setProperty("name", newValue("SyntaxError"));
        error.setProperty("message", newValue(syntaxCheck->errorMessage()));
        error.setProperty("fileName", newValue(scriptProgram->fileName()));
        error.setProperty("lineNumber", newValue(syntaxCheck->errorLineNumber()));
        error.setProperty("columnNumber", newValue(syntaxCheck->errorColumnNumber()));
        error.setProperty("stack", newValue(syntaxCheck->errorBacktrace()));
        const QString formatted = QString("[SyntaxError] %1 in %2:%3(%4)")
            .arg(syntaxCheck->errorMessage(), scriptProgram->fileName())
            .arg(syntaxCheck->errorLineNumber())
            .arg(syntaxCheck->errorColumnNumber());
        error.setProperty("formatted", newValue(formatted));
        Q_UNUSED(ctx);
        return error;
    }
    return undefinedValue();
}

ScriptValue ScriptEngineQJS::newArray(uint length) {
    JSContext* ctx = context();
    JSValue array = JS_NewArray(ctx);
    if (length > 0) {
        JS_SetPropertyStr(ctx, array, "length", JS_NewUint32(ctx, length));
    }
    return toScriptValue(array);
}

ScriptValue ScriptEngineQJS::newArrayBuffer(const QByteArray& message) {
    return toScriptValue(JS_NewArrayBufferCopy(context(),
                                               reinterpret_cast<const uint8_t*>(message.constData()), message.size()));
}

ScriptValue ScriptEngineQJS::newFunction(ScriptEngine::FunctionSignature fun, int length) {
    return toScriptValue(makeFunctionValue(fun, length));
}

ScriptValue ScriptEngineQJS::newObject() {
    return toScriptValue(JS_NewObject(context()));
}

ScriptProgramPointer ScriptEngineQJS::newProgram(const QString& sourceCode, const QString& fileName) {
    return std::make_shared<ScriptProgramQJS>(this, sourceCode, fileName);
}

ScriptValue ScriptEngineQJS::newQObject(QObject* object, ScriptEngine::ValueOwnership ownership,
                                        const ScriptEngine::QObjectWrapOptions& options) {
    return ScriptObjectQJSProxy::newQObject(this, object, ownership, options);
}

ScriptValue ScriptEngineQJS::newValue(bool value) {
    return toScriptValue(JS_NewBool(context(), value));
}

ScriptValue ScriptEngineQJS::newValue(int value) {
    return toScriptValue(JS_NewInt32(context(), value));
}

ScriptValue ScriptEngineQJS::newValue(uint value) {
    return toScriptValue(JS_NewUint32(context(), value));
}

ScriptValue ScriptEngineQJS::newValue(double value) {
    return toScriptValue(JS_NewFloat64(context(), value));
}

ScriptValue ScriptEngineQJS::newValue(const QString& value) {
    return toScriptValue(JS_NewString(context(), value.toUtf8().constData()));
}

ScriptValue ScriptEngineQJS::newValue(const QLatin1String& value) {
    return toScriptValue(JS_NewString(context(), value.latin1()));
}

ScriptValue ScriptEngineQJS::newValue(const char* value) {
    return toScriptValue(JS_NewString(context(), value ? value : ""));
}

ScriptValue ScriptEngineQJS::newVariant(const QVariant& value) {
    return toScriptValue(variantToJSValue(value));
}

ScriptValue ScriptEngineQJS::nullValue() {
    return toScriptValueBorrowed(JS_NULL);
}

ScriptValue ScriptEngineQJS::undefinedValue() {
    return toScriptValueBorrowed(JS_UNDEFINED);
}

ScriptValue ScriptEngineQJS::makeError(const ScriptValue& other, const QString& type) {
    JSContext* ctx = context();
    JSValue error = JS_NewError(ctx);

    if (type != "Error" && !type.isEmpty()) {
        ScriptValue proto = globalObject().property(type);
        if (proto.isValid() && proto.isObject()) {
            JS_SetPrototype(ctx, error, scriptValueToJSValue(proto));
            if (JS_HasException(ctx)) {
                clearPendingException();
            }
        }
    }

    if (ScriptValueQJSWrapper* otherUnwrapped = ScriptValueQJSWrapper::unwrap(other)) {
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &count, otherUnwrapped->value(), JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < count; ++i) {
                const char* name = JS_AtomToCString(ctx, props[i].atom);
                if (name) {
                    JSValue propValue = JS_GetProperty(ctx, otherUnwrapped->value(), props[i].atom);
                    JS_SetPropertyStr(ctx, error, name, propValue);
                    JS_FreeCString(ctx, name);
                }
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        } else {
            clearPendingException();
        }
    }

    return toScriptValue(error);
}

bool ScriptEngineQJS::raiseException(const QString& exception, const QString& reason) {
    if (reason.isEmpty()) {
        return raiseException(newValue(exception));
    }
    return raiseException(newValue(QString("%1: %2").arg(exception, reason)));
}

bool ScriptEngineQJS::raiseException(const ScriptValue& exception, const QString& reason) {
    ScriptValue value = exception;
    if (!reason.isEmpty()) {
        value = newValue(QString("%1: %2").arg(exception.toString(), reason));
    }
    JSContext* ctx = context();
    JSValue exceptionValue = scriptValueToJSValue(value);
    JS_Throw(ctx, exceptionValue);
    return true;
}

void ScriptEngineQJS::registerEnum(ScriptEngineScopeGuard* scopeGuard, const QString& enumName, QMetaEnum newEnum) {
    Q_ASSERT(scopeGuard);
    if (!newEnum.isValid()) {
        qCCritical(scriptengine_qjs) << "registerEnum called on invalid enum with name " << enumName;
        return;
    }
    for (int i = 0; i < newEnum.keyCount(); ++i) {
        const char* keyName = newEnum.key(i);
        QString fullName = enumName + "." + keyName;
        registerValueOnGlobal(fullName, newValue(newEnum.keyToValue(keyName)));
    }
}

void ScriptEngineQJS::registerFunction(ScriptEngineScopeGuard* scopeGuard, const QString& name,
                                       ScriptEngine::FunctionSignature fun, int numArguments) {
    Q_ASSERT(scopeGuard && dynamic_cast<ScriptEngineScopeGuardQJS*>(scopeGuard));
    globalObject().setProperty(name, newFunction(fun, numArguments));
}

void ScriptEngineQJS::registerFunction(ScriptEngineScopeGuard* scopeGuard, const QString& parent, const QString& name,
                                       ScriptEngine::FunctionSignature fun, int numArguments) {
    Q_ASSERT(scopeGuard && dynamic_cast<ScriptEngineScopeGuardQJS*>(scopeGuard));
    ScriptValue object = globalObject().property(parent);
    if (object.isValid() && object.isObject()) {
        object.setProperty(name, newFunction(fun, numArguments));
    }
}

void ScriptEngineQJS::registerGetterSetter(ScriptEngineScopeGuard* scopeGuard, const QString& name,
                                           ScriptEngine::FunctionSignature getter, ScriptEngine::FunctionSignature setter,
                                           const QString& parent) {
    Q_ASSERT(scopeGuard && dynamic_cast<ScriptEngineScopeGuardQJS*>(scopeGuard));
    JSContext* ctx = context();
    JSValue getterValue = makeFunctionValue(getter, 0);
    JSValue setterValue = makeFunctionValue(setter, 1);
    JSAtom nameAtom = JS_NewAtom(ctx, name.toUtf8().constData());

    JSValue target = JS_DupValue(ctx, globalObjectValue());
    if (!parent.isEmpty()) {
        ScriptValue parentValue = globalObject().property(parent);
        if (parentValue.isValid() && parentValue.isObject()) {
            JS_FreeValue(ctx, target);
            target = scriptValueToJSValue(parentValue);
        }
    }

    int flags = JS_PROP_HAS_GET | JS_PROP_HAS_SET | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    if (JS_DefineProperty(ctx, target, nameAtom, JS_UNDEFINED, getterValue, setterValue, flags) < 0) {
        clearPendingException();
    }

    JS_FreeValue(ctx, target);
    JS_FreeAtom(ctx, nameAtom);
    // JS_DefineProperty duplicates getter/setter; the originals are still ours.
    JS_FreeValue(ctx, getterValue);
    JS_FreeValue(ctx, setterValue);
}

void ScriptEngineQJS::registerGlobalObject(ScriptEngineScopeGuard* scopeGuard, const QString& name,
                                           QObject* object, ScriptEngine::ValueOwnership ownership) {
    Q_ASSERT(scopeGuard && dynamic_cast<ScriptEngineScopeGuardQJS*>(scopeGuard));
    ScriptValue value;
    if (object) {
        value = ScriptObjectQJSProxy::newQObject(this, object, ownership, QObjectWrapOptions());
    } else {
        value = nullValue();
    }
    registerValueOnGlobal(name, value);
}

void ScriptEngineQJS::registerSignalSlot(QjsSignalSlot* slot) {
    if (slot) {
        _signalSlots.insert(slot);
    }
}

void ScriptEngineQJS::unregisterSignalSlot(QjsSignalSlot* slot) {
    _signalSlots.remove(slot);
}

void ScriptEngineQJS::setObjectName(const QString& name) {
    _objectName = name;
    QObject::setObjectName(name);
}

bool ScriptEngineQJS::setProperty(const char* name, const QVariant& value) {
    JSContext* ctx = context();
    JSValue converted = variantToJSValue(value);
    int result = JS_SetPropertyStr(ctx, globalObjectValue(), name, converted);
    if (JS_HasException(ctx)) {
        clearPendingException();
        return false;
    }
    return result >= 0;
}

void ScriptEngineQJS::setProcessEventsInterval(int interval) {
    _processEventsInterval = interval;
    if (_engineHandle) {
        // QuickJS uses an interrupt handler; this maps QScriptEngine's
        // "process events interval" to a hard execution time budget.
        _engineHandle->setExecutionTimeLimit(interval);
    }
}

QThread* ScriptEngineQJS::thread() const {
    return _thread;
}

void ScriptEngineQJS::setThread(QThread* thread) {
    if (thread == _thread) {
        return;
    }
    _thread = thread;
    moveToThread(thread);
    qCDebug(scriptengine_qjs) << "Moved script engine " << objectName() << " to different thread";
}

std::shared_ptr<ScriptException> ScriptEngineQJS::uncaughtException() const {
    if (_uncaughtException) {
        return _uncaughtException->clone();
    }
    return std::shared_ptr<ScriptException>();
}

void ScriptEngineQJS::updateMemoryCost(const qint64& deltaSize) {
    Q_UNUSED(deltaSize);
}

void ScriptEngineQJS::requestCollectGarbage() {
    if (_engineHandle) {
        _engineHandle->runGC();
    }
}

void ScriptEngineQJS::processEvents() {
    QEventLoop loop;
    loop.processEvents();
}

void ScriptEngineQJS::compileTest() {
    qCDebug(scriptengine_qjs) << "compileTest is not implemented for the QJS engine";
}

QString ScriptEngineQJS::scriptValueDebugDetails(const ScriptValue& value) {
    if (ScriptValueQJSWrapper* unwrapped = ScriptValueQJSWrapper::unwrap(value)) {
        return jsValueToString(context(), unwrapped->value());
    }
    return value.toString();
}

QString ScriptEngineQJS::scriptValueDebugListMembers(const ScriptValue& value) {
    if (!value.isObject()) {
        return QString();
    }
    return value.getPropertyNames().join('\n');
}

void ScriptEngineQJS::logBacktrace(const QString& title) {
    if (_uncaughtException) {
        qCDebug(scriptengine_qjs) << title << "Backtrace:" << _uncaughtException->backtrace;
    } else {
        qCDebug(scriptengine_qjs) << title << "No backtrace available";
    }
}

ScriptEngineMemoryStatistics ScriptEngineQJS::getMemoryUsageStatistics() {
    ScriptEngineMemoryStatistics statistics;
    memset(&statistics, 0, sizeof(statistics));
    if (!_engineHandle) {
        return statistics;
    }
    JSMemoryUsage usage = _engineHandle->memoryUsage();
    statistics.totalHeapSize = usage.malloc_size;
    statistics.usedHeapSize = usage.memory_used_size;
    statistics.totalAvailableSize = usage.malloc_limit > 0 ? usage.malloc_limit : 0;
    statistics.totalGlobalHandlesSize = usage.atom_size;
    statistics.usedGlobalHandlesSize = usage.str_size;
    return statistics;
}

void ScriptEngineQJS::startCollectingObjectStatistics() {
    qCDebug(scriptengine_qjs) << "startCollectingObjectStatistics is not implemented for the QJS engine";
}

void ScriptEngineQJS::dumpHeapObjectStatistics() {
    qCDebug(scriptengine_qjs) << "dumpHeapObjectStatistics is not implemented for the QJS engine";
}

void ScriptEngineQJS::startProfiling() {
    qCDebug(scriptengine_qjs) << "startProfiling is not implemented for the QJS engine";
}

void ScriptEngineQJS::stopProfilingAndSave() {
    qCDebug(scriptengine_qjs) << "stopProfilingAndSave is not implemented for the QJS engine";
}

void ScriptEngineQJS::disconnectSignalProxies() {
    for (QjsSignalSlot* slot : _signalSlots) {
        slot->disconnectAll();
    }
}

QStringList ScriptEngineQJS::getCurrentScriptURLs() const {
    return _currentScriptURLs;
}

void ScriptEngineQJS::perManagerLoopIterationCleanup() {
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helpers used by QJS wrappers

ScriptValue ScriptEngineQJS::toScriptValue(JSValue value) {
    return ScriptValue(new ScriptValueQJSWrapper(this, std::make_shared<qjs::QjsValueHandle>(_engineHandle, value)));
}

ScriptValue ScriptEngineQJS::toScriptValue(qjs::QjsValueHandlePointer handle) {
    return ScriptValue(new ScriptValueQJSWrapper(this, std::move(handle)));
}

ScriptValue ScriptEngineQJS::toScriptValueBorrowed(JSValueConst value) {
    return ScriptValue(new ScriptValueQJSWrapper(this, qjs::dupValue(_engineHandle, value)));
}

JSValue ScriptEngineQJS::scriptValueToJSValue(const ScriptValue& value) {
    ScriptValueQJSWrapper* unwrapped = ScriptValueQJSWrapper::unwrap(value);
    if (!unwrapped) {
        return JS_DupValue(context(), JS_UNDEFINED);
    }
    return JS_DupValue(context(), unwrapped->value());
}

JSValueConst ScriptEngineQJS::globalObjectValue() const {
    if (_globalObjectHandle) {
        return _globalObjectHandle->value();
    }
    return JS_UNDEFINED;
}

QString ScriptEngineQJS::clearPendingException() {
    JSContext* ctx = context();
    if (!ctx || !JS_HasException(ctx)) {
        return QString();
    }
    JSValue exception = JS_GetException(ctx);
    QString message = jsValueToString(ctx, exception);
    JS_FreeValue(ctx, exception);
    JS_SetUncatchableException(ctx, 0);
    return message;
}

ScriptContextPointer ScriptEngineQJS::pushContext(JSValueConst thisObject, int argc, JSValueConst* argv,
                                                  ScriptContextPointer parent) {
    ScriptContextPointer scriptContext = std::make_shared<ScriptContextQJS>(this, thisObject, argc, argv, parent);
    _contexts.append(scriptContext);
    return scriptContext;
}

void ScriptEngineQJS::popContext() {
    if (!_contexts.isEmpty()) {
        _contexts.removeLast();
    }
}

ScriptValue ScriptEngineQJS::wrapCallResult(JSValue resultValue, const QString& callDescription) {
    Q_UNUSED(callDescription);
    JSContext* ctx = context();
    if (JS_HasException(ctx)) {
        JSValue thrownValue = JS_GetException(ctx);
        JS_SetUncatchableException(ctx, 0);
        return toScriptValue(thrownValue);
    }
    return toScriptValue(resultValue);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Private implementation

void ScriptEngineQJS::setUncaughtException(const QString& message, const QString& info, int line, int column,
                                           const QStringList& backtrace) {
    setUncaughtException(std::make_shared<ScriptException>(message, info, line, column, backtrace));
}

void ScriptEngineQJS::setUncaughtException(std::shared_ptr<ScriptException> newException) {
    if (!newException) {
        return;
    }
    qCDebug(scriptengine_qjs) << "Emitting exception:" << newException;
    _uncaughtException = newException;
    emit exception(newException->clone());
}

void ScriptEngineQJS::setUncaughtExceptionFromValue(JSValue thrownValue, const QString& info) {
    auto exception = std::make_shared<ScriptRuntimeException>();
    exception->additionalInfo = info;

    JSContext* ctx = context();
    if (JS_IsObject(thrownValue)) {
        exception->errorMessage = readErrorPropertyString(ctx, thrownValue, "message");
        exception->errorLine = readErrorPropertyInt(ctx, thrownValue, "lineNumber");
        exception->errorColumn = readErrorPropertyInt(ctx, thrownValue, "columnNumber");
        exception->backtrace = readErrorBacktrace(ctx, thrownValue);
    }
    if (exception->errorMessage.isEmpty()) {
        exception->errorMessage = jsValueToString(ctx, thrownValue);
    }
    if (exception->errorMessage.isEmpty()) {
        exception->errorMessage = "Script error";
    }

    exception->thrownValue = toScriptValue(thrownValue);
    setUncaughtException(std::move(exception));
}

void ScriptEngineQJS::registerQObjectProxyClass() {
    JS_NewClassID(&_qobjectProxyClassId);
    JSClassDef classDef = {};
    classDef.class_name = "OverteQObjectProxy";
    classDef.finalizer = ScriptObjectQJSProxy::finalizer;
    classDef.gc_mark = ScriptObjectQJSProxy::gcMark;
    memset(&_qobjectProxyExotic, 0, sizeof(_qobjectProxyExotic));
    _qobjectProxyExotic.get_own_property = ScriptObjectQJSProxy::getOwnProperty;
    _qobjectProxyExotic.get_own_property_names = ScriptObjectQJSProxy::getOwnPropertyNames;
    _qobjectProxyExotic.set_property = ScriptObjectQJSProxy::setProperty;
    classDef.exotic = &_qobjectProxyExotic;
    if (JS_NewClass(runtime(), _qobjectProxyClassId, &classDef) < 0) {
        qCWarning(scriptengine_qjs) << "Failed to register QObject proxy class";
        _qobjectProxyClassId = 0;
        return;
    }
    JSValue proto = JS_NewObject(context());
    JS_SetClassProto(context(), _qobjectProxyClassId, proto);

    JS_NewClassID(&_signalHandleClassId);
    JSClassDef signalClassDef = {};
    signalClassDef.class_name = "OverteSignalHandle";
    signalClassDef.finalizer = ScriptObjectQJSProxy::finalizeSignalHandle;
    if (JS_NewClass(runtime(), _signalHandleClassId, &signalClassDef) < 0) {
        qCWarning(scriptengine_qjs) << "Failed to register signal handle class";
        _signalHandleClassId = 0;
        return;
    }
    JSValue signalProto = JS_NewObject(context());
    JS_SetClassProto(context(), _signalHandleClassId, signalProto);
}

void ScriptEngineQJS::registerValueOnGlobal(const QString& path, const ScriptValue& value) {
    if (path.isEmpty()) {
        return;
    }
    QStringList parts = path.split('.');
    ScriptValue current = globalObject();
    for (int i = 0; i < parts.size(); ++i) {
        bool isLast = (i == parts.size() - 1);
        if (isLast) {
            current.setProperty(parts[i], value);
        } else {
            ScriptValue child = current.property(parts[i]);
            if (!child.isObject()) {
                child = newObject();
                current.setProperty(parts[i], child);
            }
            current = child;
        }
    }
}

void ScriptEngineQJS::beginEvaluation() {
    ++_evaluatingCounter;
    if (_engineHandle) {
        _engineHandle->setEvaluating(true);
    }
}

void ScriptEngineQJS::endEvaluation() {
    if (_evaluatingCounter > 0) {
        --_evaluatingCounter;
    }
    if (_engineHandle && _evaluatingCounter == 0) {
        _engineHandle->setEvaluating(false);
    }
}

ScriptValue ScriptEngineQJS::handleEvaluationResult(JSValue resultValue, const QString& description) {
    JSContext* ctx = context();
    if (JS_HasException(ctx)) {
        JSValue thrownValue = JS_GetException(ctx);
        JS_SetUncatchableException(ctx, 0);
        if (_engineHandle && _engineHandle->wasInterrupted()) {
            JS_FreeValue(ctx, thrownValue);
            _engineHandle->resetInterrupted();
            _engineHandle->clearAbortRequest();
            thrownValue = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, thrownValue, "message",
                              JS_NewString(ctx, "Script aborted because it exceeded the maximum allowed execution time"));
            JSValue wrappedCopy = JS_DupValue(ctx, thrownValue);
            setUncaughtExceptionFromValue(thrownValue, description);
            return toScriptValue(wrappedCopy);
        }
        JSValue wrappedCopy = JS_DupValue(ctx, thrownValue);
        setUncaughtExceptionFromValue(thrownValue, description);
        return toScriptValue(wrappedCopy);
    }
    return toScriptValue(resultValue);
}

JSValue ScriptEngineQJS::makeFunctionValue(ScriptEngine::FunctionSignature fun, int numArguments) {
    JSContext* ctx = context();
    JSValue data[2];
    qjs::packPointerIntoValues(ctx, reinterpret_cast<const void*>(fun), data);
    int length = numArguments < 0 ? 0 : numArguments;
    JSValue func = JS_NewCFunctionData(ctx, ScriptEngineQJS::callFunctionData, length, numArguments, 2, data);
    JS_FreeValue(ctx, data[0]);
    JS_FreeValue(ctx, data[1]);
    return func;
}

JSValue ScriptEngineQJS::callFunctionData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic,
                                          JSValue* func_data) {
    Q_UNUSED(magic);
    ScriptEngineQJS* engine = static_cast<ScriptEngineQJS*>(JS_GetContextOpaque(ctx));
    if (!engine) {
        return JS_EXCEPTION;
    }
    ScriptEngine::FunctionSignature function =
        reinterpret_cast<ScriptEngine::FunctionSignature>(qjs::unpackPointerFromValues(func_data));
    if (!function) {
        engine->raiseException("Invalid native function");
        return JS_EXCEPTION;
    }

    ScriptContextPointer parent = engine->currentContextPointer();
    ScriptContextPointer scriptContext = engine->pushContext(this_val, argc, argv, parent);
    ScriptEngineScopeGuardQJS guard;
    ScriptValue resultValue = function(scriptContext.get(), engine);
    engine->popContext();

    if (JS_HasException(ctx)) {
        return JS_EXCEPTION;
    }
    return JS_DupValue(ctx, engine->scriptValueToJSValue(resultValue));
}
