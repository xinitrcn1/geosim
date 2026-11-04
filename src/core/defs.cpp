#include "core/defs.hpp"

#include "core/csv.hpp"

#include <cctype>
#include <cmath>
#include <charconv>

namespace geosim::core {

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string_view trim(std::string_view s) {
    while(!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while(!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

// Removes '#' comments (to end of line), keeping newlines so line numbers survive.
std::string strip_comments(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool in_comment = false;
    for(char c : text) {
        if(c == '\n') {
            in_comment = false;
            out.push_back(c);
        } else if(c == '#') {
            in_comment = true;
        } else if(!in_comment) {
            out.push_back(c);
        }
    }
    return out;
}

struct parser {
    std::string_view text;
    std::string_view src;
    diagnostics& diag;
    std::size_t pos = 0;
    std::size_t line = 1;

    void fail(std::size_t at, std::string const& msg) { diag.fail(src, at, msg); }

    void skip_ws() {
        while(pos < text.size() && is_space(text[pos])) {
            if(text[pos] == '\n') {
                ++line;
            }
            ++pos;
        }
    }

    std::vector<def_node> parse_seq(bool nested) {
        std::vector<def_node> out;
        while(!diag.failed()) {
            skip_ws();
            if(pos >= text.size()) {
                if(nested) {
                    fail(line, "unexpected end of input (missing '}')");
                }
                return out;
            }
            if(text[pos] == '}') {
                if(!nested) {
                    fail(line, "unmatched '}'");
                }
                return out;
            }
            def_node node;
            node.line = line;
            const std::size_t key_start = pos;
            while(pos < text.size() && !is_space(text[pos]) && text[pos] != '{' && text[pos] != '}') {
                ++pos;
            }
            node.key = std::string(text.substr(key_start, pos - key_start));
            skip_ws();
            if(pos < text.size() && text[pos] == '{') {
                ++pos;
                // Find the matching brace; decide leaf vs node by whether a '{' occurs inside.
                std::size_t depth = 1;
                std::size_t scan = pos;
                std::size_t scan_line = line;
                bool has_child = false;
                while(scan < text.size() && depth > 0) {
                    if(text[scan] == '{') {
                        ++depth;
                        has_child = true;
                    } else if(text[scan] == '}') {
                        --depth;
                    } else if(text[scan] == '\n') {
                        ++scan_line;
                    }
                    if(depth > 0) {
                        ++scan;
                    }
                }
                if(depth != 0) {
                    fail(node.line, "unterminated '{' after '" + node.key + "'");
                    return out;
                }
                if(has_child) {
                    node.children = parse_seq(true);
                    skip_ws();
                    if(pos >= text.size() || text[pos] != '}') {
                        fail(line, "expected '}'");
                        return out;
                    }
                    ++pos;
                } else {
                    node.text = std::string(trim(text.substr(pos, scan - pos)));
                    pos = scan + 1;
                    line = scan_line;
                }
            }
            out.push_back(std::move(node));
        }
        return out;
    }
};

} // namespace

def_node const* def_node::find(std::string_view child_key) const noexcept {
    for(auto const& c : children) {
        if(c.key == child_key) {
            return &c;
        }
    }
    return nullptr;
}

std::string_view def_node::req(std::string_view child_key, std::string_view source, diagnostics& diag) const {
    auto c = find(child_key);
    if(!c) {
        diag.fail(source, line, "'" + key + "' is missing '" + std::string(child_key) + "{...}'");
        return {};
    }
    return c->text;
}

std::optional<double> parse_number(std::string_view s, std::string_view source, std::size_t line, diagnostics& diag) {
    double v = 0.0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if(s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v)) {
        diag.fail(source, line, "not a number: '" + std::string(s) + "'");
        return std::nullopt;
    }
    return v;
}

std::optional<double> def_node::num(std::string_view child_key, std::string_view source, diagnostics& diag) const {
    auto c = find(child_key);
    if(!c) {
        return std::nullopt;
    }
    return parse_number(c->text, source, c->line, diag);
}

double def_node::num_or(std::string_view child_key, double fallback, std::string_view source, diagnostics& diag) const {
    return num(child_key, source, diag).value_or(fallback);
}

std::vector<def_node> parse_defs(std::string_view text, std::string_view source_name, diagnostics& diag) {
    const std::string clean = strip_comments(text);
    parser p{clean, source_name, diag};
    return p.parse_seq(false);
}

std::vector<def_node> load_defs(std::string const& path, diagnostics& diag) {
    auto text = read_text_file(path, diag);
    if(!text) {
        return {};
    }
    return parse_defs(*text, path, diag);
}

} // namespace geosim::core
