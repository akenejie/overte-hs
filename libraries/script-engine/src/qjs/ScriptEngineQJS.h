//
//  ScriptEngineQJS.h
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

#ifndef hifi_ScriptEngineQJS_h
#define hifi_ScriptEngineQJS_h

#include <memory>

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QMetaEnum>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "../ScriptEngine.h"
#include "../ScriptException.h"
#include "../ScriptContext.h"
#include "../quickjs/qjs_core.h"

class QjsSignalSlot;
class ScriptContextQJS;
class ScriptEngineQJS;
class ScriptObjectQJSProxy;
class ScriptManager;
class ScriptValueQJSWrapper;

Q_DECLARE_METATYPE(ScriptEngine::FunctionSignature)

/// [QJS] Implements ScriptEngine for QuickJS
class ScriptEngineQJS final : public ScriptEngine,
                              public std::enable_shared_from_this<ScriptEngineQJS> {
    Q_OBJECT

public:
    explicit ScriptEngineQJS(ScriptManager* manager = nullptr);
    ~ScriptEngineQJS() override;

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // ScriptEngine implementation

    class ScriptEngineScopeGuardQJS final : public ScriptEngineScopeGuard {
    public:
        ScriptEngineScopeGuardQJS() = default;
        ~ScriptEngineScopeGuardQJS() override = default;
    };

    std::unique_ptr<ScriptEngineScopeGuard> getScopeGuard() override;
    void abortEvaluation() override;
    void clearExceptions() override;
    ScriptContext* currentContext() const override;
    ScriptValue evaluate(const QString& program, const QString& fileName = QString()) override;
    ScriptValue evaluate(const ScriptProgramPointer& program) override;
    ScriptValue evaluateInClosure(const ScriptValue& locals, const ScriptProgramPointer& program) override;
    ScriptValue globalObject() override;
    bool hasUncaughtException() const override;
    bool isEvaluating() const override;
    ScriptValue checkScriptSyntax(ScriptProgramPointer program) override;

    ScriptValue newArray(uint length = 0) override;
    ScriptValue newArrayBuffer(const QByteArray& message) override;
    ScriptValue newFunction(ScriptEngine::FunctionSignature fun, int length = 0) override;
    ScriptValue newObject() override;
    ScriptProgramPointer newProgram(const QString& sourceCode, const QString& fileName) override;
    ScriptValue newQObject(QObject* object, ScriptEngine::ValueOwnership ownership = ScriptEngine::QtOwnership,
                           const ScriptEngine::QObjectWrapOptions& options = ScriptEngine::QObjectWrapOptions()) override;
    ScriptValue newValue(bool value) override;
    ScriptValue newValue(int value) override;
    ScriptValue newValue(uint value) override;
    ScriptValue newValue(double value) override;
    ScriptValue newValue(const QString& value) override;
    ScriptValue newValue(const QLatin1String& value) override;
    ScriptValue newValue(const char* value) override;
    ScriptValue newVariant(const QVariant& value) override;
    ScriptValue nullValue() override;

    ScriptValue makeError(const ScriptValue& other, const QString& type = "Error") override;

    bool raiseException(const QString& exception, const QString& reason = QString()) override;
    bool raiseException(const ScriptValue& exception, const QString& reason = QString()) override;
    void registerEnum(ScriptEngineScopeGuard* scopeGuard, const QString& enumName, QMetaEnum newEnum) override;
    void registerFunction(ScriptEngineScopeGuard* scopeGuard, const QString& name,
                          ScriptEngine::FunctionSignature fun, int numArguments = -1) override;
    void registerFunction(ScriptEngineScopeGuard* scopeGuard, const QString& parent, const QString& name,
                          ScriptEngine::FunctionSignature fun, int numArguments = -1) override;
    void registerGetterSetter(ScriptEngineScopeGuard* scopeGuard, const QString& name,
                              ScriptEngine::FunctionSignature getter, ScriptEngine::FunctionSignature setter,
                              const QString& parent = QString("")) override;
    void registerGlobalObject(ScriptEngineScopeGuard* scopeGuard, const QString& name,
                              QObject* object, ScriptEngine::ValueOwnership = ScriptEngine::QtOwnership) override;
    void setDefaultPrototype(int metaTypeId, const ScriptValue& prototype) override;
    void setObjectName(const QString& name) override;
    bool setProperty(const char* name, const QVariant& value) override;
    void setProcessEventsInterval(int interval) override;
    QThread* thread() const override;
    void setThread(QThread* thread) override;
    ScriptValue undefinedValue() override;
    std::shared_ptr<ScriptException> uncaughtException() const override;
    void updateMemoryCost(const qint64& deltaSize) override;
    void requestCollectGarbage() override;
    void processEvents() override;
    void compileTest() override;
    QString scriptValueDebugDetails(const ScriptValue& value) override;
    QString scriptValueDebugListMembers(const ScriptValue& value) override;
    void logBacktrace(const QString& title = QString("")) override;
    ScriptEngineMemoryStatistics getMemoryUsageStatistics() override;
    void startCollectingObjectStatistics() override;
    void dumpHeapObjectStatistics() override;
    void startProfiling() override;
    void stopProfilingAndSave() override;
    void disconnectSignalProxies() override;

    // public non-interface methods for other QJS-specific classes to use
    ScriptValue create(int type, const void* ptr) override;
    QVariant convert(const ScriptValue& value, int type) override;
    void registerCustomType(int type, ScriptEngine::MarshalFunction marshalFunc,
                            ScriptEngine::DemarshalFunction demarshalFunc) override;
    QStringList getCurrentScriptURLs() const override;
    void perManagerLoopIterationCleanup() override;

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Helpers used by QJS wrappers

    JSContext* context() const { return _engineHandle->context(); }
    JSRuntime* runtime() const { return _engineHandle->runtime(); }
    qjs::QjsEngineHandlePointer engineHandle() const { return _engineHandle; }

    /// Re-anchors QuickJS's C-stack baseline to this thread before JS entry.
    /// The runtime is created on the assignment thread but evaluated on the
    /// script manager thread; without re-anchoring, QuickJS would compare two
    /// unrelated thread stacks and spuriously report "stack overflow".
    void refreshStackTop() { _engineHandle->refreshStackTop(); }

    /// Wraps an owned JSValue into a ScriptValue.
    ScriptValue toScriptValue(JSValue value);
    /// Wraps an owned value handle into a ScriptValue.
    ScriptValue toScriptValue(qjs::QjsValueHandlePointer handle);
    /// Wraps a borrowed JSValue into a ScriptValue by duplicating it.
    ScriptValue toScriptValueBorrowed(JSValueConst value);
    /// Converts a ScriptValue to an owned JSValue (duplicated).
    JSValue scriptValueToJSValue(const ScriptValue& value);
    /// Converts a JSValue to a QVariant.
    QVariant jsValueToVariant(JSValueConst value);
    /// Converts a JSValue to a QObject*, if it is a wrapped QObject.
    QObject* jsValueToQObject(JSValueConst value);
    /// Converts a QVariant to an owned JSValue.
    JSValue variantToJSValue(const QVariant& value);

    /// Global JS object (borrowed).
    JSValueConst globalObjectValue() const;

    /// Clears any pending QuickJS exception and returns its message.
    QString clearPendingException();

    /// Context stack management. The first context is the base context used
    /// during evaluate(), native function callbacks push their own context.
    ScriptContextPointer pushContext(JSValueConst thisObject, int argc, JSValueConst* argv, ScriptContextPointer parent);
    void popContext();
    ScriptContextPointer currentContextPointer() const { return _contexts.isEmpty() ? ScriptContextPointer() : _contexts.last(); }

    /// Wraps a call result, translating a pending exception into a ScriptValue
    /// wrapping the thrown value.
    ScriptValue wrapCallResult(JSValue resultValue, const QString& callDescription);
    /// Wraps the result of a top-level evaluation, recording any thrown value as
    /// the uncaught exception.
    ScriptValue handleEvaluationResult(JSValue resultValue, const QString& description);

    JSClassID qobjectProxyClassId() const { return _qobjectProxyClassId; }
    JSClassID signalHandleClassId() const { return _signalHandleClassId; }
    JSClassID variantProxyClassId() const { return _variantProxyClassId; }

    /// Registers/unregisters a QjsSignalSlot so the engine can disconnect all
    /// signal connections (e.g. when a script is being stopped).
    void registerSignalSlot(QjsSignalSlot* slot);
    void unregisterSignalSlot(QjsSignalSlot* slot);

    struct CustomMarshal {
        ScriptEngine::MarshalFunction marshalFunc;
        ScriptEngine::DemarshalFunction demarshalFunc;
    };
    using CustomMarshalMap = QHash<int, CustomMarshal>;
    const CustomMarshalMap& customMarshalMap() const { return _customTypes; }
    const QHash<int, ScriptValue>& customPrototypes() const { return _customPrototypes; }

    std::shared_ptr<ScriptException> uncaughtExceptionShared() const { return _uncaughtException; }

private:
    void setUncaughtException(const QString& message, const QString& info = QString(), int line = -1, int column = -1,
                              const QStringList& backtrace = QStringList());
    void setUncaughtException(std::shared_ptr<ScriptException> exception);
    void setUncaughtExceptionFromValue(JSValue thrownValue, const QString& info);
    void registerSystemTypes();
    void registerQObjectProxyClass();
    void registerVariantProxyClass();
    void registerValueOnGlobal(const QString& path, const ScriptValue& value);
    /// Converts a JSValue to a QVariant of the requested type (QMetaType typeId).
    bool castValueToVariant(JSValueConst value, int destTypeId, QVariant& dest);
    /// Returns the QVariant stored in a variant-proxy JS object, if @p value is one.
    QVariant unwrapVariantProxy(JSValueConst value) const;
    void beginEvaluation();
    void endEvaluation();
    JSValue makeFunctionValue(ScriptEngine::FunctionSignature fun, int numArguments);
    static JSValue callFunctionData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic,
                                    JSValue* func_data);

    qjs::QjsEngineHandlePointer _engineHandle;
    QList<ScriptContextPointer> _contexts;
    std::shared_ptr<ScriptException> _uncaughtException;
    QString _objectName;
    QThread* _thread;
    int _processEventsInterval;

    JSClassID _qobjectProxyClassId;
    JSClassID _signalHandleClassId;
    JSClassID _variantProxyClassId;
    /// Must outlive the runtime registration, since JS_NewClass only copies the
    /// JSClassDef struct, not the exotic methods structure it points to.
    JSClassExoticMethods _qobjectProxyExotic;

    qjs::QjsValueHandlePointer _globalObjectHandle;

    CustomMarshalMap _customTypes;
    QHash<int, ScriptValue> _customPrototypes;
    QSet<QjsSignalSlot*> _signalSlots;
    QStringList _currentScriptURLs;
    int _evaluatingCounter;

    friend class ScriptValueQJSWrapper;
    friend class ScriptObjectQJSProxy;
};

ScriptEnginePointer newScriptEngineQJS(ScriptManager* manager);

#endif  // hifi_ScriptEngineQJS_h

/// @}
