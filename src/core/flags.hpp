#pragma once
// National flags as data (docs/08-ui.md#flags): each flag is a list of simple vexillological
// elements in painter's order (field, stripes, rectangles, polygons, bands, discs, rings,
// stars, crescents and emblems from a shared pool), read from assets/defs/flags.csv and
// assets/defs/flag_emblems.csv. Nothing here rasterises: `compile` lowers the elements to
// a flat list of primitives (polygons, discs / rings, crescents) that the client's flag
// material evaluates per pixel on the GPU, so a flag can be edited in game (by the player or
// an agent) by changing its elements and compiling again.
//
// Coordinates in the files: x as a fraction of the flag's width, y of its height (top-left
// origin); radii and sizes as a fraction of its height, so discs stay round on any
// proportions. Compiled primitives use flag units: height 1, x in [0, aspect].

#include "core/diagnostics.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace geosim::core::flags {

enum class shape : uint8_t {
    field,    // the whole flag
    hstripes, // horizontal bands, top to bottom; colours a|b|c, args = optional weights
    vstripes, // vertical bands, hoist to fly
    rect,     // x y w h
    poly,     // x1 y1 x2 y2 ... (at least three points)
    line,     // x0 y0 x1 y1 thickness: a straight band (diagonals, saltires)
    circle,   // cx cy r [inner]: a disc, or a ring when inner (fraction of r) > 0
    star,     // cx cy r [points=5] [angle=0] [inner]: a regular star, point up at angle 0
    crescent, // cx cy r dx dy cut: the disc minus a disc of radius cut*r offset by (dx, dy)*r
    emblem,   // name cx cy size [angle=0] [stretch=1]: an emblem of the pool; negative stretch mirrors
    starring, // cx cy R n r [points=5]: n upright stars on a circle of radius R
    stargrid, // x y w h cols rows r [stagger=0]: stars in a grid; stagger: odd rows one fewer, offset
    inset,    // key x y h: another flag (by key) with its top-left at (x, y), h tall (a Union Jack canton)
    count
};
[[nodiscard]] char const* shape_name(shape s);

struct element {
    shape kind = shape::field;
    std::vector<uint32_t> colours; // RGBA8, r in the low byte (the client's pack())
    std::string name;              // emblem name; inset: the flag's key
    std::vector<float> args;
};

struct flag {
    std::string key;    // ISO3 (or any key: subnational, historical, a player's design)
    float aspect = 1.5f; // width / height
    std::vector<element> elements;
};

// An emblem: parts in a [-1, 1] box (y down), all of one colour.
struct emblem {
    std::string name;
    struct part {
        enum class kind : uint8_t { poly, mirror, circle } k = kind::poly;
        std::vector<float> v; // poly / mirror: x y pairs; circle: cx cy r [inner]
    };
    std::vector<part> parts;
};

struct book {
    std::vector<flag> flags;
    std::vector<emblem> emblems;
    [[nodiscard]] std::optional<std::size_t> find(std::string_view key) const;
    [[nodiscard]] emblem const* find_emblem(std::string_view name) const;
};

// Reads flag_emblems.csv and flags.csv from `defs_dir`; errors go to `diag` (with the line).
bool load(std::string const& defs_dir, book& out, diagnostics& diag);
// Parses one flag's rows (the same columns as flags.csv) into `out`, e.g. an edited design.
bool parse_flags(std::string_view csv_text, std::string_view source, book& out, diagnostics& diag);
bool parse_emblems(std::string_view csv_text, std::string_view source, book& out, diagnostics& diag);

// GPU primitive (std430, 64 bytes; keep in sync with shaders/flag.frag).
struct prim {
    enum : uint32_t { poly = 0, disc = 1, crescent = 2 };
    uint32_t kind = poly;
    uint32_t colour = 0;
    uint32_t first = 0, count = 0; // poly: vertices
    float a[4] = {};               // disc: cx cy r inner; crescent: cx cy r -
    float b[4] = {};               // crescent: cut cx cy r -
    float box[4] = {};             // bounds x0 y0 x1 y1 (flag units), clipped to an inset; the shader clips to it
};
static_assert(sizeof(prim) == 64);

struct compiled {
    struct range {
        uint32_t first = 0, count = 0; // primitives
        float aspect = 1.5f;
    };
    std::vector<prim> prims;
    std::vector<float> verts; // x y pairs
    std::vector<range> flags; // by book index
};
// Lowers every flag of `b` (unknown emblems were rejected by the loader and are skipped).
[[nodiscard]] compiled compile(book const& b);

} // namespace geosim::core::flags
