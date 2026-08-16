//
//  ScriptContextQJS.h
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

#ifndef hifi_ScriptContextQJS_h
#define hifi_ScriptContextQJS_h

#include <memory>

#include "../ScriptContext.h"
#include "../ScriptValue.h"
#include "../quickjs/qjs_core.h"

class ScriptEngineQJS;

/// [QJS] Implements ScriptFunctionContext for QuickJS
class ScriptFunctionContextQJS final : public ScriptFunctionContext {
public:
    ScriptFunctionContextQJS(const QString& fileName, const QString& functionName, FunctionType type, int lineNumber);

    QString fileName() const override;
    QString functionName() const override;
    FunctionType functionType() const override;
    int lineNumber() const override;

private:
    QString _fileName;
    QString _functionName;
    FunctionType _functionType;
    int _lineNumber;
};

/// [QJS] Implements ScriptContext for QuickJS
class ScriptContextQJS final : public ScriptContext {
public:
    /// Context created for the arguments of a native function call.
    /// @p argv points to the argument values, which are alive as long as the
    /// native call is in progress. Ownership of the values is NOT taken.
    ScriptContextQJS(ScriptEngineQJS* engine, JSValueConst thisObject, int argc, JSValueConst* argv, ScriptContextPointer parent);
    ~ScriptContextQJS();

    static ScriptContextQJS* unwrap(ScriptContext* val);

    int argumentCount() const override;
    ScriptValue argument(int index) const override;
    QStringList backtrace() const override;
    int currentLineNumber() const override;
    QString currentFileName() const override;
    ScriptValue callee() const override;
    ScriptEnginePointer engine() const override;
    ScriptFunctionContextPointer functionContext() const override;
    ScriptContextPointer parentContext() const override;
    ScriptValue thisObject() const override;
    ScriptValue throwError(const QString& text) override;
    ScriptValue throwValue(const ScriptValue& value) override;

    JSValueConst thisValue() const;

private:
    ScriptEngineQJS* _engine;
    qjs::QjsEngineHandlePointer _engineHandle;
    qjs::QjsValueHandlePointer _thisObject;
    int _argc;
    const JSValueConst* _argv;
    ScriptContextPointer _parentContext;
};

#endif  // hifi_ScriptContextQJS_h

/// @}
