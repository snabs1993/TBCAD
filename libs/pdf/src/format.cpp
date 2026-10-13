#include <charconv>
#include <cmath>

#include "tbcad/pdf/pdf.h"

namespace tbcad::pdf {

std::string formatNumber(double value, int decimals) {
    if (!std::isfinite(value))
        return "0";
    // Keep within a sane range so the fixed-notation text stays short.
    constexpr double kLimit = 1e9;
    if (value > kLimit)
        value = kLimit;
    if (value < -kLimit)
        value = -kLimit;

    char buf[64];
    // to_chars is locale independent: always '.' as the decimal separator.
    auto res = std::to_chars(buf, buf + sizeof buf, value, std::chars_format::fixed, decimals);
    std::string s(buf, res.ptr);
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    if (s == "-0")
        s = "0";
    return s;
}

}  // namespace tbcad::pdf
