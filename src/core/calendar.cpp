#include "core/calendar.hpp"

#include <charconv>
#include <cstdio>

namespace geosim::core {

int64_t days_from_civil(civil_date d) noexcept {
    const int64_t y = static_cast<int64_t>(d.year) - (d.month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t mp = (static_cast<int64_t>(d.month) + 9) % 12;
    const int64_t doy = (153 * mp + 2) / 5 + static_cast<int64_t>(d.day) - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

civil_date civil_from_days(int64_t z) noexcept {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    return civil_date{static_cast<int32_t>(y + (m <= 2 ? 1 : 0)), static_cast<uint32_t>(m), static_cast<uint32_t>(d)};
}

std::optional<civil_date> parse_date(std::string_view s) noexcept {
    if(s.size() != 10 || s[4] != '-' || s[7] != '-') {
        return std::nullopt;
    }
    int32_t y = 0;
    uint32_t m = 0;
    uint32_t d = 0;
    auto ok = [](std::from_chars_result r, const char* end) { return r.ec == std::errc{} && r.ptr == end; };
    if(!ok(std::from_chars(s.data(), s.data() + 4, y), s.data() + 4)
       || !ok(std::from_chars(s.data() + 5, s.data() + 7, m), s.data() + 7)
       || !ok(std::from_chars(s.data() + 8, s.data() + 10, d), s.data() + 10)) {
        return std::nullopt;
    }
    if(m < 1 || m > 12 || d < 1 || d > 31) {
        return std::nullopt;
    }
    const civil_date out{y, m, d};
    const civil_date round_trip = civil_from_days(days_from_civil(out));
    if(round_trip.month != m || round_trip.day != d) {
        return std::nullopt; // e.g. 2020-02-30
    }
    return out;
}

std::string format_date(civil_date d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", d.year, d.month, d.day);
    return buf;
}

int32_t day_of_year(civil_date d) noexcept {
    return static_cast<int32_t>(days_from_civil(d) - days_from_civil(civil_date{d.year, 1, 1}));
}

} // namespace geosim::core
