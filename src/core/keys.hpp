#pragma once
// Short text keys (ISO3 codes, sector keys like "csv", NPI keys like "C2")
// packed into a uint32_t so they can live in DCON properties.

#include <cstdint>
#include <string>
#include <string_view>

namespace geosim::core {

[[nodiscard]] constexpr uint32_t pack_key(std::string_view s) noexcept {
    uint32_t v = 0;
    for(std::size_t i = 0; i < 4 && i < s.size(); ++i) {
        v |= static_cast<uint32_t>(static_cast<unsigned char>(s[i])) << (8 * i);
    }
    return v;
}

[[nodiscard]] inline std::string unpack_key(uint32_t v) {
    std::string s;
    for(int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((v >> (8 * i)) & 0xFF);
        if(c == 0) {
            break;
        }
        s.push_back(c);
    }
    return s;
}

} // namespace geosim::core
