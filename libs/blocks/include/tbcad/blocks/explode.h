#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::blocks {

struct ExplodeResult {
    // New entities with a null id and fully resolved properties, ready for Transaction::add.
    std::vector<Entity> entities{};
    // Things the user should hear about ("polyline widths were dropped").
    std::vector<std::string> messages{};
};

// Hook for kinds this module does not know how to break up (Dimension, Hatch, Leader register
// theirs when those modules exist). The function returns the replacement entities with a null id.
using ExplodeFn = std::function<Result<ExplodeResult>(const Document&, const Entity&)>;

class ExplodeRegistry {
public:
    // Insert and Polyline are handled here and cannot be overridden.
    static void set(EntityKind kind, ExplodeFn fn);
    static void clear(EntityKind kind);
    static bool has(EntityKind kind);
};

// True for kinds explode() can break up right now.
bool canExplode(EntityKind kind);

// One level of EXPLODE.
//  * Insert: the block's entities transformed into the Insert's space. Layer-0 entities take the
//    Insert's layer, ByBlock colour / linetype / lineweight take the Insert's. A circle or arc
//    under non-uniform scale becomes an ellipse; nested inserts stay inserts unless the
//    non-uniform scale cannot be represented, in which case they are flattened; text under
//    non-uniform scale is scaled uniformly (message).
//  * Polyline: lines and arcs (widths are dropped, with a message).
//  * Other kinds: the registered hook, else an Unsupported error.
//  * Attributes of an Insert become plain text holding the insert's values (BURST), or with
//    `attributesAsDefinitions` set go back to attribute definitions showing their tags (EXPLODE).
Result<ExplodeResult> explode(const Document& doc, const Entity& entity,
                              bool attributesAsDefinitions = false);

// The segments of a polyline as lines and arcs; the common properties are copied.
std::vector<Entity> explodePolyline(const Entity& polylineEntity);

}  // namespace tbcad::blocks
