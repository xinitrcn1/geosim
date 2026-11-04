#pragma once
// Parser for the brace-delimited definition format used by assets/defs and
// assets/events (same shape as DCON's schema files, plus '#' comments):
//
//   key{ value }                     leaf: text value
//   key{ child{ value } other{ x } } node with children
//   # comment until end of line
//
// A value is parsed as children if it contains '{', otherwise it is text.
// Errors go to a `diagnostics` sink (no exceptions; see core/diagnostics.hpp).

#include "core/diagnostics.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace geosim::core {

struct def_node {
    std::string key;
    std::string text;                // trimmed raw text (empty for nodes with children)
    std::vector<def_node> children;
    std::size_t line = 0;            // 1-based line of the key, for error messages

    [[nodiscard]] def_node const* find(std::string_view child_key) const noexcept;
    // Required child text; records "source:line: ..." and returns "" if missing.
    [[nodiscard]] std::string_view req(std::string_view child_key, std::string_view source, diagnostics& diag) const;
    // nullopt if missing; records an error (and returns nullopt) if not a number.
    [[nodiscard]] std::optional<double> num(std::string_view child_key, std::string_view source, diagnostics& diag) const;
    [[nodiscard]] double num_or(std::string_view child_key, double fallback, std::string_view source, diagnostics& diag) const;
};

// Records "source:line: message" on syntax errors and returns what was parsed before it.
[[nodiscard]] std::vector<def_node> parse_defs(std::string_view text, std::string_view source_name, diagnostics& diag);
[[nodiscard]] std::vector<def_node> load_defs(std::string const& path, diagnostics& diag);

// Records an error and returns nullopt if `s` is not a finite number.
[[nodiscard]] std::optional<double> parse_number(std::string_view s, std::string_view source, std::size_t line, diagnostics& diag);

} // namespace geosim::core
