#pragma once
// Minimal CSV reader for asset tables.
//  - Lines starting with '#' (before the header or between rows) are comments.
//  - The first non-comment line is the header.
//  - Fields may be double-quoted; "" inside quotes is a literal quote.
//
// No exceptions: structural errors (unreadable file, no header, unterminated
// quote) and bad cells (non-numeric text in a numeric field, a missing
// required column) are recorded in the `diagnostics` sink given to load/parse
// and also kept on the table (`error()`), while the accessor returns an empty
// value.

#include "core/diagnostics.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace geosim::core {

class csv_table {
public:
    static csv_table parse(std::string_view text, std::string_view source_name, diagnostics& diag);
    static csv_table load(std::string const& path, diagnostics& diag);

    [[nodiscard]] std::size_t rows() const noexcept { return data_.size(); }
    [[nodiscard]] std::optional<std::size_t> column(std::string_view name) const noexcept;
    [[nodiscard]] bool has_column(std::string_view name) const noexcept { return column(name).has_value(); }

    // Raw field; empty string if the row is short.
    [[nodiscard]] std::string_view get(std::size_t row, std::size_t col) const noexcept;
    // Required column: a missing column is an error (empty result).
    [[nodiscard]] std::string_view get(std::size_t row, std::string_view col) const;
    // Numeric field; nullopt if empty or the column is missing. Non-numeric text is an error.
    [[nodiscard]] std::optional<double> number(std::size_t row, std::string_view col) const;
    [[nodiscard]] double number_or(std::size_t row, std::string_view col, double fallback) const;

    [[nodiscard]] std::vector<std::string> const& header() const noexcept { return header_; }
    [[nodiscard]] std::string const& source() const noexcept { return source_; }
    [[nodiscard]] bool failed() const noexcept { return !error_.empty(); }
    [[nodiscard]] std::string const& error() const noexcept { return error_; }

private:
    void record(std::string message) const;

    std::string source_;
    std::vector<std::string> header_;
    std::vector<std::vector<std::string>> data_;
    std::vector<std::size_t> line_of_row_;
    mutable std::string error_;
    diagnostics* sink_ = nullptr;
};

// nullopt (and an error in `diag`) if the file cannot be read.
[[nodiscard]] std::optional<std::string> read_text_file(std::string const& path, diagnostics& diag);

} // namespace geosim::core
