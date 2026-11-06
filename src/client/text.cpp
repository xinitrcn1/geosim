#include "text.hpp"

#include "flags.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#include FT_MULTIPLE_MASTERS_H

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace geosim::client {

namespace {

// Decodes one UTF-8 codepoint; invalid bytes become U+FFFD.
uint32_t next_codepoint(std::string_view s, std::size_t& i) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if(c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if(extra < 0) return 0xFFFD;
    uint32_t cp = c & (0x3Fu >> extra);
    for(int k = 0; k < extra; ++k) {
        if(i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3Fu);
    }
    return cp;
}

bool is_regional_indicator(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }

// Codepoints drawn from the emoji font first (pictographs and symbols).
bool prefers_emoji(uint32_t cp) {
    return cp >= 0x1F000 || (cp >= 0x2190 && cp <= 0x21FF) || (cp >= 0x2300 && cp <= 0x23FF) || (cp >= 0x2460 && cp <= 0x27BF)
           || (cp >= 0x2900 && cp <= 0x2BFF);
}

// Big-endian readers for the GSUB parser; out-of-range reads return 0.
struct be_reader {
    std::vector<uint8_t> const& d;
    [[nodiscard]] uint32_t u16(std::size_t at) const { return at + 2 <= d.size() ? (uint32_t{d[at]} << 8) | d[at + 1] : 0u; }
    [[nodiscard]] uint32_t u32(std::size_t at) const { return at + 4 <= d.size() ? (u16(at) << 16) | u16(at + 2) : 0u; }
};

// Coverage table -> list of glyphs in coverage-index order.
std::vector<uint32_t> read_coverage(be_reader const& r, std::size_t at) {
    std::vector<uint32_t> glyphs;
    const uint32_t format = r.u16(at);
    if(format == 1) {
        const uint32_t n = r.u16(at + 2);
        for(uint32_t i = 0; i < n; ++i) glyphs.push_back(r.u16(at + 4 + 2 * i));
    } else if(format == 2) {
        const uint32_t n = r.u16(at + 2);
        for(uint32_t i = 0; i < n; ++i) {
            const std::size_t rec = at + 4 + 6 * i;
            const uint32_t start = r.u16(rec), end = r.u16(rec + 2), index = r.u16(rec + 4);
            for(uint32_t g = start; g <= end && end - start < 65536; ++g) {
                if(glyphs.size() <= index + (g - start)) glyphs.resize(index + (g - start) + 1);
                glyphs[index + (g - start)] = g;
            }
        }
    }
    return glyphs;
}

} // namespace

bool text_system::init(std::string const& fonts_dir, core::diagnostics& diag) {
    if(FT_Init_FreeType(&lib_) != 0) {
        diag.fail("FT_Init_FreeType failed");
        return false;
    }
    char const* files[family_count] = {"SourceSans3-Variable.ttf", "NotoEmoji-Variable.ttf"};
    for(uint8_t f = 0; f < family_count; ++f) {
        const std::string path = fonts_dir + "/" + files[f];
        std::ifstream in(path, std::ios::binary);
        if(in) files_[f].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if(files_[f].empty()) {
            if(f == family_emoji) {
                SDL_Log("warning: emoji font %s not found; icons will render as boxes", path.c_str());
                continue;
            }
            diag.fail("cannot read font " + path);
            return false;
        }
    }
    if(face_for(family_text, static_cast<uint16_t>(font_weight::regular)) < 0) {
        diag.fail("cannot open font " + fonts_dir + "/" + files[family_text]);
        return false;
    }
    const int emoji = face_for(family_emoji, static_cast<uint16_t>(font_weight::regular));
    if(emoji >= 0) load_ligatures(faces_[static_cast<std::size_t>(emoji)].face);
    atlas_.assign(std::size_t{atlas_size} * atlas_size, 0);
    return true;
}

void text_system::shutdown() {
    for(auto& f : faces_) FT_Done_Face(f.face);
    faces_.clear();
    if(lib_) FT_Done_FreeType(lib_);
    lib_ = nullptr;
}

int text_system::face_for(family fam, uint16_t weight) {
    weight = static_cast<uint16_t>((weight + 25) / 50 * 50);
    for(std::size_t i = 0; i < faces_.size(); ++i) {
        if(faces_[i].fam == fam && faces_[i].weight == weight) return static_cast<int>(i);
    }
    if(files_[fam].empty() || faces_.size() >= 255) return -1;
    FT_Face face = nullptr;
    if(FT_New_Memory_Face(lib_, files_[fam].data(), static_cast<FT_Long>(files_[fam].size()), 0, &face) != 0) return -1;
    // Variable font: set the wght axis (clamped to its range); other axes keep defaults.
    FT_MM_Var* mm = nullptr;
    if(FT_HAS_MULTIPLE_MASTERS(face) && FT_Get_MM_Var(face, &mm) == 0) {
        std::vector<FT_Fixed> coords(mm->num_axis);
        for(FT_UInt a = 0; a < mm->num_axis; ++a) {
            coords[a] = mm->axis[a].def;
            if(mm->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
                coords[a] = std::clamp(static_cast<FT_Fixed>(weight) << 16, mm->axis[a].minimum, mm->axis[a].maximum);
            }
        }
        FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords.data());
        FT_Done_MM_Var(lib_, mm);
    }
    faces_.push_back({fam, weight, face});
    return static_cast<int>(faces_.size() - 1);
}

