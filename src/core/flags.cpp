#include "core/flags.hpp"

#include "core/csv.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace geosim::core::flags {

namespace {

constexpr char const* shape_names[] = {"field", "hstripes", "vstripes", "rect", "poly", "line", "circle",
                                       "star", "crescent", "emblem", "starring", "stargrid", "inset"};
static_assert(std::size(shape_names) == static_cast<std::size_t>(shape::count));

constexpr float pi = 3.14159265358979f;

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for(std::size_t i = 0; i <= s.size(); ++i) {
        if(i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::vector<std::string_view> tokens(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while(i < s.size()) {
        while(i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        const std::size_t start = i;
        while(i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if(i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while(!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while(!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool to_float(std::string_view s, float& v) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

// "RRGGBB" or "RRGGBBAA", optional '#'.
bool to_colour(std::string_view s, uint32_t& out) {
    s = trim(s);
    if(!s.empty() && s.front() == '#') s.remove_prefix(1);
    if(s.size() != 6 && s.size() != 8) return false;
    uint32_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if(r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    const uint32_t rgba = s.size() == 6 ? (v << 8) | 0xFFu : v;
    const uint32_t R = rgba >> 24, G = (rgba >> 16) & 0xFF, B = (rgba >> 8) & 0xFF, A = rgba & 0xFF;
    out = R | (G << 8) | (B << 16) | (A << 24);
    return true;
}

// Minimum argument counts per shape (emblem: the name is separate).
constexpr std::size_t min_args[] = {0, 0, 0, 4, 6, 5, 3, 3, 6, 3, 5, 7, 3};
static_assert(std::size(min_args) == static_cast<std::size_t>(shape::count));

} // namespace

char const* shape_name(shape s) { return shape_names[static_cast<std::size_t>(s)]; }

std::optional<std::size_t> book::find(std::string_view key) const {
    for(std::size_t i = 0; i < flags.size(); ++i) {
        if(flags[i].key == key) return i;
    }
    return std::nullopt;
}

emblem const* book::find_emblem(std::string_view name) const {
    for(auto const& e : emblems) {
        if(e.name == name) return &e;
    }
    return nullptr;
}

bool parse_emblems(std::string_view text, std::string_view source, book& out, diagnostics& diag) {
    const auto t = csv_table::parse(text, source, diag);
    if(diag.failed()) return false;
    for(std::size_t row = 0; row < t.rows(); ++row) {
        const std::string name(trim(t.get(row, "name")));
        const std::string_view kind = trim(t.get(row, "part"));
        auto fail = [&](std::string const& what) {
            diag.fail(std::string(source) + ": emblem '" + name + "' row " + std::to_string(row + 1) + ": " + what);
            return false;
        };
        emblem::part p;
        if(kind == "poly") p.k = emblem::part::kind::poly;
        else if(kind == "mirror") p.k = emblem::part::kind::mirror;
        else if(kind == "circle") p.k = emblem::part::kind::circle;
        else return fail("unknown part '" + std::string(kind) + "' (poly, mirror, circle)");
        for(auto tok : tokens(t.get(row, "points"))) {
            float v = 0;
            if(!to_float(tok, v)) return fail("not a number: '" + std::string(tok) + "'");
            p.v.push_back(v);
        }
        if(p.k == emblem::part::kind::circle ? p.v.size() < 3 : (p.v.size() % 2 != 0 || p.v.size() < (p.k == emblem::part::kind::mirror ? 4u : 6u))) {
            return fail("wrong number of coordinates");
        }
        emblem* e = nullptr;
        for(auto& x : out.emblems) {
            if(x.name == name) e = &x;
        }
        if(!e) {
            out.emblems.push_back({name, {}});
            e = &out.emblems.back();
        }
        e->parts.push_back(std::move(p));
    }
    return true;
}

bool parse_flags(std::string_view text, std::string_view source, book& out, diagnostics& diag) {
    const auto t = csv_table::parse(text, source, diag);
    if(diag.failed()) return false;
    for(std::size_t row = 0; row < t.rows(); ++row) {
        const std::string key(trim(t.get(row, "iso3")));
        const std::string_view kind = trim(t.get(row, "shape"));
        auto fail = [&](std::string const& what) {
            diag.fail(std::string(source) + ": flag " + key + " (" + std::string(kind) + "), row " + std::to_string(row + 1) + ": " + what);
            return false;
        };
        auto toks = tokens(t.get(row, "args"));
        if(kind == "flag") {
            float w = 0, h = 0;
            if(toks.size() != 2 || !to_float(toks[0], w) || !to_float(toks[1], h) || w <= 0 || h <= 0) return fail("expected 'W H' proportions");
            if(out.find(key)) return fail("defined twice");
            out.flags.push_back({key, w / h, {}});
            continue;
        }
        if(out.flags.empty() || out.flags.back().key != key) return fail("a flag starts with a 'flag' row (its proportions)");
        element e;
        bool known = false;
        for(std::size_t s = 0; s < std::size(shape_names); ++s) {
            if(kind == shape_names[s]) {
                e.kind = static_cast<shape>(s);
                known = true;
            }
        }
        if(!known) return fail("unknown shape");
        for(auto c : split(t.get(row, "colour"), '|')) {
            if(trim(c).empty() && trim(t.get(row, "colour")).empty()) break;
            uint32_t v = 0;
            if(!to_colour(c, v)) return fail("bad colour '" + std::string(c) + "' (RRGGBB or RRGGBBAA)");
            e.colours.push_back(v);
        }
        const bool stripes = e.kind == shape::hstripes || e.kind == shape::vstripes;
        if(e.kind == shape::inset ? !e.colours.empty() : !stripes && e.colours.size() != 1) {
            return fail(e.kind == shape::inset ? "an inset takes no colour" : "one colour expected");
        }
        if(e.kind == shape::emblem || e.kind == shape::inset) {
            if(toks.empty()) return fail(e.kind == shape::emblem ? "emblem name missing" : "flag key missing");
            e.name = std::string(toks.front());
            toks.erase(toks.begin());
            if(e.kind == shape::emblem && !out.find_emblem(e.name)) return fail("unknown emblem '" + e.name + "'");
            if(e.kind == shape::inset && (e.name == key || !out.find(e.name))) return fail("inset of an unknown (or not yet defined) flag '" + e.name + "'");
        }
        for(auto tok : toks) {
            float v = 0;
            if(!to_float(tok, v)) return fail("not a number: '" + std::string(tok) + "'");
            e.args.push_back(v);
        }
        if(e.args.size() < min_args[static_cast<std::size_t>(e.kind)]) return fail("too few arguments");
        if(e.kind == shape::poly && e.args.size() % 2 != 0) return fail("odd number of coordinates");
        if(stripes && !e.args.empty() && e.args.size() != e.colours.size()) return fail("one weight per colour");
        out.flags.back().elements.push_back(std::move(e));
    }
    return true;
}

bool load(std::string const& defs_dir, book& out, diagnostics& diag) {
    const auto emblems = read_text_file(defs_dir + "/flag_emblems.csv", diag);
    if(!emblems || !parse_emblems(*emblems, "flag_emblems.csv", out, diag)) return false;
    const auto flags = read_text_file(defs_dir + "/flags.csv", diag);
    return flags && parse_flags(*flags, "flags.csv", out, diag);
}

namespace {

struct lowerer {
    compiled& c;
    float aspect;
    float ox = 0, oy = 0, s = 1; // this flag's placement in the compiled one (insets): (ox, oy) + p * s
    int depth = 0;
    float clip[4] = {-1e9f, -1e9f, 1e9f, 1e9f}; // an inset's rectangle: its primitives are clipped to it

    [[nodiscard]] float tx(float x) const { return ox + x * s; }
    [[nodiscard]] float ty(float y) const { return oy + y * s; }

    // Bounds, clipped to the inset rectangle: the shader also clips each primitive to its box.
    void bound(prim& p, float x0, float y0, float x1, float y1) {
        p.box[0] = std::max(x0, clip[0]);
        p.box[1] = std::max(y0, clip[1]);
        p.box[2] = std::min(x1, clip[2]);
        p.box[3] = std::min(y1, clip[3]);
    }
    // A polygon in flag units.
    void poly(uint32_t colour, std::vector<float> const& xy) {
        if(xy.size() < 6) return;
        prim p;
        p.kind = prim::poly;
        p.colour = colour;
        p.first = static_cast<uint32_t>(c.verts.size() / 2);
        p.count = static_cast<uint32_t>(xy.size() / 2);
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for(std::size_t i = 0; i < xy.size(); i += 2) {
            const float x = tx(xy[i]), y = ty(xy[i + 1]);
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
            c.verts.push_back(x);
            c.verts.push_back(y);
        }
        bound(p, x0, y0, x1, y1);
        c.prims.push_back(p);
    }
    void box(uint32_t colour, float x0, float y0, float x1, float y1) { poly(colour, {x0, y0, x1, y0, x1, y1, x0, y1}); }
    void disc(uint32_t colour, float cx, float cy, float r, float inner) {
        prim p;
        p.kind = prim::disc;
        p.colour = colour;
        cx = tx(cx);
        cy = ty(cy);
        r *= s;
        p.a[0] = cx;
        p.a[1] = cy;
        p.a[2] = r;
        p.a[3] = inner > 0.0f ? inner * r : 0.0f;
        bound(p, cx - r, cy - r, cx + r, cy + r);
        c.prims.push_back(p);
    }
    // Star with `n` points of outer radius r (flag units), point up at angle 0 (degrees, clockwise).
    void star(uint32_t colour, float cx, float cy, float r, int n, float angle, float inner) {
        n = std::clamp(n, 3, 64);
        if(inner <= 0.0f) inner = n == 5 ? 0.381966f : 0.5f;
        std::vector<float> xy;
        for(int k = 0; k < 2 * n; ++k) {
            const float a = (angle + 180.0f * static_cast<float>(k) / static_cast<float>(n)) * pi / 180.0f;
            const float rr = k % 2 == 0 ? r : r * inner;
            xy.push_back(cx + rr * std::sin(a));
            xy.push_back(cy - rr * std::cos(a));
        }
        poly(colour, xy);
    }

    void element(core::flags::book const& b, core::flags::element const& e) {
        auto const& a = e.args;
        auto arg = [&](std::size_t i, float fallback) { return i < a.size() ? a[i] : fallback; };
        auto X = [&](float fx) { return fx * aspect; };
        const uint32_t col = e.colours.empty() ? 0xFF000000u : e.colours.front();
        switch(e.kind) {
        case shape::field: box(col, 0, 0, aspect, 1); break;
        case shape::hstripes:
        case shape::vstripes: {
            float total = 0;
            for(std::size_t i = 0; i < e.colours.size(); ++i) total += arg(i, 1.0f);
            float at = 0;
            for(std::size_t i = 0; i < e.colours.size(); ++i) {
                // Each band runs to the far edge and the next paints over it: no seams.
                const float t = at / total;
                if(e.kind == shape::hstripes) box(e.colours[i], 0, t, aspect, 1);
                else box(e.colours[i], X(t), 0, aspect, 1);
                at += a.empty() ? 1.0f : a[i];
            }
            break;
        }
        case shape::rect: box(col, X(a[0]), a[1], X(a[0] + a[2]), a[1] + a[3]); break;
        case shape::poly: {
            std::vector<float> xy;
            for(std::size_t i = 0; i + 1 < a.size(); i += 2) {
                xy.push_back(X(a[i]));
                xy.push_back(a[i + 1]);
            }
            poly(col, xy);
            break;
        }
        case shape::line: {
            const float x0 = X(a[0]), y0 = a[1], x1 = X(a[2]), y1 = a[3], t = a[4] * 0.5f;
            const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
            if(len <= 0.0f) break;
            const float nx = -dy / len * t, ny = dx / len * t;
            poly(col, {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny});
            break;
        }
        case shape::circle: disc(col, X(a[0]), a[1], a[2], arg(3, 0.0f)); break;
        case shape::star: star(col, X(a[0]), a[1], a[2], static_cast<int>(arg(3, 5.0f)), arg(4, 0.0f), arg(5, 0.0f)); break;
        case shape::crescent: {
            prim p;
            p.kind = prim::crescent;
            p.colour = col;
            const float cx = tx(X(a[0])), cy = ty(a[1]), r = a[2] * s;
            p.a[0] = cx;
            p.a[1] = cy;
            p.a[2] = r;
            p.b[0] = cx + a[3] * r;
            p.b[1] = cy + a[4] * r;
            p.b[2] = a[5] * r;
            bound(p, cx - r, cy - r, cx + r, cy + r);
            c.prims.push_back(p);
            break;
        }
        case shape::emblem: {
            emblem const* em = b.find_emblem(e.name);
            if(!em) break;
            const float cx = X(a[0]), cy = a[1], half = a[2] * 0.5f, ang = arg(3, 0.0f) * pi / 180.0f, stretch = arg(4, 1.0f);
            const float cs = std::cos(ang), sn = std::sin(ang);
            auto map = [&](float ex, float ey, float& ox, float& oy) {
                const float x = ex * half * stretch, y = ey * half;
                ox = cx + x * cs - y * sn;
                oy = cy + x * sn + y * cs;
            };
            for(auto const& part : em->parts) {
                if(part.k == emblem::part::kind::circle) {
                    float ox = 0, oy = 0;
                    map(part.v[0], part.v[1], ox, oy);
                    disc(col, ox, oy, part.v[2] * half * std::min(1.0f, std::fabs(stretch)), part.v.size() > 3 ? part.v[3] : 0.0f);
                    continue;
                }
                std::vector<float> pts = part.v;
                if(part.k == emblem::part::kind::mirror) {
                    // Left half (top centre .. bottom centre) + its reflection, back up.
                    for(std::size_t i = part.v.size(); i >= 2; i -= 2) {
                        const float mx = -part.v[i - 2], my = part.v[i - 1];
                        if(std::fabs(mx) < 1e-6f && (i == part.v.size() || i == 2)) continue; // on the axis: already there
                        pts.push_back(mx);
                        pts.push_back(my);
                    }
                }
                std::vector<float> xy;
                for(std::size_t i = 0; i + 1 < pts.size(); i += 2) {
                    float ox = 0, oy = 0;
                    map(pts[i], pts[i + 1], ox, oy);
                    xy.push_back(ox);
                    xy.push_back(oy);
                }
                poly(col, xy);
            }
            break;
        }
        case shape::starring: {
            const float cx = X(a[0]), cy = a[1], R = a[2], r = a[4];
            const int n = static_cast<int>(a[3]), points = static_cast<int>(arg(5, 5.0f));
            for(int k = 0; k < n; ++k) {
                const float t = 2.0f * pi * static_cast<float>(k) / static_cast<float>(std::max(1, n));
                star(col, cx + R * std::sin(t), cy - R * std::cos(t), r, points, 0.0f, 0.0f);
            }
            break;
        }
        case shape::stargrid: {
            const int cols = static_cast<int>(a[4]), rows = static_cast<int>(a[5]);
            const bool stagger = arg(7, 0.0f) != 0.0f;
            for(int j = 0; j < rows; ++j) {
                const bool odd = stagger && j % 2 == 1;
                const int n = odd ? cols - 1 : cols;
                for(int i = 0; i < n; ++i) {
                    const float fx = a[0] + a[2] * (static_cast<float>(i) + (odd ? 1.0f : 0.5f)) / static_cast<float>(cols);
                    const float fy = a[1] + a[3] * (static_cast<float>(j) + 0.5f) / static_cast<float>(rows);
                    star(col, X(fx), fy, a[6], 5, 0.0f, 0.0f);
                }
            }
            break;
        }
        case shape::inset: {
            const auto i = b.find(e.name);
            if(!i || depth >= 4) break;
            auto const& f = b.flags[*i];
            lowerer in{c, f.aspect, tx(X(a[0])), ty(a[1]), s * a[2], depth + 1, {}};
            in.clip[0] = std::max(clip[0], in.ox);
            in.clip[1] = std::max(clip[1], in.oy);
            in.clip[2] = std::min(clip[2], in.ox + f.aspect * in.s);
            in.clip[3] = std::min(clip[3], in.oy + in.s);
            for(auto const& x : f.elements) in.element(b, x);
            break;
        }
        case shape::count: break;
        }
    }
};

} // namespace

compiled compile(book const& b) {
    compiled c;
    for(auto const& f : b.flags) {
        compiled::range r;
        r.first = static_cast<uint32_t>(c.prims.size());
        r.aspect = f.aspect;
        lowerer low{c, f.aspect};
        for(auto const& e : f.elements) low.element(b, e);
        r.count = static_cast<uint32_t>(c.prims.size()) - r.first;
        c.flags.push_back(r);
    }
    return c;
}

} // namespace geosim::core::flags
