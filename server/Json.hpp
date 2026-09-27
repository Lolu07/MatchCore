#pragma once
// Just enough JSON for the API: string escaping for output, and a parser for
// flat request objects whose values are strings or integers.
//
// Floats are rejected on purpose — prices cross the API as integer ticks, the
// same representation the engine uses, so no rounding can creep in.

#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace matchcore::json {

inline std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) out += c;
        }
    }
    return out + "\"";
}

inline std::string error(std::string_view msg) {
    return "{\"error\":" + quote(msg) + "}";
}

using Value  = std::variant<std::string, int64_t, std::nullptr_t>;
using Object = std::map<std::string, Value, std::less<>>;

// Parses {"key": "str" | 123 | null, ...}. Returns nullopt on anything else
// (nested values, floats, escapes other than \" and \\, trailing garbage).
inline std::optional<Object> parse_flat_object(std::string_view s) {
    size_t i = 0;
    auto skip_ws = [&] { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
    auto parse_string = [&]() -> std::optional<std::string> {
        if (i >= s.size() || s[i] != '"') return std::nullopt;
        ++i;
        std::string out;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                if (++i >= s.size() || (s[i] != '"' && s[i] != '\\')) return std::nullopt;
            }
            out += s[i++];
        }
        if (i >= s.size()) return std::nullopt;
        ++i;
        return out;
    };

    Object obj;
    skip_ws();
    if (i >= s.size() || s[i++] != '{') return std::nullopt;
    skip_ws();
    if (i < s.size() && s[i] == '}') { ++i; skip_ws(); return i == s.size() ? std::optional(obj) : std::nullopt; }

    while (true) {
        skip_ws();
        auto key = parse_string();
        if (!key) return std::nullopt;
        skip_ws();
        if (i >= s.size() || s[i++] != ':') return std::nullopt;
        skip_ws();
        if (i >= s.size()) return std::nullopt;

        if (s[i] == '"') {
            auto v = parse_string();
            if (!v) return std::nullopt;
            obj[*key] = std::move(*v);
        } else if (s.substr(i, 4) == "null") {
            i += 4;
            obj[*key] = nullptr;
        } else {
            bool neg = s[i] == '-';
            if (neg) ++i;
            if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return std::nullopt;
            uint64_t mag = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                mag = mag * 10 + static_cast<uint64_t>(s[i++] - '0');
                if (mag > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
            }
            if (i < s.size() && (s[i] == '.' || s[i] == 'e' || s[i] == 'E')) return std::nullopt;
            obj[*key] = neg ? -static_cast<int64_t>(mag) : static_cast<int64_t>(mag);
        }

        skip_ws();
        if (i >= s.size()) return std::nullopt;
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == '}') { ++i; break; }
        return std::nullopt;
    }
    skip_ws();
    if (i != s.size()) return std::nullopt;
    return obj;
}

} // namespace matchcore::json
