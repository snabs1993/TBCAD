#pragma once

// User coordinate systems (UCS): a named origin and rotation that typed coordinates, ortho / polar
// tracking and the grid follow. The table is stored in Header::custom["tbcad.ucs"], so it is saved
// with the drawing and changes are undoable. The current UCS is the host setting "drafting.ucs"
// (a name, or "@x,y,degrees" for an unnamed system; empty or "World" is the world system).

#include <string>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/core/core.h"

namespace tbcad::drafting {

struct Ucs {
    std::string name{};  // empty = the world system
    Vec2 origin{};       // in world coordinates
    double angle = 0;    // rotation of the X axis, radians counter-clockwise from world +X

    bool isWorld() const { return origin.x == 0 && origin.y == 0 && angle == 0; }
    Vec2 toWorld(Vec2 p) const;     // point: rotate, then move to the origin
    Vec2 fromWorld(Vec2 p) const;   // point: inverse of toWorld
    Vec2 dirToWorld(Vec2 v) const;  // vector: rotate only
    Vec2 dirFromWorld(Vec2 v) const;
};

inline constexpr const char* kUcsSettingKey = "drafting.ucs";

// All named systems of the drawing, in creation order.
std::vector<Ucs> ucsList(const Document& doc);
inline const std::string kWorldUcsName = "World";
// Valid names are 1-31 characters without tab, comma or line breaks; "World" is reserved.
Result<void> validateUcsName(const std::string& name);
// Adds or replaces the system with this name.
Result<void> saveUcs(Transaction& tx, const Ucs& ucs);
Result<void> deleteUcs(Transaction& tx, const std::string& name);

// Host setting value for a system that has no name: "@x,y,angleDegrees". currentUcs understands it.
std::string unnamedUcsSetting(const Ucs& ucs);

// The current system: the named one from the host setting, or the world system when the setting is
// empty, "World", or names a system that no longer exists.
Ucs currentUcs(const Document* doc, const cmd::IHost* host);

}  // namespace tbcad::drafting
