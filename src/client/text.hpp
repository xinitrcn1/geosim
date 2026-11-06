#pragma once
// Text: FreeType rasterisation into one R8 glyph atlas, and "shaping-lite"
// (docs/08-ui.md#text): UTF-8 -> codepoints -> per-codepoint font fallback
// (Source Sans 3, then Noto Emoji) -> horizontal advances.
//
// Both fonts are variable fonts with a `wght` axis: one file per family is
// loaded into memory and one FT_Face per (family, weight) is instantiated on
// demand via FT_Set_Var_Design_Coordinates, so any weight 200..900 can be
// used (static fonts also work; their weight is then fixed). National flags are not
// emoji: a flag_set token (U+F0000 + index) becomes a flag quad (client/flags.hpp);
// regional indicator pairs still resolve through the emoji font's GSUB ligatures.
// No kerning, no complex scripts, no bidi: enough for the Latin UI. HarfBuzz
// can replace shape() later without touching callers.
//
// Emoji are monochrome: the atlas stores coverage only and every glyph is
// tinted by its quad colour (white icons, per the style).

#include "core/diagnostics.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace geosim::client {

// CSS-style weight on the `wght` axis. Any value is allowed (font_weight{550});
// faces are instantiated per weight rounded to a multiple of 50.
enum class font_weight : uint16_t { light = 300, regular = 400, medium = 500, semibold = 600, bold = 700 };

struct glyph_quad {
    float x0, y0, x1, y1; // pixels
    float u0, v0, u1, v1; // atlas; a flag: u0 = first primitive, v0 = primitive count
    bool flag = false;    // a flag (flag_set token): drawn with material::flag
};

class flag_set;

class text_system {
public:
    static constexpr uint32_t atlas_size = 1024;

    text_system() = default;
    text_system(text_system const&) = delete;
    text_system& operator=(text_system const&) = delete;
    ~text_system() { shutdown(); }

    // Loads SourceSans3-Variable.ttf and NotoEmoji-Variable.ttf from fonts_dir.
    // A missing emoji font is not fatal (icons then render as boxes).
    bool init(std::string const& fonts_dir, core::diagnostics& diag);
    void shutdown();

    // Lays out `utf8` with its baseline at (x, y) and appends glyph quads.
    // Returns the advance width. px is the font size in pixels.
    float shape(std::string_view utf8, float px, font_weight w, float x, float y, std::vector<glyph_quad>* out);
    float measure(std::string_view utf8, float px, font_weight w = font_weight::regular) { return shape(utf8, px, w, 0, 0, nullptr); }
    [[nodiscard]] float ascender(float px) const noexcept;  // pixels above the baseline
    [[nodiscard]] float descender(float px) const noexcept; // pixels below the baseline (positive)

    // Flags in text (flag_set tokens); without a set they take no space.
    void set_flags(flag_set const* flags) noexcept { flags_ = flags; }
    [[nodiscard]] flag_set const* flags() const noexcept { return flags_; }
    // The universal flag cell at font size px: width, height (pixels).
    [[nodiscard]] static float flag_cell_w(float px) noexcept;
    [[nodiscard]] static float flag_cell_h(float px) noexcept;

    // Drops every cached glyph (e.g. after a display scale change).
    void clear_cache();

    // CPU copy of the atlas; the renderer uploads it when dirty().
    [[nodiscard]] std::vector<uint8_t> const& atlas() const noexcept { return atlas_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void mark_uploaded() noexcept { dirty_ = false; }

    [[nodiscard]] std::size_t flag_ligatures() const noexcept { return ligatures_.size(); }

private:
    struct glyph {
        float u0, v0, u1, v1;
        float left, top;   // bearing, pixels
        float w, h;        // bitmap size, pixels
        float advance;     // pixels
    };
    enum family : uint8_t { family_text = 0, family_emoji = 1, family_count };
    struct face_slot {
        family fam;
        uint16_t weight;
        FT_Face face;
    };

    glyph const* get_glyph(uint8_t slot, uint32_t glyph_index, uint32_t px);
    // The face of `fam` at `weight` (created on first use); returns a slot index
    // or -1 if the family is not loaded.
    int face_for(family fam, uint16_t weight);
    bool pack(uint32_t w, uint32_t h, uint32_t& x, uint32_t& y);
    void load_ligatures(FT_Face emoji);

    FT_Library lib_ = nullptr;
    std::vector<unsigned char> files_[family_count]; // font file bytes (FT memory faces point into these)
    std::vector<face_slot> faces_;
    std::unordered_map<uint64_t, glyph> cache_;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ligatures_; // emoji font: (glyph, glyph) -> ligature glyph
    std::vector<uint8_t> atlas_;
    uint32_t shelf_x_ = 1, shelf_y_ = 1, shelf_h_ = 0;
    bool dirty_ = true;
    flag_set const* flags_ = nullptr;
};

} // namespace geosim::client
