//
//  RecurseOctreeToJSONOperator.cpp
//  libraries/entities/src
//
//  Created by Simon Walton on Oct 11, 2018.
//  Copyright 2018 High Fidelity, Inc.
//  Copyright 2023 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//  SPDX-License-Identifier: Apache-2.0
//

//
// overte-hs modifications:
// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
// (Full AGPL text in LICENSE-AGPL-3.0.txt; see NOTICE in the repository root)

#include "RecurseOctreeToJSONOperator.h"
#include "EntityItemProperties.h"
#include <ScriptValue.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include <cstdio>
#include <exception>

static void processEntityDbg(const char* msg) {
    static FILE* file = nullptr;
    if (!file) {
        file = fopen("crashdbg.log", "a");
    }
    if (file) {
        fprintf(file, "[CRASH-DBG] processEntity: %s\n", msg);
        fflush(file);
    }
}

RecurseOctreeToJSONOperator::RecurseOctreeToJSONOperator(const OctreeElementPointer&, ScriptEngine* engine,
    QString jsonPrefix, bool skipDefaults, bool skipThoseWithBadParents):
    _engine(engine),
    _json(jsonPrefix),
    _skipDefaults(skipDefaults),
    _skipThoseWithBadParents(skipThoseWithBadParents)
{
    // Intentionally empty: we now use QJsonDocument for serialization
    // instead of calling _engine->evaluate() which crashes with
    // STATUS_STACK_BUFFER_OVERRUN on Windows due to a QuickJS bug.
}

bool RecurseOctreeToJSONOperator::postRecursion(const OctreeElementPointer& element) {
    EntityTreeElementPointer entityTreeElement = std::static_pointer_cast<EntityTreeElement>(element);

    entityTreeElement->forEachEntity([&](const EntityItemPointer& entity) { processEntity(entity); } );
    return true;
}

void RecurseOctreeToJSONOperator::processEntity(const EntityItemPointer& entity) {
    if (_skipThoseWithBadParents && !entity->isParentIDValid()) {
        return;  // we weren't able to resolve a parent from _parentID, so don't save this entity.
    }

    QVariantMap propertiesMap;
    {
        const QString id = entity->getID().toString();
        const QString typeName = EntityTypes::getEntityTypeName(entity->getType());
        processEntityDbg((QStringLiteral("start id=%1 type=%2").arg(id, typeName)).toUtf8().constData());
        try {
            ScriptValue qScriptValues = _skipDefaults
                ? EntityItemNonDefaultPropertiesToScriptValue(_engine, entity->getProperties())
                : EntityItemPropertiesToScriptValue(_engine, entity->getProperties());
            processEntityDbg("properties converted");
            propertiesMap = qScriptValues.toVariant().toMap();
            processEntityDbg("variant map built");
        } catch (const std::exception& ex) {
            processEntityDbg((QStringLiteral("EXCEPTION id=%1 type=%2 what=%3")
                                  .arg(id, typeName, QString::fromUtf8(ex.what())))
                                 .toUtf8().constData());
        } catch (...) {
            processEntityDbg((QStringLiteral("EXCEPTION-UNKNOWN id=%1 type=%2")
                                  .arg(id, typeName))
                                 .toUtf8().constData());
        }
    }

    if (_comma) {
        _json += ',';
    };
    _comma = true;
    _json += "\n    ";

    _json += QJsonDocument(QJsonObject::fromVariantMap(propertiesMap)).toJson(QJsonDocument::Indented);
}