// Collects every two-glyph ligature of the emoji font's GSUB (lookup type 4,
// also through type 7 extensions). Feature/script selection is ignored: only
// regional-indicator pairs are ever looked up, and only flags match those.
void text_system::load_ligatures(FT_Face face) {
    if(!face || !FT_IS_SFNT(face)) return;
    FT_ULong len = 0;
    if(FT_Load_Sfnt_Table(face, TTAG_GSUB, 0, nullptr, &len) != 0 || len == 0) return;
    std::vector<uint8_t> data(len);
    if(FT_Load_Sfnt_Table(face, TTAG_GSUB, 0, data.data(), &len) != 0) return;
    be_reader r{data};

    const std::size_t lookup_list = r.u16(8);
    const uint32_t lookups = r.u16(lookup_list);
    for(uint32_t l = 0; l < lookups; ++l) {
        const std::size_t lookup = lookup_list + r.u16(lookup_list + 2 + 2 * l);
        uint32_t type = r.u16(lookup);
        const uint32_t subtables = r.u16(lookup + 4);
        for(uint32_t s = 0; s < subtables; ++s) {
            std::size_t sub = lookup + r.u16(lookup + 6 + 2 * s);
            uint32_t sub_type = type;
            if(type == 7) { // extension: format, extensionLookupType, extensionOffset32
                sub_type = r.u16(sub + 2);
                sub = sub + r.u32(sub + 4);
            }
            if(sub_type != 4 || r.u16(sub) != 1) continue;
            const auto coverage = read_coverage(r, sub + r.u16(sub + 2));
            const uint32_t sets = r.u16(sub + 4);
            for(uint32_t k = 0; k < sets && k < coverage.size(); ++k) {
                const std::size_t set = sub + r.u16(sub + 6 + 2 * k);
                const uint32_t ligs = r.u16(set);
                for(uint32_t g = 0; g < ligs; ++g) {
                    const std::size_t lig = set + r.u16(set + 2 + 2 * g);
                    if(r.u16(lig + 2) == 2) { // two components: first from coverage, second listed
                        ligatures_[{coverage[k], r.u16(lig + 4)}] = r.u16(lig);
                    }
                }
            }
        }
    }
}

float text_system::ascender(float px) const noexcept {
    FT_Face f = faces_.empty() ? nullptr : faces_[0].face;
    return f ? px * static_cast<float>(f->ascender) / static_cast<float>(f->units_per_EM) : px * 0.8f;
}

float text_system::descender(float px) const noexcept {
    FT_Face f = faces_.empty() ? nullptr : faces_[0].face;
    return f ? -px * static_cast<float>(f->descender) / static_cast<float>(f->units_per_EM) : px * 0.2f;
}

void text_system::clear_cache() {
    cache_.clear();
    std::fill(atlas_.begin(), atlas_.end(), uint8_t{0});
    shelf_x_ = 1;
    shelf_y_ = 1;
    shelf_h_ = 0;
    dirty_ = true;
}

bool text_system::pack(uint32_t w, uint32_t h, uint32_t& x, uint32_t& y) {
    if(shelf_x_ + w + 1 > atlas_size) {
        shelf_x_ = 1;
        shelf_y_ += shelf_h_ + 1;
        shelf_h_ = 0;
    }
    if(shelf_y_ + h + 1 > atlas_size || w + 2 > atlas_size) {
        return false;
    }
    x = shelf_x_;
    y = shelf_y_;
    shelf_x_ += w + 1;
    shelf_h_ = std::max(shelf_h_, h);
    return true;
}

