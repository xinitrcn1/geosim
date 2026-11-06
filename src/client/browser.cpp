// The Internet window (docs/08-ui.md#internet): a browser in the style of the early
// 2000s - simple, non-draggable tabs, back / forward / reload / home, an address bar and
// a search box on the default engine, a bookmarks bar of the five most used sites, and
// text-only pages (sim/web.cpp) with the ad blocker's count in the status bar.

#include "ui_layout.hpp"

#include "style.hpp"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geosim::client {

namespace {

using steady = std::chrono::steady_clock;

// sim::web::block::kind and page::layout values (kept in step with sim/web.hpp).
enum : uint8_t { b_heading, b_text, b_headline, b_link, b_stat, b_banner, b_rule, b_search, b_result, b_post };
enum : uint8_t { l_home, l_engine, l_social, l_news, l_state, l_watchdog, l_sports, l_forum, l_business, l_tabloid, l_error,
                 l_company, l_exchange, l_torrent };

constexpr rgba link_colour = hex(0x1A0DAB);
constexpr rgba chrome_bg = hex(0xE4E1D8);   // the window chrome: warm grey
constexpr rgba chrome_dark = hex(0xC9C5BA);
constexpr rgba page_bg = hex(0xFFFFFF);

std::string const& current_url(browser_state const& b) {
    auto const& t = b.tabs[b.active];
    return t.history[t.at];
}

void navigate(browser_state& b, std::string url) {
    auto& t = b.tabs[b.active];
    if(url == t.history[t.at]) return;
    t.history.resize(t.at + 1);
    t.history.push_back(std::move(url));
    t.at = t.history.size() - 1;
    t.scroll = 0;
    b.have = false;
}

// Address-bar input: a domain goes there, anything else is a search.
std::string resolve(browser_state const& b, std::string const& text, std::string const& engine) {
    std::string s = text;
    while(!s.empty() && s.back() == ' ') s.pop_back();
    while(!s.empty() && s.front() == ' ') s.erase(s.begin());
    if(s.empty()) return current_url(b);
    if(s.rfind("about:", 0) == 0) return s;
    if(s.rfind("http://", 0) == 0) s = s.substr(7);
    if(s.find(' ') == std::string::npos && s.find('.') != std::string::npos) return s.rfind("www.", 0) == 0 ? s : "www." + s;
    return engine + "/search?q=" + s;
}

// Words of `s` wrapped into lines of width `w`.
std::vector<std::string> wrap(ui_context& ui, std::string const& s, float size, float w, font_weight fw = font_weight::regular) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush_word = [&] {
        if(word.empty()) return;
        const std::string trial = line.empty() ? word : line + " " + word;
        if(!line.empty() && ui.text_width(trial, size, fw) > w) {
            lines.push_back(line);
            line = word;
        } else {
            line = trial;
        }
        word.clear();
    };
    for(char c : s) {
        if(c == ' ') flush_word();
        else word += c;
    }
    flush_word();
    if(!line.empty()) lines.push_back(line);
    return lines;
}

// A post's text and its slang term wrapped together: lines of runs, the slang's words marked.
struct run {
    std::string s;
    bool slang;
};
std::vector<std::vector<run>> wrap_post(ui_context& ui, std::string const& text, std::string const& slang, float size, float w) {
    std::vector<std::pair<std::string, bool>> words;
    auto split = [&](std::string const& s, bool sl) {
        std::string word;
        for(char c : s) {
            if(c == ' ') {
                if(!word.empty()) words.push_back({word, sl});
                word.clear();
            } else {
                word += c;
            }
        }
        if(!word.empty()) words.push_back({word, sl});
    };
    split(text, false);
    split(slang, true);
    std::vector<std::vector<run>> lines;
    std::vector<run> line;
    std::string plain; // the line so far, for measuring
    for(auto const& [word, sl] : words) {
        const std::string trial = plain.empty() ? word : plain + " " + word;
        if(!plain.empty() && ui.text_width(trial, size) > w) {
            lines.push_back(std::move(line));
            line.clear();
            plain = word;
        } else {
            plain = trial;
        }
        if(!line.empty() && line.back().slang == sl) line.back().s += " " + word;
        else line.push_back({line.empty() ? word : " " + word, sl});
    }
    if(!line.empty()) lines.push_back(std::move(line));
    return lines;
}

