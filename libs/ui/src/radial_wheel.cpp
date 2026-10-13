#include "tbcad/ui/radial_wheel.h"

#include <cmath>

#include "tbcad/cmd/input.h"

namespace tbcad::ui {

std::vector<std::string> defaultWheelTools() {
    return {"LINE", "POLYLINE", "CIRCLE", "RECTANGLE", "MOVE", "COPY", "TRIM", "ERASE"};
}

std::vector<std::string> parseWheelTools(std::string_view text) {
    if (cmd::trim(text).empty())
        return defaultWheelTools();
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (static_cast<int>(out.size()) < kWheelSlots) {
        std::size_t end = text.find(',', pos);
        const bool last = end == std::string_view::npos;
        if (last)
            end = text.size();
        out.push_back(cmd::toUpperAscii(cmd::trim(text.substr(pos, end - pos))));
        if (last)
            break;
        pos = end + 1;
    }
    out.resize(kWheelSlots);
    return out;
}

std::string formatWheelTools(const std::vector<std::string>& tools) {
    std::string out;
    for (int i = 0; i < kWheelSlots; ++i) {
        if (i)
            out += ',';
        if (i < static_cast<int>(tools.size()))
            out += tools[static_cast<std::size_t>(i)];
    }
    return out;
}

bool wheelShouldOpen(double heldMs, double delayMs) {
    return heldMs >= delayMs;
}

int wheelSliceAt(double dx, double dy, double innerRadius, int slots) {
    if (slots < 1 || std::hypot(dx, dy) <= innerRadius)
        return -1;
    constexpr double kTau = 6.283185307179586;
    double a = std::atan2(dx, -dy);  // 0 = up, clockwise positive
    const double width = kTau / slots;
    a += width / 2;  // slice 0 is centred on up
    a = std::fmod(a, kTau);
    if (a < 0)
        a += kTau;
    return std::min(slots - 1, static_cast<int>(a / width));
}

double wheelSliceCentre(int index, int slots) {
    return slots < 1 ? 0.0 : 6.283185307179586 * index / slots;
}

}  // namespace tbcad::ui
