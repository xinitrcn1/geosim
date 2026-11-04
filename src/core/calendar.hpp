#pragma once
// Calendar: ticks are whole days since the epoch 2020-01-01 (tick 0).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace geosim::core {

struct civil_date {
    int32_t year;
    uint32_t month; // 1..12
    uint32_t day;   // 1..31
};

// Days since 1970-01-01 (proleptic Gregorian). H. Hinnant's algorithm.
[[nodiscard]] int64_t days_from_civil(civil_date d) noexcept;
[[nodiscard]] civil_date civil_from_days(int64_t days) noexcept;

inline constexpr int64_t epoch_days_1970 = 18262; // 2020-01-01

[[nodiscard]] inline int32_t tick_from_date(civil_date d) noexcept {
    return static_cast<int32_t>(days_from_civil(d) - epoch_days_1970);
}
[[nodiscard]] inline civil_date date_from_tick(int32_t tick) noexcept {
    return civil_from_days(epoch_days_1970 + tick);
}

// Parses "YYYY-MM-DD". Returns nullopt on malformed input.
[[nodiscard]] std::optional<civil_date> parse_date(std::string_view s) noexcept;
[[nodiscard]] std::string format_date(civil_date d);

// Day of year, 0-based.
[[nodiscard]] int32_t day_of_year(civil_date d) noexcept;

} // namespace geosim::core