rgba rgb(uint32_t c) { return hex(c & 0xFFFFFF); }

} // namespace

void browser_window(ui_context& ui, sim_runner& sim, hud_state& st, rect win) {
    browser_state& b = st.browser;
    if(b.tabs.empty()) b.tabs.push_back({});
    b.active = std::min(b.active, b.tabs.size() - 1);
    const float pad = ui.px(style::pad);
    const float small = ui.px(style::font_small);
    const rect frame = window_frame(ui, st, win, "Internet", b.have ? b.view.title : std::string("Loading…"));
    ui.set_text_backing(false);
    ui.fill(layer::overlay, frame, chrome_bg);

    // The page: asked when the tab's address changes, refreshed every few seconds (the news).
    if(b.pending.valid() && b.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        web_view v = b.pending.get();
        if(b.pending_url == current_url(b)) {
            b.view = std::move(v);
            b.have = true;
            b.tabs[b.active].title = b.view.title;
            if(!b.view.bookmarks.empty()) b.bookmarks = b.view.bookmarks;
            b.engine = b.view.engine;
            b.engine_name = b.view.engine_name;
        }
    }
    const auto now = steady::now();
    if(sim.status() == sim_status::running && !b.pending.valid()
       && (!b.have || b.view.url != current_url(b) || now - b.last > std::chrono::seconds(3))) {
        b.pending_url = current_url(b);
        b.pending = sim.request_web(b.pending_url);
        b.last = now;
    }
    const std::string engine = b.search_engine.empty() ? b.engine : b.search_engine;

    // Text input (the app routes typed keys into b.input while a field is focused).
    if(b.submit) {
        b.submit = false;
        if(b.focus == 1) navigate(b, resolve(b, b.input, engine));
        else if(b.focus == 2 && !b.input.empty()) navigate(b, engine + "/search?q=" + b.input);
        b.focus = 0;
        b.input.clear();
    }

    float y = frame.y;
    // --- tabs (simple, not draggable) ---
    {
        const float th = ui.px(26);
        const float plus_w = ui.px(26);
        const float tw = std::min(ui.px(200), (frame.w - pad * 2 - plus_w) / static_cast<float>(b.tabs.size()));
        float x = frame.x + pad;
        constexpr std::size_t no_tab = static_cast<std::size_t>(-1); // not the old count: '+' grows the list below
        std::size_t close = no_tab;
        for(std::size_t i = 0; i < b.tabs.size(); ++i) {
            const rect r{std::round(x), y + ui.px(3), std::floor(tw) - ui.px(2), th - ui.px(3)};
            const bool active = i == b.active;
            ui.fill(layer::overlay, r, active ? page_bg : chrome_dark);
            char id[40];
            std::snprintf(id, sizeof(id), "web.tab.%zu", i);
            std::string title = b.tabs[i].title.empty() ? b.tabs[i].history[b.tabs[i].at] : b.tabs[i].title;
            const float cw = b.tabs.size() > 1 ? ui.px(18) : 0.0f;
            while(title.size() > 3 && ui.text_width(title, small) > r.w - pad * 2 - cw) title.resize(title.size() - 2);
            if(!active && ui.hovered(r) && ui.input().released) {
                b.active = i;
                b.have = false;
                b.focus = 0;
            }
            ui.text_in(layer::overlay, {r.x + pad, r.y, r.w - cw, r.h}, title, small, style::flat_text);
            if(b.tabs.size() > 1) {
                std::snprintf(id, sizeof(id), "web.tabx.%zu", i);
                if(ui.flat_button(id, {r.right() - cw - ui.px(2), r.y + (r.h - ui.px(16)) * 0.5f, cw, ui.px(16)}, "✖", ui.px(10))) close = i;
            }
            x += tw;
        }
        if(ui.flat_button("web.tab.new", {x + ui.px(2), y + ui.px(4), plus_w - ui.px(4), th - ui.px(6)}, "+", small)) {
            b.tabs.push_back({});
            b.active = b.tabs.size() - 1;
            b.have = false;
        }
        if(close != no_tab) {
            b.tabs.erase(b.tabs.begin() + static_cast<std::ptrdiff_t>(close));
            if(b.active >= close && b.active > 0) --b.active;
            b.have = false;
        }
        y += th;
    }
    // --- toolbar: back, forward, reload, home, address bar, search box ---
    {
        const float th = ui.px(32), bs = ui.px(26), by = y + (th - bs) * 0.5f;
        auto& t = b.tabs[b.active];
        float x = frame.x + pad;
        if(ui.flat_button("web.back", {x, by, bs, bs}, "◀", small, false, "Back") && t.at > 0) --t.at, b.have = false;
        x += bs + ui.px(2);
        if(ui.flat_button("web.fwd", {x, by, bs, bs}, "▶", small, false, "Forward") && t.at + 1 < t.history.size()) ++t.at, b.have = false;
        x += bs + ui.px(2);
        if(ui.flat_button("web.reload", {x, by, bs, bs}, "🔄", small, false, "Reload")) b.last = {};
        x += bs + ui.px(2);
        if(ui.flat_button("web.home", {x, by, bs, bs}, "⌂", small, false, "Home")) navigate(b, "about:home");
        x += bs + pad;
        const float search_w = std::min(ui.px(220), frame.w * 0.25f);
        const rect address{x, by, frame.right() - pad - search_w - pad - x, bs};
        const rect search{address.right() + pad, by, search_w, bs};
        const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
        auto field = [&](rect r, uint8_t id, std::string const& shown, std::string const& placeholder) {
            ui.fill(layer::overlay, r, page_bg);
            ui.fill(layer::overlay, {r.x, r.bottom() - std::max(1.0f, ui.px(1)), r.w, std::max(1.0f, ui.px(1))}, b.focus == id ? style::button_active : chrome_dark);
            std::string text = b.focus == id ? b.input + (blink ? "|" : "") : shown;
            const bool ph = b.focus != id && shown.empty();
            ui.text_in(layer::overlay, {r.x + pad * 0.5f, r.y, r.w - pad, r.h}, ph ? placeholder : text, small, ph ? style::flat_dim : style::flat_text);
            if(ui.hovered(r) && ui.input().released) {
                b.focus = id;
                b.input = id == 1 ? current_url(b) : std::string{};
            }
        };
        field(address, 1, current_url(b), "Address");
        field(search, 2, {}, (b.engine_name.empty() ? std::string("Search") : b.engine_name) + (b.search_engine.empty() ? "" : " (chosen)"));
        if(ui.hovered(search)) ui.tooltip("Search with " + engine + " (the engine most used where your agents are; click an engine's search box to switch)");
        y += th;
    }
    // --- bookmarks: the five most used sites worldwide ---
    {
        const float bh = ui.px(22);
        float x = frame.x + pad;
        ui.text_in(layer::overlay, {x, y, ui.px(80), bh}, "⭐ Most visited", small, style::flat_dim);
        x += ui.text_width("⭐ Most visited", small) + pad;
        for(std::size_t i = 0; i < b.bookmarks.size(); ++i) {
            const float w = ui.text_width(b.bookmarks[i].name, small) + pad * 1.5f;
            if(x + w > frame.right() - pad) break;
            char id[32];
            std::snprintf(id, sizeof(id), "web.bm.%zu", i);
            if(ui.flat_button(id, {x, y + ui.px(1), w, bh - ui.px(2)}, b.bookmarks[i].name, small, false, b.bookmarks[i].domain)) navigate(b, b.bookmarks[i].domain);
            x += w + ui.px(3);
        }
        y += bh + ui.px(2);
    }
    // --- the page ---
    const float status_h = ui.px(18);
    const rect content{frame.x + pad, y, frame.w - pad * 2, frame.bottom() - y - status_h - pad * 0.5f};
    ui.fill(layer::overlay, content, page_bg);
    auto& tab = b.tabs[b.active];
    if(!b.have || b.view.url != current_url(b)) {
        ui.text_in(layer::overlay, content, sim.status() == sim_status::running ? "Loading…" : "No connection", ui.px(14), style::flat_dim, align::centre);
    } else {
        web_view const& v = b.view;
        const float cpad = ui.px(14);
        const float cx = content.x + cpad, cw = content.w - cpad * 2;
        const float body = ui.px(12), head = ui.px(15);
        const rgba accent = rgb(v.accent);
        // Lay out the page into draw items, then draw the visible ones (scrolling).
        struct item {
            float h;
            uint8_t kind;
            std::vector<std::string> lines;
            web_block const* b;
            std::vector<std::vector<run>> runs = {}; // posts
            float indent = 0;                        // posts: replies sit under their post
            bool open = false;                       // posts: replies shown
        };
        std::vector<item> items;
        float total = 0;
        auto thread_key = [](web_block const& bl) { return bl.author + '\x1f' + bl.text; };
        bool thread_open = false; // the replies of the last post without `reply`
        // Masthead.
        const bool home = v.style == l_home, error = v.style == l_error;
        const float mast = home ? ui.px(70) : error ? ui.px(10) : ui.px(48);
        total += mast;
        for(auto const& bl : v.blocks) {
            item it{0, bl.kind, {}, &bl};
            switch(bl.kind) {
            case b_heading: it.h = ui.px(30); break;
            case b_text: it.lines = wrap(ui, bl.text, body, cw); it.h = ui.px(17) * static_cast<float>(it.lines.size()) + ui.px(4); break;
            case b_headline:
                it.lines = wrap(ui, bl.text, ui.px(13), cw - ui.px(14), font_weight::semibold);
                it.h = ui.px(18) * static_cast<float>(it.lines.size()) + (bl.sub.empty() ? ui.px(4) : ui.px(18));
                break;
            case b_link: it.h = ui.px(34); break;
            case b_result:
                it.lines = wrap(ui, bl.sub, ui.px(11), cw);
                it.h = ui.px(20) + ui.px(15) * static_cast<float>(std::min<std::size_t>(it.lines.size(), 2)) + ui.px(6);
                break;
            case b_stat: it.h = ui.px(20); break;
            case b_banner: it.h = ui.px(error ? 60 : 30); break;
            case b_rule: it.h = ui.px(12); break;
            case b_search: it.h = ui.px(home ? 50 : 42); break;
            case b_post:
                if(bl.reply) {
                    if(!thread_open) continue; // packed under its post
                    it.indent = ui.px(36);
                } else {
                    const auto key = thread_key(bl);
                    thread_open = std::find(b.open_threads.begin(), b.open_threads.end(), key) != b.open_threads.end();
                    it.open = thread_open;
                }
                it.runs = wrap_post(ui, bl.text, bl.slang, body, cw - ui.px(44) - it.indent);
                it.h = ui.px(22) + ui.px(17) * static_cast<float>(it.runs.size()) + ui.px(24) + (bl.replies > 0 ? ui.px(18) : 0.0f);
                break;
            default: it.h = ui.px(18); break;
            }
            total += it.h;
            items.push_back(std::move(it));
        }
        const float max_scroll = std::max(0.0f, total - content.h + ui.px(20));
        tab.scroll = std::clamp(tab.scroll - ui.take_wheel(content) * ui.px(60), 0.0f, max_scroll);
        float py = content.y - tab.scroll;
        auto visible = [&](float top, float h) { return top + h > content.y && top < content.bottom(); };
        // Masthead: the site's name on its colour (the start page: a logo line).
        if(home) {
            if(visible(py, mast)) {
                ui.text_in(layer::overlay, {content.x, py + ui.px(12), content.w, ui.px(34)}, "Start Page", ui.px(26), hex(0xE66000), align::centre, font_weight::semibold);
                ui.text_in(layer::overlay, {content.x, py + ui.px(44), content.w, ui.px(18)}, "Your gateway to the world's web. Advertisements are blocked.", small, style::flat_dim,
                           align::centre);
            }
        } else if(!error && visible(py, mast)) {
            const rect m{content.x, std::max(content.y, py), content.w, std::min(mast, py + mast - content.y) - ui.px(6)};
            if(m.h > 0) {
                ui.fill(layer::overlay, m, accent);
                if(py >= content.y) {
                    ui.text_in(layer::overlay, {cx, py, cw, ui.px(30)}, v.host, ui.px(19), style::text, align::left, font_weight::semibold);
                    ui.text_in(layer::overlay, {cx, py + ui.px(24), cw, ui.px(16)}, v.category + (v.owner.empty() ? "" : " · run by " + v.owner), small, hex(0xFFFFFF, 0.8f));
                }
            }
        }
        py += mast;
        for(auto const& it : items) {
            web_block const& bl = *it.b;
            if(!visible(py, it.h)) {
                py += it.h;
                continue;
            }
            const bool inside = py >= content.y && py + it.h <= content.bottom() + ui.px(2);
            switch(it.kind) {
            case b_heading:
                if(inside) {
                    ui.text_in(layer::overlay, {cx, py + ui.px(6), cw, ui.px(20)}, bl.text, head, home ? hex(0x2B4C7E) : accent, align::left, font_weight::semibold);
                    ui.fill(layer::overlay, {cx, py + ui.px(27), cw, std::max(1.0f, ui.px(1))}, style::flat_rule);
                }
                break;
            case b_text:
                for(std::size_t k = 0; k < it.lines.size(); ++k) {
                    const float ly = py + ui.px(17) * static_cast<float>(k);
                    if(ly >= content.y && ly + ui.px(17) <= content.bottom()) ui.text_in(layer::overlay, {cx, ly, cw, ui.px(17)}, it.lines[k], body, style::flat_text);
                }
                break;
            case b_headline: {
                const rgba dot = bl.tone < 0 ? style::flat_bad : bl.tone > 0 ? style::flat_good : style::flat_dim;
                if(py >= content.y) ui.text_in(layer::overlay, {cx, py, ui.px(12), ui.px(18)}, "•", ui.px(14), dot);
                for(std::size_t k = 0; k < it.lines.size(); ++k) {
                    const float ly = py + ui.px(18) * static_cast<float>(k);
                    if(ly >= content.y && ly + ui.px(18) <= content.bottom())
                        ui.text_in(layer::overlay, {cx + ui.px(14), ly, cw - ui.px(14), ui.px(18)}, it.lines[k], ui.px(13), bl.url.empty() ? style::flat_text : link_colour, align::left, font_weight::semibold);
                }
                const float sy = py + ui.px(18) * static_cast<float>(it.lines.size());
                if(!bl.sub.empty() && sy >= content.y && sy + ui.px(16) <= content.bottom()) ui.text_in(layer::overlay, {cx + ui.px(14), sy, cw, ui.px(16)}, bl.sub, ui.px(11), style::flat_dim);
                const rect hit{cx, py, cw, it.h};
                if(!bl.url.empty() && inside && ui.hovered(hit) && ui.input().released) navigate(b, bl.url);
                break;
            }
            case b_link:
            case b_result: {
                const rect hit{cx, py, cw, it.h - ui.px(4)};
                const bool hov = inside && ui.hovered(hit);
                if(py >= content.y) {
                    ui.text_in(layer::overlay, {cx, py, cw, ui.px(20)}, bl.text, ui.px(13), link_colour, align::left, hov ? font_weight::semibold : font_weight::regular);
                    if(hov) ui.fill(layer::overlay, {cx, py + ui.px(18), ui.text_width(bl.text, ui.px(13), font_weight::semibold), std::max(1.0f, ui.px(1))}, link_colour);
                }
                if(it.kind == b_link) {
                    if(py + ui.px(32) <= content.bottom() && py >= content.y) ui.text_in(layer::overlay, {cx, py + ui.px(17), cw, ui.px(14)}, bl.sub, ui.px(11), hex(0x006621));
                } else {
                    for(std::size_t k = 0; k < it.lines.size() && k < 2; ++k) {
                        const float ly = py + ui.px(20) + ui.px(15) * static_cast<float>(k);
                        if(ly >= content.y && ly + ui.px(15) <= content.bottom()) ui.text_in(layer::overlay, {cx, ly, cw, ui.px(15)}, it.lines[k], ui.px(11), k == 0 ? hex(0x006621) : style::flat_dim);
                    }
                }
                if(hov) {
                    ui.tooltip(bl.url);
                    if(ui.input().released) navigate(b, bl.url);
                }
                break;
            }
            case b_stat:
                if(inside) {
                    ui.text_in(layer::overlay, {cx, py, cw * 0.6f, ui.px(20)}, bl.text, body, style::flat_text);
                    ui.text_in(layer::overlay, {cx + cw * 0.6f, py, cw * 0.4f, ui.px(20)}, bl.sub, body, style::flat_text, align::right, font_weight::semibold);
                    ui.fill(layer::overlay, {cx, py + ui.px(19), cw, std::max(1.0f, ui.px(1))}, hex(0xEEF0F3));
                }
                break;
            case b_banner:
                if(inside) {
                    const rect r{cx, py + ui.px(3), cw, it.h - ui.px(6)};
                    ui.fill(layer::overlay, r, bl.tone < 0 ? hex(0xFBE3E1) : hex(0xFFF6D5));
                    ui.text_in(layer::overlay, {r.x + pad, r.y, r.w - pad * 2, r.h}, bl.text, error ? ui.px(22) : body, bl.tone < 0 ? style::flat_bad : style::flat_text,
                               error ? align::left : align::left, font_weight::semibold);
                }
                break;
            case b_rule:
                if(inside) ui.fill(layer::overlay, {cx, py + ui.px(6), cw, std::max(1.0f, ui.px(1))}, style::flat_rule);
                break;
            case b_search: {
                if(!inside) break;
                // The engine's own search box: focusing it searches with that engine.
                const float bh = ui.px(home ? 34 : 28);
                const float w = std::min(cw, ui.px(home ? 520 : 420));
                const rect r{home ? cx + (cw - w) * 0.5f : cx, py + ui.px(6), w, bh};
                ui.fill(layer::overlay, r, hex(0xF5F7FA));
                ui.fill(layer::overlay, {r.x, r.bottom() - std::max(1.0f, ui.px(1)), r.w, std::max(1.0f, ui.px(1))}, chrome_dark);
                const bool mine = b.focus == 2 && (bl.url == engine);
                const std::string shown = mine ? b.input + ((SDL_GetTicks() / 500) % 2 == 0 ? "|" : "") : bl.text + " (" + bl.url + ")";
                ui.text_in(layer::overlay, {r.x + pad, r.y, r.w - pad * 2, r.h}, shown, home ? ui.px(14) : body, mine ? style::flat_text : style::flat_dim);
                if(ui.hovered(r) && ui.input().released) {
                    b.search_engine = bl.url == b.engine ? std::string{} : bl.url;
                    b.focus = 2;
                    b.input.clear();
                }
                break;
            }
            case b_post: {
                // A post: the default profile picture (a grey disc with a silhouette), the
                // poster's flag and name, the text (slang in blue; hovering gives its meaning),
                // then likes and age, and its replies behind "View replies (N)".
                const float pcx = cx + it.indent, pcw = cw - it.indent;
                const float av = ui.px(32), tx = pcx + ui.px(42);
                if(py >= content.y && py + av <= content.bottom()) {
                    ui.fill(layer::overlay, {pcx, py + ui.px(4), av, av}, hex(0xD6DBE1));
                    ui.text_in(layer::overlay, {pcx, py + ui.px(4), av, av}, "👤", ui.px(18), hex(0x8A939E), align::centre);
                }
                if(py >= content.y && py + ui.px(20) <= content.bottom()) {
                    float nx = tx;
                    if(!bl.flag.empty()) nx += ui.text_in(layer::overlay, {nx, py + ui.px(2), pcw, ui.px(18)}, bl.flag, small, style::flat_text) + ui.px(5);
                    ui.text_in(layer::overlay, {nx, py + ui.px(2), pcw, ui.px(18)}, bl.author, small, accent, align::left, font_weight::semibold);
                }
                for(std::size_t k = 0; k < it.runs.size(); ++k) {
                    const float ly = py + ui.px(22) + ui.px(17) * static_cast<float>(k);
                    if(ly < content.y || ly + ui.px(17) > content.bottom()) continue;
                    float rx = tx;
                    for(auto const& r : it.runs[k]) {
                        const float rw = ui.text_width(r.s, body);
                        ui.text_in(layer::overlay, {rx, ly, rw + ui.px(2), ui.px(17)}, r.s, body, r.slang ? link_colour : style::flat_text);
                        if(r.slang && !bl.gloss.empty() && ui.hovered({rx, ly, rw, ui.px(17)})) ui.tooltip("\u201C" + bl.slang + "\u201D: " + bl.gloss);
                        rx += rw;
                    }
                }
                const float sy = py + ui.px(22) + ui.px(17) * static_cast<float>(it.runs.size()) + ui.px(2);
                if(sy >= content.y && sy + ui.px(16) <= content.bottom()) ui.text_in(layer::overlay, {tx, sy, pcw, ui.px(16)}, "❤ " + bl.sub, ui.px(11), style::flat_dim);
                if(bl.replies > 0) {
                    const float ry = sy + ui.px(17);
                    char label[48];
                    std::snprintf(label, sizeof(label), it.open ? "Hide replies (%d)" : "View replies (%d)", bl.replies);
                    const rect hit{tx, ry, ui.text_width(label, ui.px(11), font_weight::semibold), ui.px(16)};
                    if(ry >= content.y && ry + ui.px(16) <= content.bottom()) {
                        const bool hov = ui.hovered(hit);
                        ui.text_in(layer::overlay, {tx, ry, pcw, ui.px(16)}, label, ui.px(11), link_colour, align::left, hov ? font_weight::semibold : font_weight::regular);
                        if(hov && ui.input().released) {
                            const auto key = thread_key(bl);
                            auto& open = b.open_threads;
                            if(auto f = std::find(open.begin(), open.end(), key); f != open.end()) open.erase(f);
                            else open.push_back(key);
                            if(open.size() > 64) open.erase(open.begin()); // the oldest first
                        }
                    }
                }
                if(inside) ui.fill(layer::overlay, {pcx, py + it.h - std::max(1.0f, ui.px(1)), pcw, std::max(1.0f, ui.px(1))}, style::flat_rule);
                break;
            }
            default: break;
            }
            py += it.h;
        }
        if(max_scroll > 0.0f) {
            // A thin scroll indicator.
            const float track = content.h, bar = std::max(ui.px(20), track * content.h / (total + ui.px(20)));
            const float at = content.y + (track - bar) * (tab.scroll / max_scroll);
            ui.fill(layer::overlay, {content.right() - ui.px(4), at, ui.px(3), bar}, chrome_dark);
        }
    }
    // --- status bar ---
    {
        const float sy = frame.bottom() - status_h;
        const std::string left = !b.have ? "Connecting…" : b.view.style == l_error ? "Done (with errors)" : "Done";
        ui.text_in(layer::overlay, {frame.x + pad, sy, frame.w * 0.5f, status_h}, left, ui.px(11), style::flat_dim);
        if(b.have && b.view.ads_blocked > 0) {
            char ads[48];
            std::snprintf(ads, sizeof(ads), "🛡 %d ad%s blocked", b.view.ads_blocked, b.view.ads_blocked == 1 ? "" : "s");
            ui.text_in(layer::overlay, {frame.x + pad, sy, frame.w - pad * 2, status_h}, ads, ui.px(11), style::flat_dim, align::right);
        }
    }
    ui.set_text_backing(true);
}

} // namespace geosim::client
