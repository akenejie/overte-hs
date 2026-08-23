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

#include "RecurseOctreeToJSONOperator.h"
#include "EntityItemProperties.h"
#include <ScriptValue.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

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
        ScriptValue qScriptValues = _skipDefaults
            ? EntityItemNonDefaultPropertiesToScriptValue(_engine, entity->getProperties())
            : EntityItemPropertiesToScriptValue(_engine, entity->getProperties());
        propertiesMap = qScriptValues.toVariant().toMap();
    }

    if (_comma) {
        _json += ',';
    };
    _comma = true;
    _json += "\n    ";

    _json += QJsonDocument(QJsonObject::fromVariantMap(propertiesMap)).toJson(QJsonDocument::Indented);
}
