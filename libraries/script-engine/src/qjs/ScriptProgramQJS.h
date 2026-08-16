//
//  ScriptProgramQJS.h
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

#ifndef hifi_ScriptProgramQJS_h
#define hifi_ScriptProgramQJS_h

#include <memory>

#include <QtCore/QString>

#include "../ScriptProgram.h"
#include "../quickjs/qjs_core.h"

class ScriptEngineQJS;

/// [QJS] Implements ScriptSyntaxCheckResult for QuickJS
class ScriptSyntaxCheckResultQJS final : public ScriptSyntaxCheckResult {
public:
    ScriptSyntaxCheckResultQJS(State state, int errorColumn = -1, int errorLine = -1,
                               const QString& errorMessage = QString(), const QString& errorBacktrace = QString());

    int errorColumnNumber() const override;
    int errorLineNumber() const override;
    QString errorMessage() const override;
    QString errorBacktrace() const override;
    State state() const override;

private:
    State _state;
    int _errorColumnNumber;
    int _errorLineNumber;
    QString _errorMessage;
    QString _errorBacktrace;
};

/// [QJS] Implements ScriptProgram for QuickJS
class ScriptProgramQJS final : public ScriptProgram {
public:
    ScriptProgramQJS(ScriptEngineQJS* engine, const QString& sourceCode, const QString& fileName);

    static ScriptProgramQJS* unwrap(ScriptProgramPointer val);

    ScriptSyntaxCheckResultPointer checkSyntax() override;
    QString fileName() const override;
    QString sourceCode() const override;

private:
    bool compile();

    ScriptEngineQJS* _engine;
    qjs::QjsEngineHandlePointer _engineHandle;
    QString _sourceCode;
    QString _fileName;
    ScriptSyntaxCheckResultPointer _compileResult;
    bool _isCompiled;
};

#endif  // hifi_ScriptProgramQJS_h

/// @}
