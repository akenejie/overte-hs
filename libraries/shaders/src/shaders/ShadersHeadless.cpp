// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
//
// This file is part of Overte Headless-Server (overte-hs), an unofficial
// stripped-down, headless-only derivative of Overte. It is licensed under
// the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
// NOTICE in the repository root).

//
//  Minimal headless implementation of the shaders library.
//
//  The full implementation (Shaders.cpp) requires Qt resource bundles generated
//  by the scribe tooling, nlohmann json and GPU support. Headless builds have
//  none of these, but the gpu/procedural libraries still reference the shader
//  Source/Reflection API. This file provides linkable (empty) implementations.
//

#include "Shaders.h"

#include <vector>
#include <string>

namespace shader {

const std::vector<uint32_t>& startupPrograms() {
    static const std::vector<uint32_t> STARTUP_PROGRAMS;
    return STARTUP_PROGRAMS;
}

const std::vector<uint32_t>& allShaders() {
    static const std::vector<uint32_t> ALL_SHADERS;
    return ALL_SHADERS;
}

const std::vector<Dialect>& allDialects() {
    static const std::vector<Dialect> ALL_DIALECTS{ { Dialect::glsl450 } };
    return ALL_DIALECTS;
}

const std::string& dialectPath(Dialect dialect) {
    static const std::string e310esPath { "/310es/" };
    static const std::string e410Path { "/410/" };
    static const std::string e450Path { "/450/" };
    switch (dialect) {
        case Dialect::glsl310es: return e310esPath;
        case Dialect::glsl450: return e450Path;
        case Dialect::glsl410: return e410Path;
        default: break;
    }
    throw std::runtime_error("Invalid dialect");
}

const std::vector<Variant>& allVariants() {
    static const std::vector<Variant> ALL_VARIANTS{ { Variant::Mono, Variant::Stereo } };
    return ALL_VARIANTS;
}

Source& Source::operator=(const Source& other) {
    // DO NOT COPY the shader ID
    name = other.name;
    dialectSources = other.dialectSources;
    replacements = other.replacements;
    return *this;
}

String Source::getSource(Dialect dialect, Variant variant) const {
    return {};
}

const Reflection& Source::getReflection(Dialect dialect, Variant variant) const {
    static const Reflection EMPTY_REFLECTION;
    return EMPTY_REFLECTION;
}

const Source& Source::get(uint32_t shaderId) {
    static const Source EMPTY_SHADER;
    return EMPTY_SHADER;
}

void Reflection::parse(const std::string& jsonString) {
}

void Reflection::merge(const Reflection& reflection) {
}

void Reflection::updateValid() {
}

std::vector<std::string> Reflection::getNames(const LocationMap& locations) {
    std::vector<std::string> result;
    result.reserve(locations.size());
    for (const auto& entry : locations) {
        result.push_back(entry.first);
    }
    return result;
}

}  // namespace shader
