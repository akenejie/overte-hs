//
//  ScriptProgramQJS.cpp
//  libraries/script-engine/src/qjs
//
//  Created for Overte by tomoya on 2026-08-16.
//  Copyright 2026 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//  SPDX-License-Identifier: Apache-2.0
//

#include "ScriptProgramQJS.h"

#include <QtCore/QDebug>

#include "ScriptEngineQJS.h"
#include "ScriptEngineLoggingQJS.h"

ScriptSyntaxCheckResultQJS::ScriptSyntaxCheckResultQJS(State state, int errorColumn, int errorLine,
                                                       const QString& errorMessage, const QString& errorBacktrace) :
    _state(state),
    _errorColumnNumber(errorColumn),
    _errorLineNumber(errorLine),
    _errorMessage(errorMessage),
    _errorBacktrace(errorBacktrace) {
}

int ScriptSyntaxCheckResultQJS::errorColumnNumber() const {
    return _errorColumnNumber;
}

int ScriptSyntaxCheckResultQJS::errorLineNumber() const {
    return _errorLineNumber;
}

QString ScriptSyntaxCheckResultQJS::errorMessage() const {
    return _errorMessage;
}

QString ScriptSyntaxCheckResultQJS::errorBacktrace() const {
    return _errorBacktrace;
}

ScriptSyntaxCheckResult::State ScriptSyntaxCheckResultQJS::state() const {
    return _state;
}

ScriptProgramQJS::ScriptProgramQJS(ScriptEngineQJS* engine, const QString& sourceCode, const QString& fileName) :
    _engine(engine),
    _engineHandle(engine->engineHandle()),
    _sourceCode(sourceCode),
    _fileName(fileName),
    _isCompiled(false) {
}

ScriptProgramQJS* ScriptProgramQJS::unwrap(ScriptProgramPointer val) {
    if (!val) {
        return nullptr;
    }
    return dynamic_cast<ScriptProgramQJS*>(val.get());
}

ScriptSyntaxCheckResultPointer ScriptProgramQJS::checkSyntax() {
    if (!_isCompiled) {
        compile();
    }
    return _compileResult;
}

QString ScriptProgramQJS::fileName() const {
    return _fileName;
}

QString ScriptProgramQJS::sourceCode() const {
    return _sourceCode;
}

bool ScriptProgramQJS::compile() {
    if (_isCompiled) {
        return true;
    }
    JSContext* ctx = _engineHandle->context();
    _engineHandle->refreshStackTop();
    QByteArray sourceUtf8 = _sourceCode.toUtf8();
    QByteArray fileUtf8 = _fileName.toUtf8();
    JSValue result = JS_Eval(ctx, sourceUtf8.constData(), static_cast<size_t>(sourceUtf8.length()),
                             fileUtf8.isEmpty() ? "<script>" : fileUtf8.constData(),
                             JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx);
        QString message;
        QString backtrace;
        int line = -1;
        int column = -1;
        if (JS_IsObject(ex)) {
            JSAtom messageAtom = JS_NewAtom(ctx, "message");
            JSAtom stackAtom = JS_NewAtom(ctx, "stack");
            JSAtom lineAtom = JS_NewAtom(ctx, "lineNumber");
            JSAtom columnAtom = JS_NewAtom(ctx, "columnNumber");
            JSValue messageValue = JS_GetProperty(ctx, ex, messageAtom);
            JSValue stackValue = JS_GetProperty(ctx, ex, stackAtom);
            JSValue lineValue = JS_GetProperty(ctx, ex, lineAtom);
            JSValue columnValue = JS_GetProperty(ctx, ex, columnAtom);
            const char* messageStr = JS_ToCString(ctx, messageValue);
            const char* stackStr = JS_ToCString(ctx, stackValue);
            if (messageStr) {
                message = QString::fromUtf8(messageStr);
                JS_FreeCString(ctx, messageStr);
            }
            if (stackStr) {
                backtrace = QString::fromUtf8(stackStr);
                JS_FreeCString(ctx, stackStr);
            }
            int32_t lineNum = -1;
            int32_t columnNum = -1;
            if (JS_ToInt32(ctx, &lineNum, lineValue) < 0) {
                lineNum = -1;
            }
            if (JS_ToInt32(ctx, &columnNum, columnValue) < 0) {
                columnNum = -1;
            }
            line = lineNum;
            column = columnNum;
            JS_FreeValue(ctx, columnValue);
            JS_FreeValue(ctx, lineValue);
            JS_FreeValue(ctx, stackValue);
            JS_FreeValue(ctx, messageValue);
            JS_FreeAtom(ctx, columnAtom);
            JS_FreeAtom(ctx, lineAtom);
            JS_FreeAtom(ctx, stackAtom);
            JS_FreeAtom(ctx, messageAtom);
        }
        JS_FreeValue(ctx, ex);
        _compileResult = std::make_shared<ScriptSyntaxCheckResultQJS>(ScriptSyntaxCheckResult::Error, column, line,
                                                                      message, backtrace);
        return false;
    }
    JS_FreeValue(ctx, result);
    _compileResult = std::make_shared<ScriptSyntaxCheckResultQJS>(ScriptSyntaxCheckResult::Valid);
    _isCompiled = true;
    return true;
}
