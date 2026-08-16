//
//  ScriptValueIteratorQJS.h
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

#ifndef hifi_ScriptValueIteratorQJS_h
#define hifi_ScriptValueIteratorQJS_h

#include <memory>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include "../ScriptValue.h"
#include "../ScriptValueIterator.h"
#include "../quickjs/qjs_core.h"

class ScriptEngineQJS;

/// [QJS] Implements ScriptValueIterator for QuickJS
class ScriptValueIteratorQJS final : public ScriptValueIterator {
public:
    ScriptValueIteratorQJS(ScriptEngineQJS* engine, JSValueConst value);

    ScriptValue::PropertyFlags flags() const override;
    bool hasNext() const override;
    QString name() const override;
    void next() override;
    ScriptValue value() const override;

private:
    ScriptEngineQJS* _engine;
    qjs::QjsEngineHandlePointer _engineHandle;
    qjs::QjsValueHandlePointer _value;
    QStringList _names;
    int _index;
};

#endif  // hifi_ScriptValueIteratorQJS_h

/// @}
