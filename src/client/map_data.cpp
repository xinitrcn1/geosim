#include "map_data.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace geosim::client {

namespace {

constexpr char magic[8] = {'G', 'S', 'M', 'A', 'P', '0', '1', '\0'};

struct reader {
    std::vector<uint8_t> const& bytes;
    std::size_t at = 0;
    bool ok = true;

    bool take(void* dst, std::size_t n) {
        if(!ok || at + n > bytes.size()) {
            ok = false;
            return false;
        }
        std::memcpy(dst, bytes.data() + at, n);
        at += n;
        return true;
    }
    uint32_t u32() {
        uint8_t b[4] = {};
        take(b, 4);
        return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16)
               | (static_cast<uint32_t>(b[3]) << 24);
    }
    uint16_t u16() {
        uint8_t b[2] = {};
        take(b, 2);
        return static_cast<uint16_t>(b[0] | (b[1] << 8));
    }
    float f32() {
        const uint32_t v = u32();
        float f = 0;
        std::memcpy(&f, &v, 4);
        return f;
    }
};

bool read_file(std::string const& path, std::vector<uint8_t>& out, core::diagnostics& diag) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if(!f) {
        diag.fail("cannot open " + path + " (build it with tools/pipeline/build_map.py)");
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if(size <= 0) {
        std::fclose(f);
        diag.fail(path + ": empty file");
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if(got != out.size()) {
        diag.fail(path + ": short read");
        return false;
    }
    return true;
}

// Koppen-Geiger classes 1..30 (Beck et al. 2023 legend) -> land colour (sRGB), chosen
// after the look of NASA's Blue Marble: sand deserts, khaki steppes, dark green
// rainforest and boreal forest, olive savanna and Mediterranean scrub, brown tundra,
// white ice caps.
constexpr uint32_t koppen_colour[31] = {
    0xB8A27A, // 0  none (coast fallback)
    0x2A4A1B, // 1  Af   tropical rainforest
    0x31531F, // 2  Am   monsoon
    0x66703A, // 3  Aw   savanna
    0xD6B887, // 4  BWh  hot desert
    0xC1A982, // 5  BWk  cold desert
    0xA8955E, // 6  BSh  hot steppe
    0x9F9469, // 7  BSk  cold steppe
    0x7E7A49, // 8  Csa  Mediterranean, hot summer
    0x5F6B3A, // 9  Csb
    0x56643B, // 10 Csc
    0x4F6631, // 11 Cwa
    0x566A36, // 12 Cwb
    0x5A6A3D, // 13 Cwc
    0x46632B, // 14 Cfa  humid subtropical
    0x4A692E, // 15 Cfb  oceanic
    0x5A6B46, // 16 Cfc
    0x8A8358, // 17 Dsa
    0x7E7B55, // 18 Dsb
    0x5E6446, // 19 Dsc
    0x5A5E48, // 20 Dsd
    0x58703A, // 21 Dwa
    0x4D6533, // 22 Dwb
    0x3F5530, // 23 Dwc
    0x4C5440, // 24 Dwd
    0x4F6A30, // 25 Dfa
    0x3E5A2C, // 26 Dfb
    0x33482A, // 27 Dfc  boreal
    0x454C3A, // 28 Dfd
    0x8C8771, // 29 ET   tundra
    0xEDF1F5, // 30 EF   ice cap
};

struct rgbf {
    float r, g, b;
};
rgbf unpack(uint32_t hex) {
    return {static_cast<float>((hex >> 16) & 0xFF), static_cast<float>((hex >> 8) & 0xFF), static_cast<float>(hex & 0xFF)};
}
rgbf mix(rgbf a, rgbf b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
uint8_t channel(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

// Height (m) of a land relief code (build_map.py: 128..255).
float land_height(uint8_t code) {
    if(code < 128) return 0.0f;
    const float t = static_cast<float>(code - 128) / 127.0f;
    return 8800.0f * t * t;
}

// The land colour of one terrain texel.
uint32_t land_colour(uint8_t clim, uint8_t relief, float lat_deg) {
    const float alat = std::fabs(lat_deg);
    uint8_t k = clim <= 30 ? clim : 0;
    if(k == 0 && alat > 62.0f) k = 30; // polar land without a class: ice
    rgbf c = unpack(koppen_colour[k]);
    const float h = land_height(relief);
    const bool ice = k == 30;
    const bool desert = k >= 4 && k <= 7;
    if(!ice) {
        // Bare rock above the tree line, more so where the climate is dry already.
        c = mix(c, unpack(0x8A7B66), smoothstep(2200.0f, 5200.0f, h) * (desert ? 0.35f : 0.6f));
        // Snow above a snowline that falls from ~5000 m in the subtropics to ~1600 m at 70
        // degrees, higher in dry climates (the Tibetan plateau is mostly brown, its rims white).
        const float snowline = 5000.0f - 80.0f * std::max(0.0f, alat - 28.0f) + (desert ? 600.0f : k == 29 ? 300.0f : 0.0f);
        c = mix(c, unpack(0xF1F3F6), smoothstep(snowline - 400.0f, snowline + 500.0f, h) * 0.92f);
    }
    return static_cast<uint32_t>(channel(c.r)) | (static_cast<uint32_t>(channel(c.g)) << 8) | (static_cast<uint32_t>(channel(c.b)) << 16);
}

bool inflate_section(std::vector<uint8_t> const& file, std::size_t at, uint32_t packed, uint32_t raw, std::vector<uint8_t>& out) {
    out.resize(raw);
    uLongf len = raw;
    if(uncompress(out.data(), &len, file.data() + at, packed) != Z_OK || len != raw) return false;
    return true;
}

} // namespace

int map_data::find_nation(std::string const& iso3) const noexcept {
    for(std::size_t i = 0; i < nations.size(); ++i) {
        if(iso3 == nations[i].iso3.data()) return static_cast<int>(i);
    }
    return -1;
}

bool load_map_data(std::string const& path, map_data& out, core::diagnostics& diag) {
    std::vector<uint8_t> file;
    if(!read_file(path, file, diag)) return false;
    reader rd{file};
    char m[8] = {};
    rd.take(m, 8);
    if(!rd.ok || std::memcmp(m, magic, 8) != 0) {
        diag.fail(path + ": not a GSMAP01 file");
        return false;
    }
    const uint32_t id_w = rd.u32(), id_h = rd.u32(), t_w = rd.u32(), t_h = rd.u32();
    const uint32_t nation_count = rd.u32(), region_count = rd.u32(), section_count = rd.u32();
    if(!rd.ok || id_w == 0 || id_h == 0 || t_w == 0 || t_h == 0 || id_w > 32768 || t_w > 32768 || region_count >= map_unclaimed) {
        diag.fail(path + ": bad header");
        return false;
    }
    out.nations.resize(nation_count);
    for(auto& n : out.nations) {
        rd.take(n.iso3.data(), 4);
        n.iso3[3] = '\0';
        uint8_t rgbp[4] = {};
        rd.take(rgbp, 4);
        n.r = rgbp[0];
        n.g = rgbp[1];
        n.b = rgbp[2];
        n.lat = rd.f32();
        n.lon = rd.f32();
    }
    out.regions.resize(region_count);
    for(auto& r : out.regions) {
        rd.take(r.code.data(), 12);
        r.code[12] = '\0';
        r.nation = rd.u16();
        rd.u16(); // flags (unused)
        r.lat = rd.f32();
        r.lon = rd.f32();
        if(r.nation >= nation_count) rd.ok = false;
    }
    if(!rd.ok) {
        diag.fail(path + ": truncated tables");
        return false;
    }

    std::vector<uint8_t> ids_raw, elev, clim, poly, atls;
    for(uint32_t s = 0; s < section_count; ++s) {
        char tag[5] = {};
        rd.take(tag, 4);
        const uint32_t raw = rd.u32(), packed = rd.u32();
        if(!rd.ok || rd.at + packed > file.size()) {
            diag.fail(path + ": truncated section");
            return false;
        }
        std::vector<uint8_t>* dst = nullptr;
        std::size_t want = 0;
        if(std::strcmp(tag, "IDS0") == 0) {
            dst = &ids_raw;
            want = std::size_t{id_w} * id_h * 2;
        } else if(std::strcmp(tag, "ELEV") == 0) {
            dst = &elev;
            want = std::size_t{t_w} * t_h;
        } else if(std::strcmp(tag, "CLIM") == 0) {
            dst = &clim;
            want = std::size_t{t_w} * t_h;
        } else if(std::strcmp(tag, "POLY") == 0) {
            dst = &poly;
            want = raw;
        } else if(std::strcmp(tag, "ATLS") == 0) {
            dst = &atls;
            want = raw;
        }
        if(dst) {
            if(raw != want || !inflate_section(file, rd.at, packed, raw, *dst)) {
                diag.fail(path + ": bad section " + tag);
                return false;
            }
        }
        rd.at += packed;
    }
    if(ids_raw.empty() || elev.empty() || clim.empty()) {
        diag.fail(path + ": missing IDS0 / ELEV / CLIM");
        return false;
    }
    file.clear();
    file.shrink_to_fit();

    // Terrain detail atlas: layers with box-filtered mips.
    if(atls.size() >= 8) {
        uint32_t size = 0, layers = 0;
        std::memcpy(&size, atls.data(), 4);
        std::memcpy(&layers, atls.data() + 4, 4);
        if(size >= 4 && layers > 0 && layers <= 16 && atls.size() >= 8 + std::size_t{size} * size * layers) {
            out.atlas_size = size;
            out.atlas_layers = layers;
            uint32_t levels = 0;
            for(uint32_t s2 = size; s2 >= 1; s2 >>= 1) ++levels;
            out.atlas_levels = levels;
            for(uint32_t l = 0; l < layers; ++l) {
                std::vector<uint8_t> cur(atls.begin() + 8 + static_cast<std::ptrdiff_t>(std::size_t{size} * size * l),
                                         atls.begin() + 8 + static_cast<std::ptrdiff_t>(std::size_t{size} * size * (l + 1)));
                uint32_t s2 = size;
                for(uint32_t k = 0; k < levels; ++k) {
                    out.atlas.push_back(cur);
                    if(s2 == 1) break;
                    const uint32_t h2 = s2 / 2;
                    std::vector<uint8_t> next(std::size_t{h2} * h2);
                    for(uint32_t y = 0; y < h2; ++y) {
                        for(uint32_t x = 0; x < h2; ++x) {
                            const uint32_t sum = cur[(2 * y) * s2 + 2 * x] + cur[(2 * y) * s2 + 2 * x + 1] + cur[(2 * y + 1) * s2 + 2 * x] + cur[(2 * y + 1) * s2 + 2 * x + 1];
                            next[std::size_t{y} * h2 + x] = static_cast<uint8_t>((sum + 2) / 4);
                        }
                    }
                    cur.swap(next);
                    s2 = h2;
                }
            }
        }
    }

    // Vector regions: rings of 1e-5 degree deltas -> vertices and triangle fans.
    if(poly.size() >= 8) {
        std::size_t at = 0;
        auto next = [&]() -> int32_t {
            if(at + 4 > poly.size()) return 0;
            int32_t v = 0;
            std::memcpy(&v, poly.data() + at, 4);
            at += 4;
            return v;
        };
        out.poly_tolerance_deg = static_cast<float>(next()) / 1e6f;
        const int32_t count = next();
        auto ring = [&](uint32_t code) {
            const int32_t n = next();
            if(n < 3 || at + static_cast<std::size_t>(n) * 8 > poly.size()) {
                at += static_cast<std::size_t>(std::max(0, n)) * 8;
                return;
            }
            const auto first = static_cast<uint32_t>(out.poly_vertices.size() / 4);
            out.rings.push_back({first, static_cast<uint32_t>(n), code == map_lake_code ? -1 : static_cast<int32_t>((code & 0xFFFFu) - 1)});
            int32_t x = 0, y = 0;
            for(int32_t k = 0; k < n; ++k) {
                x += next();
                y += next();
                const float lon = static_cast<float>(x) / 1e5f, lat = static_cast<float>(y) / 1e5f;
                uint32_t bits[2];
                std::memcpy(&bits[0], &lon, 4);
                std::memcpy(&bits[1], &lat, 4);
                out.poly_vertices.insert(out.poly_vertices.end(), {bits[0], bits[1], first, code});
            }
            for(int32_t k = 1; k + 1 < n; ++k) {
                out.poly_indices.insert(out.poly_indices.end(), {first, first + static_cast<uint32_t>(k), first + static_cast<uint32_t>(k) + 1});
            }
        };
        for(int32_t r = 0; r < count && r < static_cast<int32_t>(region_count); ++r) {
            const int32_t rings = next();
            for(int32_t k = 0; k < rings; ++k) ring(map_region_code(static_cast<uint32_t>(r + 1)));
        }
        const int32_t lakes = next();
        for(int32_t k = 0; k < lakes; ++k) ring(map_lake_code);
    }

    // Region IDs, coarse: the GPU keeps them only for the resolve pass's fallback
    // (land of no region, wide cracks) and the raster mode, so the full-resolution
    // raster is point-sampled down to at most 2048 cells wide at once (the 64 MB
    // full-resolution CPU copy is never built), then mips down to ~256 wide.
    {
        uint32_t step = 1;
        while(id_w / step > 2048) step *= 2;
        map_level<uint16_t> l0{id_w / step, id_h / step, {}};
        l0.texels.resize(std::size_t{l0.width} * l0.height);
        for(uint32_t y = 0; y < l0.height; ++y) {
            for(uint32_t x = 0; x < l0.width; ++x) {
                const std::size_t i = std::size_t{y * step + step / 2} * id_w + x * step + step / 2;
                uint16_t v = static_cast<uint16_t>(ids_raw[2 * i] | (ids_raw[2 * i + 1] << 8));
                if(v != map_unclaimed && v > region_count) v = map_water;
                l0.texels[std::size_t{y} * l0.width + x] = v;
            }
        }
        ids_raw.clear();
        ids_raw.shrink_to_fit();
        out.ids.push_back(std::move(l0));
        while(out.ids.back().width > 256 && out.ids.back().height > 1) {
            auto const& src = out.ids.back();
            map_level<uint16_t> dst{src.width / 2, src.height / 2, {}};
            dst.texels.resize(std::size_t{dst.width} * dst.height);
            for(uint32_t y = 0; y < dst.height; ++y) {
                for(uint32_t x = 0; x < dst.width; ++x) {
                    dst.texels[std::size_t{y} * dst.width + x] = src.texels[std::size_t{2 * y + 1} * src.width + 2 * x + 1];
                }
            }
            out.ids.push_back(std::move(dst));
        }
    }

    // Region boxes and areas from the polygons: per region, longitudes unwrapped next to
    // its first ring (regions across the antimeridian), area by the shoelace formula on
    // an equal-area-ish sphere approximation (km2; "cells" keeps the name for callers).
    {
        out.boxes.assign(region_count, map_box{});
        std::vector<double> area(region_count, 0.0);
        std::vector<float> ref(region_count, 1e9f);
        for(auto const& rg : out.rings) {
            if(rg.region < 0 || static_cast<uint32_t>(rg.region) >= region_count) continue;
            map_box& b = out.boxes[static_cast<std::size_t>(rg.region)];
            float& r0 = ref[static_cast<std::size_t>(rg.region)];
            double a = 0.0;
            float plon = 0, plat = 0;
            for(uint32_t k = 0; k < rg.count; ++k) {
                float lon = 0, lat = 0;
                std::memcpy(&lon, &out.poly_vertices[std::size_t{rg.first + k} * 4], 4);
                std::memcpy(&lat, &out.poly_vertices[std::size_t{rg.first + k} * 4 + 1], 4);
                if(r0 > 1e8f) {
                    r0 = lon;
                    b = {lon, lon, lat, lat, 0};
                }
                lon = r0 + std::remainder(lon - r0, 360.0f);
                b.lon0 = std::min(b.lon0, lon);
                b.lon1 = std::max(b.lon1, lon);
                b.lat0 = std::min(b.lat0, lat);
                b.lat1 = std::max(b.lat1, lat);
                if(k > 0) a += (static_cast<double>(plon) * lat - static_cast<double>(lon) * plat) * std::cos(0.5 * (lat + plat) * 3.14159265358979 / 180.0);
                plon = lon;
                plat = lat;
            }
            area[static_cast<std::size_t>(rg.region)] += std::fabs(0.5 * a) * 111.2 * 111.2;
        }
        for(uint32_t i = 0; i < region_count; ++i) {
            map_box& b = out.boxes[i];
            if(ref[i] > 1e8f) b = {out.regions[i].lon - 0.05f, out.regions[i].lon + 0.05f, out.regions[i].lat - 0.05f, out.regions[i].lat + 0.05f, 0};
            b.cells = static_cast<uint32_t>(std::min(4.0e9, std::round(area[i])));
        }
    }

    // Terrain colour: undo the ELEV row filter, colour each texel, then box mips.
    {
        map_level<uint32_t> l0{t_w, t_h, std::vector<uint32_t>(std::size_t{t_w} * t_h)};
        for(uint32_t y = 0; y < t_h; ++y) {
            const float lat = 90.0f - (static_cast<float>(y) + 0.5f) * 180.0f / static_cast<float>(t_h);
            uint8_t prev = 0;
            for(uint32_t x = 0; x < t_w; ++x) {
                const std::size_t i = std::size_t{y} * t_w + x;
                const auto relief = static_cast<uint8_t>(elev[i] + prev);
                prev = relief;
                l0.texels[i] = land_colour(clim[i], relief, lat) | (static_cast<uint32_t>(relief) << 24);
            }
        }
        out.terrain.push_back(std::move(l0));
        while(out.terrain.back().width > 1 && out.terrain.back().height > 1) {
            auto const& src = out.terrain.back();
            map_level<uint32_t> dst{src.width / 2, src.height / 2, {}};
            dst.texels.resize(std::size_t{dst.width} * dst.height);
            for(uint32_t y = 0; y < dst.height; ++y) {
                for(uint32_t x = 0; x < dst.width; ++x) {
                    uint32_t sum[4] = {};
                    for(uint32_t k = 0; k < 4; ++k) {
                        const uint32_t v = src.texels[std::size_t{2 * y + (k >> 1)} * src.width + 2 * x + (k & 1)];
                        for(uint32_t c = 0; c < 4; ++c) sum[c] += (v >> (8 * c)) & 0xFF;
                    }
                    uint32_t v = 0;
                    for(uint32_t c = 0; c < 4; ++c) v |= ((sum[c] + 2) / 4) << (8 * c);
                    dst.texels[std::size_t{y} * dst.width + x] = v;
                }
            }
            out.terrain.push_back(std::move(dst));
        }
    }
    return true;
}

void map_loader::start(std::string path) {
    thread_ = std::jthread([this, path = std::move(path)] {
        auto data = std::make_unique<map_data>();
        core::diagnostics diag;
        ok_ = load_map_data(path, *data, diag);
        if(ok_) data_ = std::move(data);
        else error_ = diag.error();
        done_.store(true, std::memory_order_release);
    });
}

} // namespace geosim::client
