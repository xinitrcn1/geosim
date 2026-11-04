#include "core/csv.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>

namespace geosim::core {

std::optional<std::string> read_text_file(std::string const& path, diagnostics& diag) {
    std::ifstream f(path, std::ios::binary);
    if(!f) {
        diag.fail("cannot open " + path);
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

namespace {

std::vector<std::string> split_line(std::string_view line, std::string_view src, std::size_t lineno, diagnostics& diag) {
    std::vector<std::string> out;
    std::string field;
    bool quoted = false;
    for(std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if(quoted) {
            if(c == '"') {
                if(i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(c);
            }
        } else if(c == '"') {
            quoted = true;
        } else if(c == ',') {
            out.push_back(std::move(field));
            field.clear();
        } else if(c != '\r') {
            field.push_back(c);
        }
    }
    if(quoted) {
        diag.fail(src, lineno, "unterminated quote");
    }
    out.push_back(std::move(field));
    return out;
}

} // namespace

csv_table csv_table::parse(std::string_view text, std::string_view source_name, diagnostics& diag) {
    csv_table t;
    t.source_ = std::string(source_name);
    t.sink_ = &diag;
    std::size_t pos = 0;
    std::size_t lineno = 0;
    bool have_header = false;
    while(pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if(end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        ++lineno;
        pos = end + 1;
        if(!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if(line.empty() || line.front() == '#') {
            if(end == text.size()) {
                break;
            }
            continue;
        }
        auto fields = split_line(line, t.source_, lineno, diag);
        if(!have_header) {
            t.header_ = std::move(fields);
            have_header = true;
        } else {
            t.data_.push_back(std::move(fields));
            t.line_of_row_.push_back(lineno);
        }
        if(end == text.size()) {
            break;
        }
    }
    if(!have_header) {
        t.record(t.source_ + ": no header row");
    }
    return t;
}

csv_table csv_table::load(std::string const& path, diagnostics& diag) {
    auto text = read_text_file(path, diag);
    if(!text) {
        csv_table t;
        t.source_ = path;
        t.sink_ = &diag;
        t.error_ = "cannot open " + path;
        return t;
    }
    return parse(*text, path, diag);
}

void csv_table::record(std::string message) const {
    if(sink_) {
        sink_->fail(message);
    }
    if(error_.empty()) {
        error_ = std::move(message);
    }
}

std::optional<std::size_t> csv_table::column(std::string_view name) const noexcept {
    for(std::size_t i = 0; i < header_.size(); ++i) {
        if(header_[i] == name) {
            return i;
        }
    }
    return std::nullopt;
}

std::string_view csv_table::get(std::size_t row, std::size_t col) const noexcept {
    if(row >= data_.size() || col >= data_[row].size()) {
        return {};
    }
    return data_[row][col];
}

std::string_view csv_table::get(std::size_t row, std::string_view col) const {
    auto c = column(col);
    if(!c) {
        record(source_ + ": missing column '" + std::string(col) + "'");
        return {};
    }
    return get(row, *c);
}

std::optional<double> csv_table::number(std::size_t row, std::string_view col) const {
    auto c = column(col);
    if(!c) {
        return std::nullopt;
    }
    std::string_view s = get(row, *c);
    if(s.empty()) {
        return std::nullopt;
    }
    double v = 0.0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if(r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v)) {
        record(source_ + ":" + std::to_string(line_of_row_[row]) + ": column '" + std::string(col) + "' is not a number: '"
               + std::string(s) + "'");
        return std::nullopt;
    }
    return v;
}

double csv_table::number_or(std::size_t row, std::string_view col, double fallback) const {
    return number(row, col).value_or(fallback);
}

} // namespace geosim::core
