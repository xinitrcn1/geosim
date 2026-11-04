#pragma once
// Minimal CSV reader for asset tables.
//  - Lines starting with '#' (before the header or between rows) are comments.
//  - The first non-comment line is the header.
//  - Fields may be double-quoted; "" inside quotes is a literal quote.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace geosim::core {

class csv_table {
public:
    // Throws std::runtime_error on I/O or structural errors (message includes path:line).
    static csv_table parse(std::string_view text, std::string_view source_name = "<memory>");
    static csv_table load(std::string const& path);

    [[nodiscard]] std::size_t rows() const noexcept { return data_.size(); }
    [[nodiscard]] std::optional<std::size_t> column(std::string_view name) const noexcept;
    [[nodiscard]] bool has_column(std::string_view name) const noexcept { return column(name).has_value(); }

    // Raw field; empty string if the row is short.
    [[nodiscard]] std::string_view get(std::size_t row, std::size_t col) const noexcept;
    [[nodiscard]] std::string_view get(std::size_t row, std::string_view col) const;
    // Numeric field; nullopt if empty or the column is missing. Throws on non-numeric text.
    [[nodiscard]] std::optional<double> number(std::size_t row, std::string_view col) const;
    [[nodiscard]] double number_or(std::size_t row, std::string_view col, double fallback) const;

    [[nodiscard]] std::vector<std::string> const& header() const noexcept { return header_; }

private:
    std::string source_;
    std::vector<std::string> header_;
    std::vector<std::vector<std::string>> data_;
    std::vector<std::size_t> line_of_row_;
};

[[nodiscard]] std::string read_text_file(std::string const& path);

} // namespace geosim::core
