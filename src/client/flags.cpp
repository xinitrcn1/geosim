#include "flags.hpp"

namespace geosim::client {

bool flag_set::load(std::string const& defs_dir, core::diagnostics& diag) {
    core::flags::book b;
    if(!core::flags::load(defs_dir, b, diag)) return false;
    book_ = std::move(b);
    rebuild();
    return true;
}

void flag_set::rebuild() {
    gpu_ = core::flags::compile(book_);
    ++revision_;
}

std::optional<uint32_t> flag_set::index(std::string_view key) const {
    if(auto i = book_.find(key)) return static_cast<uint32_t>(*i);
    return std::nullopt;
}

std::string flag_set::token_of(uint32_t index) {
    const uint32_t cp = first_codepoint + index;
    if(cp > last_codepoint) return {};
    std::string out;
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
    return out;
}

std::string flag_set::token(std::string_view key) const {
    if(auto i = index(key)) return token_of(*i);
    return {};
}

} // namespace geosim::client
