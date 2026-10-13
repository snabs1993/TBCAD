// Diagnostics collected while reading a DXF file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "tbcad/core/result.h"

namespace tbcad::dxf {

enum class Severity { Info, Warning, Error };

// Stable diagnostic codes. Tests and the converter's --report output rely on them.
namespace code {
inline constexpr std::string_view kBadGroupCode = "dxf.lexer.bad_group_code";
inline constexpr std::string_view kBlankGroupCode = "dxf.lexer.blank_group_code";
inline constexpr std::string_view kResync = "dxf.lexer.resync";
inline constexpr std::string_view kMissingValue = "dxf.lexer.missing_value";
inline constexpr std::string_view kBadNumber = "dxf.lexer.bad_number";
inline constexpr std::string_view kDecimalComma = "dxf.lexer.decimal_comma";
inline constexpr std::string_view kMissingEof = "dxf.lexer.missing_eof";
inline constexpr std::string_view kTruncated = "dxf.lexer.truncated";
inline constexpr std::string_view kBinary = "dxf.lexer.binary";
inline constexpr std::string_view kBom = "dxf.lexer.bom";
inline constexpr std::string_view kEncodingGuess = "dxf.encoding.guess";
inline constexpr std::string_view kEncodingUnknown = "dxf.encoding.unknown_codepage";
inline constexpr std::string_view kEncodingMismatch = "dxf.encoding.mismatch";
inline constexpr std::string_view kTooManyDiagnostics = "dxf.report.too_many";
}  // namespace code

struct Diagnostic {
    Severity severity = Severity::Warning;
    std::string code;
    std::string message;
    // 1-based line in ASCII DXF; byte offset of the group in binary DXF; 0 = unknown.
    int line = 0;
    // Entity handle, when known.
    std::optional<std::uint64_t> handle;
};

class Report {
public:
    // Past this many diagnostics further ones are only counted (a hostile or badly broken
    // file must not make the report grow without bound).
    static constexpr std::size_t kMaxDiagnostics = 5000;

    void add(Severity severity, std::string_view code, std::string message, int line = 0,
             std::optional<std::uint64_t> handle = std::nullopt);
    void info(std::string_view code, std::string message, int line = 0) {
        add(Severity::Info, code, std::move(message), line);
    }
    void warn(std::string_view code, std::string message, int line = 0,
              std::optional<std::uint64_t> handle = std::nullopt) {
        add(Severity::Warning, code, std::move(message), line, handle);
    }
    void error(std::string_view code, std::string message, int line = 0,
               std::optional<std::uint64_t> handle = std::nullopt) {
        add(Severity::Error, code, std::move(message), line, handle);
    }

    const std::vector<Diagnostic>& diagnostics() const { return items_; }
    // Count per severity, including diagnostics dropped after kMaxDiagnostics.
    std::size_t count(Severity severity) const { return counts_[static_cast<int>(severity)]; }
    bool hasErrors() const { return count(Severity::Error) > 0; }
    bool empty() const { return items_.empty(); }
    // Number of diagnostics with this code (kept ones only).
    std::size_t countCode(std::string_view code) const;

    // {"info":N,"warnings":N,"errors":N,"diagnostics":[{"severity":"warning","code":"...",
    //  "message":"...","line":12,"handle":"1F"},...]} ; always valid UTF-8 JSON.
    std::string toJson() const;

private:
    std::vector<Diagnostic> items_;
    std::size_t counts_[3] = {0, 0, 0};
};

}  // namespace tbcad::dxf
