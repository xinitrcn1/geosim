#pragma once
// The client's flags (docs/08-ui.md#flags): the flag book (assets/defs/flags.csv and
// flag_emblems.csv) and its compiled primitives, which ui_renderer uploads to the flag
// material's buffers whenever `revision()` changes (an edited flag: change the book,
// call rebuild()).
//
// In text, a flag is one private-use codepoint, U+F0000 + its index (token()), drawn by
// text_system as a flag in a universal cell: 3:2, about cap height, the flag fitted inside
// with its own proportions (a square Swiss flag gets side margins, a 19:10 US flag fills
// the width).

#include "core/diagnostics.hpp"
#include "core/flags.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace geosim::client {

class flag_set {
public:
    static constexpr uint32_t first_codepoint = 0xF0000; // Supplementary Private Use Area-A
    static constexpr uint32_t last_codepoint = 0xFFFFD;

    // Loads both files; on failure the set stays empty (flags then render as nothing).
    bool load(std::string const& defs_dir, core::diagnostics& diag);
    // Recompiles the book after a change (bumps the revision).
    void rebuild();

    [[nodiscard]] core::flags::book& book() noexcept { return book_; }
    [[nodiscard]] core::flags::book const& book() const noexcept { return book_; }
    [[nodiscard]] core::flags::compiled const& gpu() const noexcept { return gpu_; }
    [[nodiscard]] uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::size_t size() const noexcept { return gpu_.flags.size(); }

    [[nodiscard]] std::optional<uint32_t> index(std::string_view key) const;
    // The text token of a flag: its codepoint as UTF-8 ("" if the key has no flag).
    [[nodiscard]] std::string token(std::string_view key) const;
    [[nodiscard]] static std::string token_of(uint32_t index);

private:
    core::flags::book book_;
    core::flags::compiled gpu_;
    uint64_t revision_ = 0;
};

} // namespace geosim::client