text_system::glyph const* text_system::get_glyph(uint8_t slot, uint32_t glyph_index, uint32_t px) {
    const uint64_t key = (uint64_t{slot} << 48) | (uint64_t{px} << 32) | glyph_index;
    if(auto it = cache_.find(key); it != cache_.end()) {
        return &it->second;
    }
    FT_Face face = faces_[slot].face;
    FT_Set_Pixel_Sizes(face, 0, px);
    if(FT_Load_Glyph(face, glyph_index, FT_LOAD_TARGET_LIGHT) != 0 || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
        return nullptr;
    }
    FT_Bitmap const& bm = face->glyph->bitmap;
    glyph g{};
    g.advance = static_cast<float>(face->glyph->advance.x) / 64.0f;
    g.left = static_cast<float>(face->glyph->bitmap_left);
    g.top = static_cast<float>(face->glyph->bitmap_top);
    g.w = static_cast<float>(bm.width);
    g.h = static_cast<float>(bm.rows);
    if(bm.width > 0 && bm.rows > 0 && bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
        uint32_t x = 0, y = 0;
        if(!pack(bm.width, bm.rows, x, y)) {
            // Atlas full: start over (glyphs already emitted this frame keep
            // stale UVs for one frame, which is harmless).
            SDL_Log("glyph atlas full, clearing");
            clear_cache();
            if(!pack(bm.width, bm.rows, x, y)) return nullptr;
        }
        for(uint32_t row = 0; row < bm.rows; ++row) {
            std::memcpy(&atlas_[(std::size_t{y} + row) * atlas_size + x], bm.buffer + static_cast<std::ptrdiff_t>(row) * bm.pitch, bm.width);
        }
        const float inv = 1.0f / static_cast<float>(atlas_size);
        g.u0 = static_cast<float>(x) * inv;
        g.v0 = static_cast<float>(y) * inv;
        g.u1 = static_cast<float>(x + bm.width) * inv;
        g.v1 = static_cast<float>(y + bm.rows) * inv;
        dirty_ = true;
    } else {
        g.w = g.h = 0;
    }
    return &cache_.emplace(key, g).first->second;
}

float text_system::flag_cell_h(float px) noexcept { return std::max(4.0f, std::round(px * 0.68f)); }
float text_system::flag_cell_w(float px) noexcept { return std::round(flag_cell_h(px) * 1.5f); }

float text_system::shape(std::string_view s, float px_f, font_weight w, float x, float y, std::vector<glyph_quad>* out) {
    const auto px = static_cast<uint32_t>(std::max(1.0f, std::round(px_f)));
    const int text_slot = face_for(family_text, static_cast<uint16_t>(w));
    const int emoji_slot = face_for(family_emoji, static_cast<uint16_t>(w));
    if(text_slot < 0) return 0;
    FT_Face text = faces_[static_cast<std::size_t>(text_slot)].face;
    FT_Face emoji = emoji_slot >= 0 ? faces_[static_cast<std::size_t>(emoji_slot)].face : nullptr;
    const float x_start = x;
    std::size_t i = 0;
    while(i < s.size()) {
        uint32_t cp = next_codepoint(s, i);
        if(cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D) continue; // variation selectors, ZWJ
        if(cp >= flag_set::first_codepoint && cp <= flag_set::last_codepoint) {
            // A flag: fitted into the universal cell with its own proportions, centred on
            // the middle of the capitals, pixel-aligned.
            if(!flags_) continue;
            const uint32_t index = cp - flag_set::first_codepoint;
            const float cw = flag_cell_w(px_f), ch = flag_cell_h(px_f);
            if(out && index < flags_->size()) {
                auto const& r = flags_->gpu().flags[index];
                float fw = cw, fh = std::round(cw / r.aspect);
                if(fh > ch) {
                    fh = ch;
                    fw = std::max(1.0f, std::round(ch * r.aspect));
                }
                const float fx = std::round(x + (cw - fw) * 0.5f);
                const float fy = std::round(y - ascender(px_f) * 0.36f - fh * 0.5f);
                out->push_back({fx, fy, fx + fw, fy + fh, static_cast<float>(r.first), static_cast<float>(r.count), 0, 0, true});
            }
            x += cw + std::round(px_f * 0.2f);
            continue;
        }
        int slot = text_slot;
        uint32_t gi = 0;
        if(emoji && is_regional_indicator(cp)) {
            // Flags: a pair of regional indicators -> GSUB ligature, if the font has one.
            gi = FT_Get_Char_Index(emoji, cp);
            slot = emoji_slot;
            std::size_t j = i;
            if(j < s.size()) {
                const uint32_t cp2 = next_codepoint(s, j);
                if(is_regional_indicator(cp2)) {
                    auto it = ligatures_.find({gi, FT_Get_Char_Index(emoji, cp2)});
                    if(it != ligatures_.end()) {
                        gi = it->second;
                        i = j;
                    }
                }
            }
        } else {
            if(emoji && prefers_emoji(cp)) {
                gi = FT_Get_Char_Index(emoji, cp);
                slot = emoji_slot;
            }
            if(gi == 0) {
                slot = text_slot;
                gi = FT_Get_Char_Index(text, cp);
            }
            if(gi == 0 && emoji) {
                const uint32_t e = FT_Get_Char_Index(emoji, cp);
                if(e != 0) {
                    slot = emoji_slot;
                    gi = e;
                }
            }
        }
        glyph const* g = get_glyph(static_cast<uint8_t>(slot), gi, px);
        if(!g) continue;
        if(out && g->w > 0) {
            const float gx = std::round(x + g->left);
            const float gy = std::round(y - g->top);
            out->push_back({gx, gy, gx + g->w, gy + g->h, g->u0, g->v0, g->u1, g->v1, false});
        }
        x += g->advance;
    }
    return x - x_start;
}

} // namespace geosim::client
