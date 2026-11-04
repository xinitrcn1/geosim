#pragma once
// Parser for the brace-delimited definition format used by assets/defs and
// assets/events (same shape as DCON's schema files, plus '#' comments):
//
//   key{ value }                     leaf: text value
//   key{ child{ value } other{ x } } node with children
//   # comment until end of line
//
// A value is parsed as children if it contains '{', otherwise it is text.

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
    // Required child text; throws with source:line if missing.
    [[nodiscard]] std::string_view req(std::string_view child_key, std::string_view source) const;
    [[nodiscard]] std::optional<double> num(std::string_view child_key, std::string_view source) const;
    [[nodiscard]] double num_or(std::string_view child_key, double fallback, std::string_view source) const;
};

// Throws std::runtime_error("source:line: message") on syntax errors.
[[nodiscard]] std::vector<def_node> parse_defs(std::string_view text, std::string_view source_name = "<memory>");
[[nodiscard]] std::vector<def_node> load_defs(std::string const& path);

[[nodiscard]] double parse_number(std::string_view s, std::string_view source, std::size_t line);

} // namespace geosim::core
