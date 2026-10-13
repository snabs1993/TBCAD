#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tbcad::ui {

// The radial tool wheel: hold the right mouse button in the drawing and a ring of tools appears
// around the cursor; release over a slice to run it. This header holds the parts without any
// drawing: the slot list kept in settings and the geometry of the ring.

inline constexpr int kWheelSlots = 8;
inline constexpr double kWheelDefaultDelayMs = 250.0;

// The factory wheel, clockwise from the top.
std::vector<std::string> defaultWheelTools();

// "LINE,CIRCLE,,MOVE" -> exactly kWheelSlots entries, upper case, empty for an unused slot.
// Extra entries are dropped; missing ones are empty. Blank text gives the defaults.
std::vector<std::string> parseWheelTools(std::string_view text);

// The inverse of parseWheelTools (kWheelSlots entries, comma separated).
std::string formatWheelTools(const std::vector<std::string>& tools);

// True once the button has been held long enough for the wheel (a shorter press is a normal
// right-click and opens the context menu on release).
bool wheelShouldOpen(double heldMs, double delayMs);

// Slice under the pointer, or -1 inside the dead zone (radius `innerRadius`, release there
// cancels) or when `slots` < 1. Slice 0 is centred on straight up, then clockwise. Pointer
// offsets are in screen pixels with y pointing down.
int wheelSliceAt(double dx, double dy, double innerRadius, int slots = kWheelSlots);

// Angle in radians of the centre of slice `index` (0 = straight up, clockwise, screen y down:
// the point is (sin a, -cos a)).
double wheelSliceCentre(int index, int slots = kWheelSlots);

}  // namespace tbcad::ui
