// The in-game internet (web.hpp, docs/08-ui.md#internet).

#include "sim/web.hpp"

#include "core/calendar.hpp"
#include "core/csv.hpp"
#include "core/diagnostics.hpp"
#include "core/keys.hpp"
#include "sim/model.hpp"
#include "sim/scenario.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace geosim::sim::web {

namespace {

uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
uint64_t hash(uint64_t a, uint64_t b, uint64_t c = 0) { return mix(mix(mix(a) ^ b) ^ c); }
float unit(uint64_t h) { return static_cast<float>(h >> 40) / 16777216.0f; }
uint64_t text_hash(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for(char c : s) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}

std::string lower(std::string s) {
    for(auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string slug(std::string const& s) {
    std::string out;
    for(char c : s) {
        if(std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}
bool contains(std::string const& hay, std::string const& needle) { return lower(hay).find(lower(needle)) != std::string::npos; }

std::string fmt(char const* f, double x) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), f, x);
    return buf;
}

std::string nation_name(game_state const& state, names const& nm, dcon::nation_id n) {
    if(!n) return "the world";
    const auto i = static_cast<std::size_t>(n.index());
    if(nm.nations && i < nm.nations->size() && !(*nm.nations)[i].empty()) return (*nm.nations)[i];
    return core::unpack_key(state.world.nation_get_iso3(n));
}
std::string region_name(game_state const& state, names const& nm, dcon::region_id r) {
    const auto i = static_cast<std::size_t>(r.index());
    if(nm.regions && i < nm.regions->size() && !(*nm.regions)[i].empty()) return (*nm.regions)[i];
    const std::string key = core::unpack_key(state.world.region_get_key(r));
    return key.empty() ? "Region " + std::to_string(i) : key;
}
std::string tld(game_state const& state, dcon::nation_id n) { return lower(core::unpack_key(state.world.nation_get_iso3(n))); }
std::string adj_or_name(std::string const& name) {
    const auto a = adjective(name);
    return a.empty() ? name : a;
}
std::string date_of(game_state const& state, int32_t tick) { return core::format_date(core::date_from_tick(tick, state.epoch_days)); }

// --- procedural names ---------------------------------------------------------

// Syllable pools by flavour: 'g' the global web, 's' the Chinese-language web, 'r' the
// Russian-language web.
std::string engine_name(char flavour, uint64_t seed) {
    static constexpr char const* g1[] = {"Goo", "Zoo", "Fin", "Seek", "Loo", "Quo", "Navi", "Ask", "Sear", "Lumi", "Vista", "Ora"};
    static constexpr char const* g2[] = {"gler", "dle", "dex", "ka", "ra", "lix", "nyx", "po", "vo", "sta", "lio", "ster"};
    static constexpr char const* s1[] = {"Bai", "Sou", "Hao", "Qian", "Xun", "Zhi", "Tian", "Long"};
    static constexpr char const* s2[] = {"du", "gou", "sou", "lin", "dao", "wen", "zhao", "jing"};
    static constexpr char const* r1[] = {"Ya", "Ra", "Zna", "Ve", "Mi", "Po", "Sky", "Iska"};
    static constexpr char const* r2[] = {"ndex", "mbler", "tok", "lka", "nets", "visk", "bor", "vik"};
    static constexpr char const* i1[] = {"Bright", "Dawn", "Red Star ", "Morning ", "Unity ", "Torch"};
    static constexpr char const* i2[] = {"Net", "Light", "Gate", "Path", "Search", "Finder"};
    const uint64_t a = hash(seed, 1), b = hash(seed, 2);
    switch(flavour) {
    case 'i': return std::string(i1[a % std::size(i1)]) + i2[b % std::size(i2)];
    case 's': return std::string(s1[a % std::size(s1)]) + s2[b % std::size(s2)];
    case 'r': return std::string(r1[a % std::size(r1)]) + r2[b % std::size(r2)];
    default: return std::string(g1[a % std::size(g1)]) + g2[b % std::size(g2)];
    }
}
std::string social_name(char flavour, uint64_t seed) {
    static constexpr char const* g1[] = {"Face", "Friend", "Chirp", "Circle", "Link", "Pal", "Buddy", "Snap", "Folk", "Kin"};
    static constexpr char const* g2[] = {"space", "ly", "r", "net", "hub", "book", "verse", "mate", "zone", "loop"};
    static constexpr char const* s1[] = {"Wei", "Qun", "Liao", "Xin", "Peng", "Hui"};
    static constexpr char const* s2[] = {"bo", "xin", "quan", "you", "ba", "liao"};
    static constexpr char const* r1[] = {"Druz", "Vstre", "Kontak", "Odno", "Sobe", "Bese"};
    static constexpr char const* r2[] = {"ya", "cha", "tik", "klassnik", "ra", "dka"};
    static constexpr char const* i1[] = {"Comrades' ", "People's ", "Citizens' ", "Workers' "};
    static constexpr char const* i2[] = {"Circle", "Square", "Hall", "Corner"};
    const uint64_t a = hash(seed, 3), b = hash(seed, 4);
    switch(flavour) {
    case 'i': return std::string(i1[a % std::size(i1)]) + i2[b % std::size(i2)];
    case 's': return std::string(s1[a % std::size(s1)]) + s2[b % std::size(s2)];
    case 'r': return std::string(r1[a % std::size(r1)]) + r2[b % std::size(r2)];
    default: return std::string(g1[a % std::size(g1)]) + g2[b % std::size(g2)];
    }
}

// --- sectors ------------------------------------------------------------------

struct sector_def {
    char const* key;
    char const* hub;
    char flavour;
    std::array<char const*, 6> members; // nullptr ends
};
// Walled-off webs with their own search engine and social network (a moddable table;
// everything else uses the global web).
constexpr sector_def sector_defs[] = {
    {"sinic", "CHN", 's', {"CHN", nullptr, nullptr, nullptr, nullptr, nullptr}},
    {"runet", "RUS", 'r', {"RUS", "BLR", "KAZ", "KGZ", "TJK", "ARM"}},
};

// Per-nation inputs shared by every site.
struct nation_facts {
    float internet = 0;   // users
    float unrest = 0;     // population mean
    float protest = 0;
    float instability = 0;
    float propaganda = 0; // the nation's news outlets
    float state_prop = 0; // its state portal
};

nation_facts facts_of(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    nation_facts f;
    // DataReportal 2020: social-media users are ~84 % of internet users.
    f.internet = w.nation_get_population(n) * std::min(0.98f, w.nation_get_connectivity(n) / 0.84f);
    float pop = 0.0f;
    for(auto own : w.nation_get_region_ownership(n)) {
        auto r = own.get_region();
        f.unrest += r.get_unrest() * r.get_population();
        f.protest += r.get_protest() * r.get_population();
        pop += r.get_population();
    }
    if(pop > 0.0f) f.unrest /= pop, f.protest /= pop;
    bool war = false;
    for(auto m : w.in_nation) war = war || w.nation_get_at_war(n, static_cast<uint16_t>(m.id.index())) > 0.0f;
    f.instability = std::clamp(1.5f * std::max(0.0f, 0.55f - w.nation_get_legitimacy(n)) + 0.15f * std::max(0.0f, f.unrest - 1.5f) + w.nation_get_occupied(n)
                                   + 0.5f * w.nation_get_cyber_damage(n) + (w.nation_get_defaulted(n) ? 0.15f : 0.0f) + (war ? 0.1f : 0.0f) + 0.5f * f.protest,
                               0.0f, 0.95f);
    // Outlets reflect reality less where the press is unfree, information is controlled
    // and power is captured; the state's own portal less still under weak liberties.
    f.propaganda = std::clamp(0.7f * (1.0f - w.nation_get_media_freedom(n)) + 0.5f * (w.nation_get_info_control(n) - 0.2f) + 0.3f * w.nation_get_capture_index(n) - 0.15f, 0.0f, 1.0f);
    f.state_prop = std::clamp(f.propaganda + 0.35f * (1.0f - w.nation_get_liberal(n)), 0.0f, 1.0f);
    return f;
}

// The board of a nation's largest firm (optionally in sector key `key`).
dcon::agent_id largest_board(game_state const& state, dcon::nation_id n, char const* key) {
    auto const& w = state.world;
    dcon::agent_id best;
    float size = -1.0f;
    for(auto f : w.in_firm) {
        if(f.get_nation() != n || !f.get_board()) continue;
        if(key && core::unpack_key(f.get_sector().get_key()) != key) continue;
        const float s = f.get_share() * w.nation_get_va0(n, static_cast<uint8_t>(f.get_sector().id.index()));
        if(s > size) size = s, best = f.get_board().id;
    }
    return best;
}
std::string board_label(game_state const& state, names const& nm, dcon::agent_id board) {
    auto const& w = state.world;
    std::string word = "Group";
    for(auto f : w.in_firm) {
        if(f.get_board() != board) continue;
        const std::string k = core::unpack_key(f.get_sector().get_key());
        word = k == "agr" ? "Agri Holdings" : k == "ene" ? "Energy" : k == "man" ? "Industries" : k == "con" ? "Construction" : k == "csv" ? "Leisure Group"
             : k == "osv" ? "Capital" : "Group";
        break;
    }
    return adj_or_name(nation_name(state, nm, w.agent_get_home(board))) + " " + word;
}

bool is_down(game_state const& state, uint64_t site_key, float p) {
    const auto week = static_cast<uint64_t>(std::max(0, state.current_tick) / 7);
    return unit(hash(state.rng_seed, site_key, week)) < std::clamp(p, 0.0f, 0.97f);
}

// --- events (what sites report) ------------------------------------------------

struct event {
    int32_t tick = 0;
    dcon::nation_id n, m; // subject and partner
    int8_t tone = 0;      // for n: -1 bad, 1 good
    float weight = 1;     // newsworthiness
    bool sensitive = false; // censored by controlled media at home
    std::string neutral, spun, tag; // spun: the subject's propaganda version ("" = omitted)
};

std::vector<event> gather_events(game_state const& state, names const& nm, int32_t days) {
    auto const& w = state.world;
    std::vector<event> out;
    const int32_t since = state.current_tick - days;
    auto nm_of = [&](dcon::nation_id n) { return nation_name(state, nm, n); };
    for(auto d : w.in_decision) {
        if(d.get_tick() < since) continue;
        const uint8_t code = d.get_lever();
        if(code < journal_player_action && code != journal_request) continue; // lever decisions
        const dcon::nation_id n = d.get_target();
        if(!n) continue;
        if(code == journal_mega_event) { // pragmatic = organisation index, before = 0 awarded / 1 opened / 2 closed / 3 postponed
            const float oi = d.get_pragmatic();
            if(!(oi >= 0.0f) || oi >= static_cast<float>(w.organisation_size())) continue;
            std::string org = core::unpack_key(w.organisation_get_key(dcon::organisation_id{static_cast<dcon::organisation_id::value_base_t>(oi)}));
            for(auto& c : org) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            static constexpr char const* what[] = {"awards its games to", "games open in", "games close in", "postpones its games in"};
            const auto k = static_cast<std::size_t>(std::clamp(d.get_before(), 0.0f, 3.0f));
            event e;
            e.tick = d.get_tick();
            e.n = n;
            e.tone = k == 3 ? int8_t(-1) : int8_t(1);
            e.weight = k == 1 ? 3.0f : 2.0f;
            e.tag = "Sports";
            e.neutral = org + " " + what[k] + " " + nm_of(n);
            e.spun = e.neutral;
            out.push_back(std::move(e));
            continue;
        }
        dcon::nation_id m;
        const float p = d.get_pragmatic();
        if(std::isfinite(p) && p >= 0.0f && p < static_cast<float>(w.nation_size())) m = dcon::nation_id{static_cast<dcon::nation_id::value_base_t>(p)};
        event e;
        e.tick = d.get_tick();
        e.n = n;
        const std::string A = nm_of(n), Adj = adj_or_name(A), B = m ? nm_of(m) : std::string{};
        switch(code) {
        case journal_war:
            if(!m) continue;
            e.m = m, e.tone = -1, e.weight = 5, e.tag = "War";
            e.neutral = "Fighting erupts between " + A + " and " + B;
            e.spun = Adj + " forces launch operation to protect the homeland from " + B;
            break;
        case journal_peace:
            if(!m) continue;
            e.m = m, e.tone = 1, e.weight = 4, e.tag = "Peace";
            e.neutral = A + " and " + B + " agree to end hostilities";
            e.spun = B + " accepts " + Adj + " terms as the operation achieves its goals";
            break;
        case journal_coup:
            e.tone = -1, e.weight = 5, e.tag = "Coup", e.sensitive = true;
            e.neutral = "Military seizes power in " + A;
            e.spun = "Armed forces step in to restore order in " + A;
            break;
        case journal_regime_change:
            e.tone = -1, e.weight = 5, e.tag = "Revolution", e.sensitive = true;
            e.neutral = "Mass protests topple the government of " + A;
            break;
        case journal_election: {
            const auto inc = w.nation_get_incumbent(n);
            const std::string party = inc ? core::unpack_key(w.party_get_key(inc)) : std::string("the incumbents");
            e.weight = 3, e.tag = "Election";
            e.neutral = A + " votes: " + party + " forms the government";
            e.spun = "Landslide in " + A + " confirms the people's trust in " + party;
            break;
        }
        case journal_no_confidence:
            e.tone = -1, e.weight = 2, e.tag = "Politics";
            e.neutral = "Government of " + A + " loses confidence vote; snap election called";
            break;
        case journal_default:
            e.tone = -1, e.weight = 4, e.tag = "Markets", e.sensitive = true;
            e.neutral = A + " defaults on its debt";
            e.spun = A + " stands firm against speculators, suspends payments to foreign funds";
            break;
        case journal_insurgency:
            if(d.get_after() < 0.5f) {
                e.tone = 1, e.weight = 2, e.tag = "Security";
                e.neutral = "Insurgency in " + A + " ends";
                e.spun = Adj + " security forces restore peace";
            } else {
                e.tone = -1, e.weight = 4, e.tag = "Conflict", e.sensitive = true;
                e.neutral = "Armed rebellion breaks out in " + A;
                e.spun = "Security forces contain criminal gangs in the provinces";
            }
            break;
        case journal_dispute:
            if(!m) continue;
            e.m = m, e.tone = -1, e.weight = 2, e.tag = "Diplomacy";
            e.neutral = A + "-" + B + " tensions escalate";
            e.spun = A + " rejects provocations from " + B;
            break;
        case journal_agreement:
            if(d.get_after() < 0.5f || d.get_after() > 1.5f) continue;
            e.m = m, e.tone = 1, e.weight = 1.5f, e.tag = "Diplomacy";
            e.neutral = A + " signs an agreement" + (m ? " with " + B : std::string{});
            e.spun = e.neutral;
            break;
        case journal_cyber:
            if(!m) continue;
            e.n = m, e.m = n, e.tone = -1, e.weight = 2, e.tag = "Cyber";
            e.neutral = B + " hit by a cyberattack";
            e.spun = B + " repels foreign hackers";
            break;
        case journal_concession:
            e.tone = -1, e.weight = 1.5f, e.tag = "Sanctions";
            e.neutral = A + " gives in to sanctions" + (m ? " by " + B : std::string{});
            e.spun = A + " and " + (m ? B : std::string("partners")) + " normalise trade";
            break;
        case journal_term_limit:
            if(d.get_after() < 0.5f) continue;
            e.tone = -1, e.weight = 2, e.tag = "Politics", e.sensitive = true;
            e.neutral = "Leader of " + A + " stays on beyond the term limit";
            e.spun = "Nation asks its leader to continue the work";
            break;
        default: continue;
        }
        out.push_back(std::move(e));
    }
    // Disasters (their own records carry casualties).
    for(auto ds : w.in_disaster) {
        if(ds.get_start() < since || !ds.get_nation()) continue;
        const auto t = describe_disaster(state, ds.id);
        if(t.potential) continue;
        event e;
        e.tick = ds.get_start();
        e.n = ds.get_nation();
        e.tone = -1, e.weight = 1.0f + std::log10(1.0f + t.deaths), e.tag = "Disaster";
        e.neutral = t.title + " in " + nation_name(state, nm, e.n) + (t.deaths >= 1.0f ? fmt(": %.0f dead", t.deaths) : std::string{});
        e.spun = "Authorities respond swiftly to " + lower(t.title);
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(), [](event const& a, event const& b) { return a.tick > b.tick; });
    // Both sides of a pair journal the same event: keep one.
    std::vector<event> unique;
    for(auto& e : out) {
        if(std::none_of(unique.begin(), unique.end(), [&](event const& u) { return u.neutral == e.neutral && std::abs(u.tick - e.tick) <= 28; })) unique.push_back(std::move(e));
    }
    return unique;
}

// Headlines from a nation's own indicators (real values; propaganda reports others).
struct statistic {
    std::string label;
    float real = 0, official = 0;
    char const* unit = "%";
};
std::vector<statistic> statistics_of(game_state const& state, dcon::nation_id n, float prop) {
    auto const& w = state.world;
    std::vector<statistic> out;
    const float u = 100.0f * w.nation_get_unemployment(n);
    out.push_back({"Unemployment", u, u * (1.0f - 0.6f * prop)});
    const float gap = 100.0f * w.nation_get_gap_smoothed(n);
    out.push_back({"Output vs normal", gap, gap + 4.0f * prop});
    const float infl = 100.0f * w.nation_get_inflation_expect(n);
    out.push_back({"Inflation (expected)", infl, infl - 3.0f * prop * std::max(0.0f, infl) / std::max(1.0f, infl)});
    double deaths = 0.0, pop = 0.0;
    for(auto own : w.nation_get_region_ownership(n)) {
        deaths += own.get_region().get_deaths_reported();
        pop += own.get_region().get_population();
    }
    const float dpm = pop > 0.0 ? static_cast<float>(deaths / pop * 1e6) : 0.0f;
    out.push_back({"Epidemic deaths per million", dpm, dpm * (1.0f - 0.7f * prop), ""});
    out.push_back({"Government approval", 100.0f * w.nation_get_approval(n), std::min(99.0f, 100.0f * w.nation_get_approval(n) + 60.0f * prop)});
    return out;
}

struct headline {
    std::string text, tag;
    int32_t tick = 0;
    int8_t tone = 0;
    float weight = 1;
};

std::vector<headline> domestic_news(game_state const& state, names const& nm, std::vector<event> const& events, dcon::nation_id n, float prop, nation_facts const& f, uint64_t key) {
    auto const& w = state.world;
    std::vector<headline> out;
    const std::string A = nation_name(state, nm, n), Adj = adj_or_name(A);
    for(auto const& e : events) {
        if(e.n != n && e.m != n) continue;
        const bool own_bad = e.n == n ? e.tone < 0 : false;
        if(own_bad && prop > 0.3f) {
            // Bad news at home: omitted, or spun, the more so the less free the outlet.
            const float u = unit(hash(key, text_hash(e.neutral)));
            if((e.sensitive && prop > 0.5f) || u < prop * 0.6f) continue;
            if(prop > 0.45f && !e.spun.empty()) {
                out.push_back({e.spun, e.tag, e.tick, 1, e.weight});
                continue;
            }
        }
        if(e.n != n && e.m == n && prop > 0.5f && !e.spun.empty() && e.tone < 0) { // a foreign event about us
            out.push_back({"Hostile actions by " + nation_name(state, nm, e.n) + " condemned", e.tag, e.tick, 0, e.weight});
            continue;
        }
        out.push_back({e.neutral, e.tag, e.tick, e.n == n ? e.tone : static_cast<int8_t>(0), e.weight});
    }
    // The state of things.
    const int32_t now = state.current_tick;
    const float u = w.nation_get_unemployment(n), u0 = w.nation_get_unemployment0(n);
    if(u > u0 + 0.02f) {
        if(prop < 0.45f) out.push_back({fmt("Unemployment climbs to %.1f%%", 100.0 * u), "Economy", now, -1, 2});
        else out.push_back({"Jobs market resilient as " + Adj + " workers adapt", "Economy", now, 1, 1});
    } else if(u < u0 - 0.005f) {
        out.push_back({fmt("Unemployment falls to %.1f%%", 100.0 * u), "Economy", now, 1, 1.5f});
    }
    const float gap = w.nation_get_gap_smoothed(n);
    if(gap < -0.03f) {
        if(prop < 0.45f) out.push_back({fmt("Economy %.1f%% below normal as output slumps", -100.0 * gap), "Economy", now, -1, 2});
        else out.push_back({"Economy weathers global headwinds", "Economy", now, 1, 1});
    } else if(gap > 0.01f) {
        out.push_back({"Growth beats expectations", "Economy", now, 1, 1});
    }
    if(w.nation_get_inflation_expect(n) > w.nation_get_inflation_trend(n) + 0.02f) {
        out.push_back({prop < 0.45f ? fmt("Prices rising: households expect %.1f%% inflation", 100.0 * w.nation_get_inflation_expect(n))
                                    : std::string("Speculators blamed for price swings"),
                       "Economy", now, prop < 0.45f ? int8_t(-1) : int8_t(0), 1.5f});
    }
    if(f.protest > 0.03f) {
        if(prop < 0.4f) out.push_back({"Protests spread across " + A, "Society", now, -1, 2.5f});
        else if(prop < 0.7f) out.push_back({"Police disperse small groups of provocateurs", "Society", now, 0, 1});
    }
    if(reported_deaths_per_million_daily(state, n) > 1.0f) {
        if(prop < 0.5f) out.push_back({fmt("Epidemic: %.1f deaths per million a day", reported_deaths_per_million_daily(state, n)), "Health", now, -1, 2});
        else out.push_back({"Health system has the situation under control", "Health", now, 1, 1});
    }
    if(w.nation_get_repression(n) > 0.3f && prop < 0.3f) out.push_back({"Crackdown on protesters draws criticism", "Rights", now, -1, 1.5f});
    // Propaganda fills the page with good news.
    static constexpr char const* fabricated[] = {"Record harvest expected across {N}", "{Adj} engineers unveil a world-class bridge",
                                                 "Foreign investors praise {Adj} stability", "New hospital opens ahead of schedule",
                                                 "Citizens rally in support of national unity", "{Adj} athletes triumph abroad",
                                                 "Exports reach new heights", "The leadership's plan delivers prosperity",
                                                 "{Adj} scientists announce breakthrough", "Tourists flock to {N}"};
    const int fake = static_cast<int>(std::round(prop * 4.0f));
    for(int i = 0; i < fake; ++i) {
        std::string t = fabricated[hash(key, static_cast<uint64_t>(now / 7), static_cast<uint64_t>(i)) % std::size(fabricated)];
        for(auto [from, to] : {std::pair<std::string, std::string>{"{Adj}", Adj}, {"{N}", A}}) {
            for(auto p = t.find(from); p != std::string::npos; p = t.find(from)) t.replace(p, from.size(), to);
        }
        if(std::none_of(out.begin(), out.end(), [&](headline const& h) { return h.text == t; })) out.push_back({t, "National", now - i, 1, 1.2f});
    }
    std::stable_sort(out.begin(), out.end(), [](headline const& a, headline const& b) { return a.tick != b.tick ? a.tick > b.tick : a.weight > b.weight; });
    return out;
}

std::vector<headline> world_news(game_state const& state, std::vector<event> const& events, dcon::nation_id viewer, float prop) {
    auto const& w = state.world;
    std::vector<headline> out;
    for(auto const& e : events) {
        if(viewer && (e.n == viewer || e.m == viewer)) continue;
        if(viewer && prop > 0.4f && e.tone < 0) {
            // Controlled outlets cover rivals' troubles and keep friends' quiet.
            const auto k = static_cast<uint16_t>(e.n.index());
            const bool friendly = w.nation_get_alliance(viewer, k) > 0.0f || w.nation_get_trust(viewer, k) > 0.7f;
            if(friendly) continue;
        }
        out.push_back({e.neutral, e.tag, e.tick, 0, e.weight});
    }
    std::stable_sort(out.begin(), out.end(), [](headline const& a, headline const& b) { return a.weight * 30.0f + static_cast<float>(a.tick) > b.weight * 30.0f + static_cast<float>(b.tick); });
    return out;
}

// Bylines: `people` drawn from a nation's culture mix (no pools: none).
struct bylines {
    name_pools const* pools = nullptr;
    std::vector<std::pair<int, float>> mix;
    uint64_t key = 0;
    [[nodiscard]] std::string by(uint64_t salt) const { return pools ? person(*pools, mix, hash(key, salt)) : std::string{}; }
};

void add_headlines(page& pg, game_state const& state, std::vector<headline> const& hs, std::size_t max, std::string const& url = {}, bylines const* by = nullptr) {
    for(std::size_t i = 0; i < hs.size() && i < max; ++i) {
        block b;
        b.k = block::kind::headline;
        b.text = hs[i].text;
        b.sub = hs[i].tag + " · " + date_of(state, hs[i].tick);
        if(by) {
            const std::string who = by->by(text_hash(hs[i].text));
            if(!who.empty()) b.sub += " · by " + who;
        }
        b.tone = hs[i].tone;
        b.url = url;
        pg.blocks.push_back(std::move(b));
    }
}
void add(page& pg, block::kind k, std::string text, std::string sub = {}, std::string url = {}, int8_t tone = 0) {
    pg.blocks.push_back({k, std::move(text), std::move(sub), std::move(url), tone});
}

std::string handle(std::string const& name, uint64_t seed) {
    std::string h;
    for(char c : name) {
        if(c == ' ') {
            h += '_';
            continue;
        }
        h += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if(h.empty()) h = "user";
    return h + std::to_string(seed % 100);
}

std::string engine_of_sector(directory const& dir, std::size_t s) { return s < dir.sectors.size() ? dir.sectors[s].engine : std::string{}; }

} // namespace

std::string adjective(std::string const& name) {
    static constexpr std::pair<char const*, char const*> known[] = {
        {"United States", "American"}, {"United Kingdom", "British"}, {"France", "French"}, {"Germany", "German"}, {"China", "Chinese"},
        {"Japan", "Japanese"}, {"Netherlands", "Dutch"}, {"Spain", "Spanish"}, {"Sweden", "Swedish"}, {"Turkey", "Turkish"}, {"Poland", "Polish"},
        {"Ireland", "Irish"}, {"Switzerland", "Swiss"}, {"Greece", "Greek"}, {"Thailand", "Thai"}, {"Vietnam", "Vietnamese"}, {"Viet Nam", "Vietnamese"},
        {"Iraq", "Iraqi"}, {"Israel", "Israeli"}, {"Pakistan", "Pakistani"}, {"Bangladesh", "Bangladeshi"}, {"Philippines", "Filipino"},
        {"Portugal", "Portuguese"}, {"Denmark", "Danish"}, {"Norway", "Norwegian"}, {"Finland", "Finnish"}, {"Saudi Arabia", "Saudi"},
        {"Egypt", "Egyptian"}, {"Iran", "Iranian"}, {"Brazil", "Brazilian"}, {"Argentina", "Argentine"}, {"Chile", "Chilean"}, {"Peru", "Peruvian"},
        {"Canada", "Canadian"}, {"Mexico", "Mexican"}, {"Russia", "Russian"}, {"Russian Federation", "Russian"}, {"Korea", "Korean"},
        {"South Korea", "South Korean"}, {"North Korea", "North Korean"}, {"New Zealand", "New Zealand"}, {"Lebanon", "Lebanese"},
        {"Sudan", "Sudanese"}, {"Senegal", "Senegalese"}, {"Congo", "Congolese"}, {"Belgium", "Belgian"}, {"Iceland", "Icelandic"},
        {"Afghanistan", "Afghan"}, {"Kazakhstan", "Kazakh"}, {"Uzbekistan", "Uzbek"}, {"Belarus", "Belarusian"}, {"Ukraine", "Ukrainian"},
        {"Yemen", "Yemeni"}, {"Oman", "Omani"}, {"Qatar", "Qatari"}, {"Kuwait", "Kuwaiti"}, {"Taiwan", "Taiwanese"}, {"Malta", "Maltese"},
        {"Cyprus", "Cypriot"}, {"Morocco", "Moroccan"}, {"Ecuador", "Ecuadorian"}, {"Uruguay", "Uruguayan"}, {"Paraguay", "Paraguayan"},
        {"Venezuela", "Venezuelan"}, {"Colombia", "Colombian"}, {"Cuba", "Cuban"}, {"Nepal", "Nepali"}, {"Myanmar", "Burmese"}};
    for(auto const& [k, v] : known) {
        if(name == k) return v;
    }
    auto ends = [&](std::string_view s) { return name.size() > s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0; };
    if(name.find(' ') != std::string::npos) return {};
    if(ends("ia")) return name + "n";                                  // Austria -> Austrian
    if(ends("a")) return name + "n";                                   // Kenya -> Kenyan
    if(ends("y")) return name.substr(0, name.size() - 1) + "ian";      // Italy -> Italian
    if(ends("o")) return name.substr(0, name.size() - 1) + "an";       // Mexico-like
    if(ends("stan")) return name + "i";                                // -stan -> -stani
    if(ends("e")) return name.substr(0, name.size() - 1) + "ian";
    if(ends("an") || ends("on") || ends("al") || ends("il")) return name + "ian";
    return {};
}

domain_table::entry domain_table::of(std::string const& iso3) const {
    for(auto const& e : entries) {
        if(e.iso3 == iso3) return e;
    }
    const std::string l = lower(iso3);
    return {iso3, {}, "com." + l, "gov." + l, "org." + l, "edu." + l};
}

bool load_domain_table(std::string const& defs_dir, domain_table& out, std::string& error) {
    core::diagnostics diag;
    out = {};
    const auto t = core::csv_table::load(defs_dir + "/tlds.csv", diag);
    if(diag.failed()) {
        error = diag.error();
        return false;
    }
    for(std::size_t r = 0; r < t.rows(); ++r) {
        domain_table::entry e;
        e.iso3 = std::string(t.get(r, "iso3"));
        e.tld = std::string(t.get(r, "tld"));
        const std::string l = lower(e.iso3);
        auto col = [&](char const* c, std::string fallback) {
            std::string v(t.get(r, c));
            return v.empty() ? fallback : v;
        };
        e.commercial = col("commercial", e.tld.empty() ? "com." + l : e.tld);
        e.government = col("government", "gov." + (e.tld.empty() ? l : e.tld));
        e.organisation = col("organisation", "org." + (e.tld.empty() ? l : e.tld));
        e.academic = col("academic", "edu." + (e.tld.empty() ? l : e.tld));
        out.entries.push_back(std::move(e));
    }
    return true;
}

bool load_name_pools(std::string const& defs_dir, name_pools& out, std::string& error) {
    core::diagnostics diag;
    out = {};
    const auto pools = core::csv_table::load(defs_dir + "/names.csv", diag);
    if(diag.failed()) {
        error = diag.error();
        return false;
    }
    auto culture_index = [&](std::string const& c) {
        for(std::size_t i = 0; i < out.cultures.size(); ++i) {
            if(out.cultures[i] == c) return static_cast<int>(i);
        }
        out.cultures.push_back(c);
        out.given.emplace_back();
        out.surname.emplace_back();
        return static_cast<int>(out.cultures.size() - 1);
    };
    for(std::size_t r = 0; r < pools.rows(); ++r) {
        const int c = culture_index(std::string(pools.get(r, "culture")));
        const std::string kind(pools.get(r, "kind")), list(pools.get(r, "names"));
        auto& dst = kind == "given" ? out.given[static_cast<std::size_t>(c)] : out.surname[static_cast<std::size_t>(c)];
        std::size_t at = 0;
        while(at <= list.size()) {
            const auto bar = std::min(list.find('|', at), list.size());
            if(bar > at) dst.push_back(list.substr(at, bar - at));
            at = bar + 1;
        }
    }
    const auto map = core::csv_table::load(defs_dir + "/name_cultures.csv", diag);
    if(diag.failed()) {
        error = diag.error();
        return false;
    }
    for(std::size_t r = 0; r < map.rows(); ++r) {
        name_pools::rule rule;
        rule.iso3 = std::string(map.get(r, "iso3"));
        rule.group = std::string(map.get(r, "group"));
        const std::string c(map.get(r, "culture"));
        rule.culture = -1;
        for(std::size_t i = 0; i < out.cultures.size(); ++i) {
            if(out.cultures[i] == c) rule.culture = static_cast<int>(i);
        }
        if(rule.culture < 0) {
            error = defs_dir + "/name_cultures.csv: unknown culture '" + c + "'";
            return false;
        }
        rule.weight = static_cast<float>(map.number_or(r, "weight", 1.0));
        out.rules.push_back(std::move(rule));
    }
    return true;
}

std::vector<std::pair<int, float>> culture_mix(game_state const& state, name_pools const& pools, dcon::nation_id n) {
    std::vector<std::pair<int, float>> mix;
    if(pools.empty() || !n) return mix;
    auto const& w = state.world;
    const std::string iso = core::unpack_key(w.nation_get_iso3(n));
    // Rules for (nation, group), then (any nation, group), then the nation's default.
    auto rules_for = [&](std::string const& group) {
        std::vector<name_pools::rule const*> out;
        for(int pass = 0; pass < 3 && out.empty(); ++pass) {
            for(auto const& r : pools.rules) {
                const bool match = pass == 0 ? (r.iso3 == iso && r.group == group) : pass == 1 ? (r.iso3 == "*" && r.group == group) : (r.iso3 == iso && r.group == "*");
                if(match) out.push_back(&r);
            }
        }
        return out;
    };
    auto add = [&](std::string const& group, float people) {
        auto rs = rules_for(group);
        float total = 0.0f;
        for(auto const* r : rs) total += r->weight;
        if(rs.empty() || total <= 0.0f) {
            mix.push_back({0, people}); // no rule at all: the first culture
            return;
        }
        for(auto const* r : rs) {
            auto it = std::find_if(mix.begin(), mix.end(), [&](auto const& m) { return m.first == r->culture; });
            if(it == mix.end()) mix.push_back({r->culture, people * r->weight / total});
            else it->second += people * r->weight / total;
        }
    };
    // The groups' current population (pops carry their group; others are the default).
    std::vector<std::pair<std::string, float>> groups;
    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) {
            auto p = loc.get_pop();
            const std::string key = p.get_group() ? core::unpack_key(p.get_group().get_key()) : std::string("*");
            auto it = std::find_if(groups.begin(), groups.end(), [&](auto const& g) { return g.first == key; });
            if(it == groups.end()) groups.push_back({key, p.get_size()});
            else it->second += p.get_size();
        }
    }
    for(auto const& [key, people] : groups) add(key, people);
    if(mix.empty()) add("*", 1.0f);
    return mix;
}

std::string person(name_pools const& pools, std::vector<std::pair<int, float>> const& mix, uint64_t seed) {
    if(pools.empty() || mix.empty()) return {};
    float total = 0.0f;
    for(auto const& m : mix) total += m.second;
    float x = unit(hash(seed, 0x9E0)) * total;
    int c = mix.back().first;
    for(auto const& m : mix) {
        x -= m.second;
        if(x <= 0.0f) {
            c = m.first;
            break;
        }
    }
    auto const& g = pools.given[static_cast<std::size_t>(c)];
    auto const& sn = pools.surname[static_cast<std::size_t>(c)];
    if(g.empty() || sn.empty()) return {};
    return g[hash(seed, 1) % g.size()] + " " + sn[hash(seed, 2) % sn.size()];
}

char const* category_name(site_kind k) {
    switch(k) {
    case site_kind::engine: return "Search";
    case site_kind::social: return "Social network";
    case site_kind::state: return "Government";
    case site_kind::paper: return "News";
    case site_kind::regional: return "Regional news";
    case site_kind::watchdog: return "Watchdog";
    case site_kind::sports: return "Sports";
    case site_kind::forum: return "Forum";
    case site_kind::business: return "Business";
    case site_kind::tabloid: return "Tabloid";
    }
    return "Site";
}

site const* directory::find(std::string const& domain) const {
    for(auto const& s : sites) {
        if(s.domain == domain) return &s;
    }
    return nullptr;
}

directory build_directory(game_state const& state, names const& nm) {
    auto const& w = state.world;
    directory dir;
    const std::size_t N = w.nation_size();
    // Domains: tlds.csv, else com.<iso3> and so on.
    static const domain_table no_table{};
    domain_table const& domains = nm.domains ? *nm.domains : no_table;
    auto dom = [&](dcon::nation_id n) { return domains.of(core::unpack_key(w.nation_get_iso3(n))); };
    // A hub's own services sit on its ccTLD (or its commercial domain without one).
    auto cc = [&](dcon::nation_id n) {
        const auto e = dom(n);
        return e.tld.empty() ? e.commercial : e.tld;
    };
    dir.sector_of.assign(N, 0);
    // Sectors: the global web and the walled-off hubs present in the scenario.
    {
        sector g;
        g.key = "global";
        g.engine = slug(engine_name('g', hash(state.rng_seed, 0x5EA))) + ".com";
        g.social = slug(social_name('g', hash(state.rng_seed, 0x50C))) + ".com";
        dir.sectors.push_back(std::move(g));
    }
    for(auto const& def : sector_defs) {
        const auto hub = find_nation(state, def.hub);
        if(!hub) continue;
        sector s;
        s.key = def.key;
        s.hub = hub;
        const uint64_t seed = hash(state.rng_seed, text_hash(def.key));
        s.engine = slug(engine_name(def.flavour, seed)) + "." + cc(hub);
        s.social = slug(social_name(def.flavour, seed)) + "." + cc(hub);
        s.flavour = def.flavour;
        for(auto m : def.members) {
            if(!m) break;
            if(const auto n = find_nation(state, m)) {
                s.members.push_back(n);
                dir.sector_of[static_cast<std::size_t>(n.index())] = static_cast<uint8_t>(dir.sectors.size());
            }
        }
        dir.sectors.push_back(std::move(s));
    }
    // Closed national intranets: where press freedom and liberties are near zero the
    // state walls its people into its own network and runs every site on it.
    for(auto n : w.in_nation) {
        const auto i = static_cast<std::size_t>(n.id.index());
        if(dir.sector_of[i] != 0 || n.get_media_freedom() >= 0.12f || n.get_liberal() >= 0.2f || n.get_population() < 1e6f) continue;
        sector s;
        s.key = "intranet." + tld(state, n.id);
        s.hub = n.id;
        s.members.push_back(n.id);
        s.flavour = 'i';
        s.closed = true;
        const uint64_t seed = hash(state.rng_seed, text_hash(s.key));
        s.engine = slug(engine_name('i', seed)) + "." + cc(n.id);
        s.social = slug(social_name('i', seed)) + "." + cc(n.id);
        dir.sector_of[i] = static_cast<uint8_t>(dir.sectors.size());
        dir.sectors.push_back(std::move(s));
    }
    std::vector<nation_facts> facts(N);
    for(auto n : w.in_nation) facts[static_cast<std::size_t>(n.id.index())] = facts_of(state, n.id);

    // Engines and social networks: a hub's own take a share growing with low media
    // freedom and filtering; members follow by their trust in the hub; walls push the
    // rest to the domestic one.
    std::vector<double> engine_users(dir.sectors.size(), 0.0), social_users(dir.sectors.size(), 0.0);
    for(auto n : w.in_nation) {
        const auto i = static_cast<std::size_t>(n.id.index());
        const float users = facts[i].internet, social = w.nation_get_population(n.id) * w.nation_get_connectivity(n.id);
        const std::size_t s = dir.sector_of[i];
        float dom_e = 0.0f, dom_s = 0.0f;
        if(s != 0) {
            const auto hub = dir.sectors[s].hub;
            const float mf_hub = w.nation_get_media_freedom(hub), filt = w.nation_get_net_filtering(n.id);
            if(dir.sectors[s].closed) {
                dom_e = dom_s = 0.98f; // the outside web is reachable by a few officials only
            } else if(hub == n.id) {
                dom_e = std::clamp(0.35f + 0.4f * (1.0f - mf_hub) + 0.4f * filt, 0.3f, 0.95f);
                dom_s = std::clamp(0.3f + 0.5f * (1.0f - mf_hub) + 0.5f * filt, 0.3f, 0.97f);
            } else {
                const float trust = w.nation_get_trust(n.id, static_cast<uint16_t>(hub.index()));
                dom_e = std::clamp((0.35f + 0.4f * (1.0f - mf_hub)) * 0.6f * trust, 0.05f, 0.9f);
                dom_s = std::clamp((0.3f + 0.5f * (1.0f - mf_hub)) * 0.6f * trust, 0.05f, 0.9f);
            }
            dom_e += (1.0f - dom_e) * filt;
            dom_s += (1.0f - dom_s) * filt;
        }
        engine_users[s] += users * dom_e;
        engine_users[0] += users * (1.0f - dom_e);
        social_users[s] += social * dom_s;
        social_users[0] += social * (1.0f - dom_s);
    }
    for(std::size_t s = 0; s < dir.sectors.size(); ++s) {
        auto const& sec = dir.sectors[s];
        const float inst = sec.hub ? facts[static_cast<std::size_t>(sec.hub.index())].instability : 0.0f;
        const uint64_t ks = hash(state.rng_seed, text_hash(sec.key));
        site e;
        e.domain = "www." + sec.engine;
        e.name = engine_name(sec.flavour, s == 0 ? hash(state.rng_seed, 0x5EA) : ks);
        e.kind = site_kind::engine;
        e.sector = static_cast<uint8_t>(s);
        e.nation = sec.hub;
        e.users = engine_users[s];
        e.accent = 0x1A73E8 ^ static_cast<uint32_t>(ks & 0x3F3F3F);
        e.down = is_down(state, text_hash(e.domain), 0.3f * inst);
        e.propaganda = sec.hub ? 0.5f * facts[static_cast<std::size_t>(sec.hub.index())].propaganda : 0.0f;
        // Run by a company: global ones on their own, a hub's by its largest firm, a
        // closed intranet's by the state.
        if(sec.closed) {
            e.owner = w.nation_get_agent_from_executive_seat(sec.hub);
            e.owner_label = "State of " + nation_name(state, nm, sec.hub);
            e.propaganda = facts[static_cast<std::size_t>(sec.hub.index())].state_prop;
        } else if(sec.hub) {
            e.owner = largest_board(state, sec.hub, nullptr);
            e.owner_label = e.owner ? board_label(state, nm, e.owner) : e.name + " Group";
        } else {
            e.owner_label = e.name + " Inc.";
        }
        dir.sites.push_back(e);
        site so = e;
        so.domain = "www." + sec.social;
        so.name = social_name(sec.flavour, s == 0 ? hash(state.rng_seed, 0x50C) : ks);
        so.kind = site_kind::social;
        so.users = social_users[s];
        so.accent = 0x3B5998 ^ static_cast<uint32_t>((ks >> 8) & 0x3F3F3F);
        so.down = is_down(state, text_hash(so.domain), 0.3f * inst);
        if(!sec.hub) so.owner_label = so.name + " Inc.";
        dir.sites.push_back(so);
    }

    // National sites.
    static constexpr char const* paper_templates[] = {"The {Adj} Times", "{N} Herald", "The {Adj} Post", "{N} Today", "The {Adj} Tribune",
                                                      "{N} Observer", "The {Adj} Chronicle", "{N} Gazette", "The {Adj} Courier", "{N} Daily News",
                                                      "The {Adj} Standard", "{N} Independent"};
    auto fill = [](std::string t, std::string const& adj, std::string const& name) {
        for(auto [from, to] : {std::pair<std::string, std::string>{"{Adj}", adj}, {"{N}", name}}) {
            for(auto p = t.find(from); p != std::string::npos; p = t.find(from)) t.replace(p, from.size(), to);
        }
        return t;
    };
    for(auto n : w.in_nation) {
        // The calibration slices' "rest of world" is an aggregate, not a place with a press.
        if(core::unpack_key(n.get_iso3()) == "ROW") continue;
        const auto i = static_cast<std::size_t>(n.id.index());
        auto const& f = facts[i];
        const std::string name = nation_name(state, nm, n.id), adj = adjective(name);
        const auto suffixes = dom(n.id);
        const std::string& t = suffixes.commercial;
        const uint64_t kn = hash(state.rng_seed, 0xA77, i);
        const bool closed = dir.sectors[dir.sector_of[i]].closed;
        const auto gov = w.nation_get_agent_from_executive_seat(n.id);
        const std::string gov_label = (w.nation_get_liberal(n.id) >= 0.5f ? "Government of " : "State of ") + name;
        auto state_run = [&](site& x) {
            x.owner = gov;
            x.owner_label = gov_label;
        };
        const double news = f.internet * 0.8; // online news reach (Reuters Institute DNR 2020)
        const float state_share = std::clamp(0.1f + 0.6f * w.nation_get_info_control(n.id) + 0.3f * (1.0f - w.nation_get_media_freedom(n.id)), 0.05f, 0.9f);
        // The state portal.
        site st;
        // gov.uk / gov.cn / bund.de host the portal itself; other government domains (go.jp,
        // gob.mx, the US's bare gov) take the nation's short name: www.japan.go.jp, www.usa.gov.
        {
            const std::string& g = suffixes.government;
            const bool portal = g.rfind("gov.", 0) == 0 || g == "bund.de" || g == "admin.ch" || g == "gouv.fr";
            const std::string short_name = slug(name).size() <= 10 ? slug(name) : lower(core::unpack_key(w.nation_get_iso3(n.id)));
            st.domain = portal ? "www." + g : "www." + short_name + "." + g;
        }
        st.name = w.nation_get_liberal(n.id) >= 0.5f ? "Government of " + name : (adj.empty() ? name : adj) + " State Information Service";
        st.kind = site_kind::state;
        st.nation = n.id;
        st.sector = dir.sector_of[i];
        st.users = news * state_share;
        st.propaganda = f.state_prop;
        st.competence = w.nation_get_state_capacity(n.id);
        st.accent = 0x2E4A7D ^ static_cast<uint32_t>(kn & 0x1F1F1F);
        st.down = is_down(state, text_hash(st.domain), f.instability + 0.4f * std::max(0.0f, 0.35f - st.competence));
        state_run(st);
        dir.sites.push_back(st);
        // Newspapers: one to three by population.
        const float pop = w.nation_get_population(n.id);
        const int papers = closed ? 3 : 1 + (pop > 20e6f ? 1 : 0) + (pop > 80e6f ? 1 : 0);
        static constexpr float weights[3] = {0.5f, 0.3f, 0.2f};
        float wsum = 0.0f;
        for(int k = 0; k < papers; ++k) wsum += weights[k];
        for(int k = 0; k < papers; ++k) {
            // distinct titles within a nation: consecutive templates from a drawn start
            std::size_t ti = (hash(kn, 0) + static_cast<uint64_t>(k) * 5) % std::size(paper_templates);
            // without an adjective: the next templates that use the name itself
            while(adj.empty() && std::string_view(paper_templates[ti]).find("{Adj}") != std::string_view::npos) ti = (ti + 1) % std::size(paper_templates);
            std::string tmpl = paper_templates[ti];
            std::string pname = fill(tmpl, adj, name);
            if(std::any_of(dir.sites.begin(), dir.sites.end(), [&](site const& s) { return s.name == pname; })) pname += k == 1 ? " Online" : " Express";
            site p;
            p.name = pname;
            p.domain = "www." + slug(pname) + "." + t;
            p.kind = site_kind::paper;
            p.nation = n.id;
            p.sector = dir.sector_of[i];
            p.users = news * (1.0f - state_share) * 0.7f * weights[k] / wsum;
            // Outlets differ: the largest follows the line most closely in an unfree press.
            p.propaganda = std::clamp(f.propaganda * (k == 0 ? 1.1f : 1.0f - 0.15f * static_cast<float>(k)), 0.0f, 1.0f);
            p.accent = static_cast<uint32_t>(hash(kn, 77, static_cast<uint64_t>(k)) & 0x7F7F7F);
            p.down = is_down(state, text_hash(p.domain), 0.8f * f.instability);
            // Ownership: the state where the press is captured, a large firm's board
            // for the second title, otherwise independent.
            if(closed || p.propaganda > 0.6f) state_run(p);
            else if(k == 1 && (p.owner = largest_board(state, n.id, nullptr))) p.owner_label = board_label(state, nm, p.owner);
            else p.owner_label = "Independent";
            dir.sites.push_back(p);
        }
        // Regional dailies for the largest regions of multi-region nations.
        std::vector<std::pair<float, dcon::region_id>> regs;
        for(auto own : w.nation_get_region_ownership(n.id)) regs.push_back({own.get_region().get_population(), own.get_region().id});
        if(regs.size() >= 2) {
            std::sort(regs.begin(), regs.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
            for(std::size_t k = 0; k < regs.size() && k < 3; ++k) {
                if(regs[k].first < 3e6f) break;
                const auto r = regs[k].second;
                const std::string rn = region_name(state, nm, r);
                site rd;
                static constexpr char const* rt[] = {"The {N} Daily", "{N} Courier", "{N} Evening Post", "The {N} Record"};
                rd.name = fill(rt[hash(kn, 0x4E6, static_cast<uint64_t>(k)) % std::size(rt)], {}, rn);
                rd.domain = "www." + slug(rd.name) + "." + t;
                rd.kind = site_kind::regional;
                rd.nation = n.id;
                rd.region = r;
                rd.sector = dir.sector_of[i];
                rd.users = f.internet * (regs[k].first / std::max(1.0f, pop)) * 0.25f;
                rd.propaganda = f.propaganda * 0.9f;
                rd.accent = static_cast<uint32_t>(hash(kn, 0x4E7, static_cast<uint64_t>(k)) & 0x6F6F6F);
                const float local = 0.2f * std::max(0.0f, w.region_get_unrest(r) - 2.0f) + (w.region_get_insurgency(r) > 0.0f ? 0.3f : 0.0f);
                rd.down = is_down(state, text_hash(rd.domain), 0.5f * f.instability + local);
                if(closed || rd.propaganda > 0.6f) state_run(rd);
                else rd.owner_label = "Independent";
                dir.sites.push_back(rd);
            }
        }
    }
    // Sports, business briefs, forums and tabloids, nation by nation. Reach (share of
    // internet users, weekly): sport ~15 %, business news ~5 % of the richer half,
    // forums ~10 % where people speak freely, tabloids ~12 % (Reuters Institute DNR 2020
    // ranges); closed and captured nations' are state-run.
    for(auto n : w.in_nation) {
        const auto i = static_cast<std::size_t>(n.id.index());
        auto const& f = facts[i];
        const bool closed = dir.sectors[dir.sector_of[i]].closed;
        const float pop = n.get_population();
        if(pop < 3e6f && !closed) continue;
        const std::string name = nation_name(state, nm, n.id), adj = adjective(name);
        const auto suffixes = dom(n.id);
        const std::string t = suffixes.commercial;
        const uint64_t kn = hash(state.rng_seed, 0xC47, i);
        const auto gov = w.nation_get_agent_from_executive_seat(n.id);
        const std::string gov_label = (n.get_liberal() >= 0.5f ? "Government of " : "State of ") + name;
        auto make = [&](site_kind k, std::initializer_list<char const*> tmpl, double users, float prop, uint64_t salt) {
            auto it = tmpl.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(hash(kn, salt) % tmpl.size()));
            std::string t0 = *it;
            if(adj.empty() && t0.find("{Adj}") != std::string::npos) t0 = "{N} " + std::string(category_name(k));
            site x;
            x.name = fill(t0, adj, name);
            if(std::any_of(dir.sites.begin(), dir.sites.end(), [&](site const& y) { return y.name == x.name; })) x.name += " Online";
            x.domain = "www." + slug(x.name) + "." + t;
            x.kind = k;
            x.nation = n.id;
            x.sector = dir.sector_of[i];
            x.users = users;
            x.propaganda = std::clamp(prop, 0.0f, 1.0f);
            x.accent = static_cast<uint32_t>(hash(kn, salt, 9) & 0x7F7F7F);
            x.down = is_down(state, text_hash(x.domain), 0.8f * f.instability);
            return x;
        };
        {
            site x = make(site_kind::sports, {"{Adj} Sports Network", "{N} Sport", "{Adj} Sport Today", "Goal {N}", "{N} Sports Daily"}, f.internet * 0.15, f.propaganda * 0.5f, 1);
            if(closed || f.propaganda > 0.6f) x.owner = gov, x.owner_label = gov_label;
            else if((x.owner = largest_board(state, n.id, "csv"))) x.owner_label = board_label(state, nm, x.owner);
            else x.owner_label = "Independent";
            dir.sites.push_back(x);
        }
        if(pop >= 10e6f || closed) {
            const float rich = std::clamp(n.get_gdp_pc0() / 20000.0f, 0.1f, 1.0f);
            site x = make(site_kind::business, {"{Adj} Business Brief", "{N} Markets", "{Adj} Financial Review", "{N} Economic Daily"}, f.internet * 0.05 * rich, f.propaganda * 0.6f, 2);
            if(closed || f.propaganda > 0.6f) x.owner = gov, x.owner_label = gov_label;
            else if((x.owner = largest_board(state, n.id, "osv")) || (x.owner = largest_board(state, n.id, nullptr))) x.owner_label = board_label(state, nm, x.owner);
            else x.owner_label = "Independent";
            dir.sites.push_back(x);
        }
        if(n.get_media_freedom() > 0.35f || closed) {
            site x = make(site_kind::forum, closed ? std::initializer_list<char const*>{"{N} Citizens' Discussion Hall", "{Adj} People's Forum"}
                                                   : std::initializer_list<char const*>{"{N} Forum", "{Adj} Boards", "Talk {N}", "{N} Community", "The {Adj} Board"},
                           f.internet * 0.1 * std::max(0.2f, n.get_media_freedom()), closed ? f.state_prop : 0.0f, 3);
            if(closed) x.owner = gov, x.owner_label = gov_label;
            else x.owner_label = "Community-run";
            dir.sites.push_back(x);
        }
        if(pop >= 10e6f || closed) {
            const bool prop = f.propaganda >= 0.5f || closed;
            site x = make(site_kind::tabloid, prop ? std::initializer_list<char const*>{"Patriot {N}", "{N} Truth", "The {Adj} Patriot", "Voice of {N}"}
                                                   : std::initializer_list<char const*>{"{N} Daily Scoop", "The {Adj} Buzz", "{N} Expose", "The {Adj} Gossip"},
                           f.internet * 0.12, prop ? f.state_prop + 0.2f : 0.25f, 4);
            if(prop) x.owner = gov, x.owner_label = gov_label;
            else if((x.owner = largest_board(state, n.id, "csv"))) x.owner_label = board_label(state, nm, x.owner);
            else x.owner_label = "Independent";
            dir.sites.push_back(x);
        }
    }
    // Global sport, business and forum sites (the global web's users).
    {
        double global_users = 0.0;
        for(auto n : w.in_nation) {
            if(dir.sector_of[static_cast<std::size_t>(n.id.index())] == 0) global_users += facts[static_cast<std::size_t>(n.id.index())].internet;
        }
        const uint64_t kg = hash(state.rng_seed, 0x610B);
        auto global = [&](site_kind k, std::string nm2, double share, uint64_t salt) {
            site x;
            x.name = std::move(nm2);
            x.domain = "www." + slug(x.name) + ".com";
            x.kind = k;
            x.users = global_users * share;
            x.accent = static_cast<uint32_t>(hash(kg, salt) & 0x6F6F6F);
            x.owner_label = x.name + (k == site_kind::forum ? " (community-run)" : " Inc.");
            dir.sites.push_back(x);
        };
        static constexpr char const* gs[] = {"World Sport Network", "Global Sports Wire", "Planet Sport"};
        static constexpr char const* gb[] = {"Global Markets Brief", "World Business Wire", "The Markets Daily"};
        static constexpr char const* gf[] = {"Worldtalk", "The Global Board", "Earthforum"};
        global(site_kind::sports, gs[hash(kg, 1) % 3], 0.08, 1);
        global(site_kind::business, gb[hash(kg, 2) % 3], 0.03, 2);
        global(site_kind::forum, gf[hash(kg, 3) % 3], 0.06, 3);
    }
    // Watchdogs: hosted in the largest free nations, read in free nations and, past the
    // filters, in unfree ones.
    {
        std::vector<std::pair<float, dcon::nation_id>> hosts;
        double readers = 0.0;
        for(auto n : w.in_nation) {
            const auto i = static_cast<std::size_t>(n.id.index());
            const float mf = n.get_media_freedom();
            readers += mf > 0.6f ? facts[i].internet * 0.02 : facts[i].internet * 0.01 * (1.0f - n.get_net_filtering());
            if(n.get_liberal() >= 0.75f && mf >= 0.7f && n.get_population() >= 4e6f) hosts.push_back({n.get_population(), n.id});
        }
        std::sort(hosts.begin(), hosts.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        if(hosts.size() > 6) hosts.resize(6);
        static constexpr char const* wt[] = {"{N} Rights Watch", "Free Press Monitor", "Open Society Observer", "Global Liberty Report",
                                             "{Adj} Institute for Democracy", "The Freedom Ledger", "Humanity First", "Independent Voices"};
        double total = 0.0;
        for(auto const& h : hosts) total += std::sqrt(h.first);
        for(std::size_t k = 0; k < hosts.size(); ++k) {
            const auto n = hosts[k].second;
            const std::string name = nation_name(state, nm, n);
            site wd;
            wd.name = fill(wt[(hash(state.rng_seed, 0xD06, k) + k) % std::size(wt)], adj_or_name(name), name);
            if(std::any_of(dir.sites.begin(), dir.sites.end(), [&](site const& s) { return s.name == wd.name; })) wd.name += " (" + name + ")";
            const auto host_org = dom(n).organisation;
            wd.domain = "www." + slug(wd.name) + "." + (host_org == "org" ? std::string("org") : host_org);
            wd.kind = site_kind::watchdog;
            wd.nation = n;
            wd.users = total > 0.0 ? readers * std::sqrt(hosts[k].first) / total : 0.0;
            wd.accent = 0x2D7D46 ^ static_cast<uint32_t>(hash(state.rng_seed, k) & 0x1F1F1F);
            wd.down = is_down(state, text_hash(wd.domain), 0.3f * facts[static_cast<std::size_t>(n.index())].instability);
            wd.owner_label = "Non-profit";
            dir.sites.push_back(wd);
        }
    }
    // Unique domains (generated names can collide): later ones get a number.
    {
        std::vector<std::string> taken;
        for(auto& x : dir.sites) {
            std::string d = x.domain;
            for(int k = 2; std::find(taken.begin(), taken.end(), d) != taken.end(); ++k) {
                const auto dot = x.domain.rfind('.');
                d = x.domain.substr(0, dot) + std::to_string(k) + x.domain.substr(dot);
            }
            x.domain = d;
            taken.push_back(std::move(d));
        }
    }
    std::stable_sort(dir.sites.begin(), dir.sites.end(), [](site const& a, site const& b) { return a.users > b.users; });
    return dir;
}

std::string default_engine(game_state const& state, directory const& dir, std::vector<dcon::nation_id> const& nations) {
    if(dir.sectors.empty()) return {};
    if(nations.empty()) {
        for(auto const& s : dir.sites) {
            if(s.kind == site_kind::engine) return s.domain; // sorted by users
        }
        return "www." + dir.sectors[0].engine;
    }
    // The engine most used across the player's agents' nations: their own sector's
    // domestic share (as in build_directory, by users) against the global engine.
    std::vector<double> use(dir.sectors.size(), 0.0);
    auto const& w = state.world;
    for(auto n : nations) {
        const auto i = static_cast<std::size_t>(n.index());
        const std::size_t s = i < dir.sector_of.size() ? dir.sector_of[i] : 0;
        const float users = facts_of(state, n).internet;
        float dom = 0.0f;
        if(s != 0) {
            const auto hub = dir.sectors[s].hub;
            const float mf = w.nation_get_media_freedom(hub), filt = w.nation_get_net_filtering(n);
            dom = hub == n ? std::clamp(0.35f + 0.4f * (1.0f - mf) + 0.4f * filt, 0.3f, 0.95f)
                           : std::clamp((0.35f + 0.4f * (1.0f - mf)) * 0.6f * w.nation_get_trust(n, static_cast<uint16_t>(hub.index())), 0.05f, 0.9f);
            dom += (1.0f - dom) * filt;
        }
        use[s] += users * dom;
        use[0] += users * (1.0f - dom);
    }
    const auto best = static_cast<std::size_t>(std::max_element(use.begin(), use.end()) - use.begin());
    return "www." + dir.sectors[best].engine;
}

page render(game_state const& state, names const& nm, directory const& dir, std::string const& url_in, std::string const& engine,
            std::vector<dcon::nation_id> const& mine) {
    auto const& w = state.world;
    page pg;
    std::string url = url_in.empty() ? std::string("about:home") : url_in;
    pg.url = url;
    std::string query;
    std::string host = url;
    if(const auto q = url.find("/search?q="); q != std::string::npos) {
        host = url.substr(0, q);
        query = url.substr(q + 10);
    } else if(const auto sl = url.find('/'); sl != std::string::npos && url.rfind("about:", 0) != 0) {
        host = url.substr(0, sl);
    }
    if(host.rfind("http://", 0) == 0) host = host.substr(7);
    if(host.rfind("www.", 0) != 0 && host.rfind("about:", 0) != 0) host = "www." + host;
    const auto events = gather_events(state, nm, 120);
    const uint64_t day_key = static_cast<uint64_t>(std::max(0, state.current_tick));

    auto site_links = [&](std::vector<site const*> const& list, std::size_t max) {
        for(std::size_t i = 0; i < list.size() && i < max; ++i) {
            add(pg, block::kind::link, list[i]->name, list[i]->domain + fmt(" · %.1fM users", list[i]->users / 1e6), list[i]->domain);
        }
    };

    if(host == "about:home" || host == "www.about:home") {
        pg.style = page::layout::home;
        pg.title = "Start Page";
        pg.host = "Start Page";
        add(pg, block::kind::search, "Search the web", engine, engine);
        add(pg, block::kind::heading, "Top sites");
        std::vector<site const*> top;
        for(auto const& s : dir.sites) {
            if(top.size() < 12) top.push_back(&s);
        }
        site_links(top, 12);
        add(pg, block::kind::heading, "Search engines");
        for(auto const& s : dir.sites) {
            if(s.kind == site_kind::engine) add(pg, block::kind::link, s.name, s.domain + fmt(" · %.0fM users", s.users / 1e6), s.domain);
        }
        // The player's own nations first: the browser as a news source.
        std::vector<dcon::nation_id> seen;
        for(auto n : mine) {
            if(!n || std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
            seen.push_back(n);
            add(pg, block::kind::heading, "News from " + nation_name(state, nm, n));
            add_headlines(pg, state, domestic_news(state, nm, events, n, 0.0f, facts_of(state, n), hash(day_key, n.index())), 5);
        }
        add(pg, block::kind::heading, "World headlines");
        add_headlines(pg, state, world_news(state, events, {}, 0.0f), 8);
        return pg;
    }
    site const* s = dir.find(host);
    if(!s) {
        pg.style = page::layout::error;
        pg.title = "Server not found";
        pg.host = host;
        add(pg, block::kind::banner, "Server not found", "", "", -1);
        add(pg, block::kind::text, "The browser can't find the server at " + host + ".");
        add(pg, block::kind::text, "Check the address for typing errors, or search for it.");
        add(pg, block::kind::search, "Search the web", engine, engine);
        return pg;
    }
    pg.title = s->name;
    pg.host = s->name;
    pg.accent = s->accent;
    pg.category = category_name(s->kind);
    pg.owner = s->owner_label;
    {
        // Ads: commercial sites carry the most; state-run and non-profit ones none.
        int lo = 0, hi = 0;
        switch(s->kind) {
        case site_kind::tabloid: lo = 6, hi = 12; break;
        case site_kind::paper:
        case site_kind::regional: lo = 3, hi = 6; break;
        case site_kind::sports: lo = 4, hi = 8; break;
        case site_kind::forum: lo = 3, hi = 5; break;
        case site_kind::business: lo = 2, hi = 4; break;
        case site_kind::social: lo = 2, hi = 4; break;
        case site_kind::engine: lo = 1, hi = 2; break;
        default: break;
        }
        if(s->owner && w.agent_get_kind(s->owner) == static_cast<uint8_t>(agent_kind::executive)) lo = hi = 0;
        pg.ads_blocked = hi > 0 ? lo + static_cast<int>(hash(text_hash(s->domain), day_key) % static_cast<uint64_t>(hi - lo + 1)) : 0;
    }
    if(s->down) {
        pg.style = page::layout::error;
        pg.title = "404 Not Found";
        add(pg, block::kind::banner, "404 Not Found", "", "", -1);
        add(pg, block::kind::text, "The requested URL was not found on this server.");
        add(pg, block::kind::text, "Server at " + s->domain + " Port 80");
        if(s->nation) add(pg, block::kind::text, "(" + nation_name(state, nm, s->nation) + " is in turmoil: its sites come and go.)");
        pg.ads_blocked = 0;
        return pg;
    }
    const std::string sector_name = s->sector < dir.sectors.size() ? dir.sectors[s->sector].key : std::string("global");
    bylines by;
    if(nm.people && !nm.people->empty()) {
        by.pools = nm.people;
        by.mix = s->nation ? culture_mix(state, *nm.people, s->nation) : std::vector<std::pair<int, float>>{};
        if(!s->nation) { // global sites: the world's largest nations' people
            std::vector<std::pair<float, dcon::nation_id>> big;
            for(auto x : w.in_nation) big.push_back({x.get_population(), x.id});
            std::sort(big.begin(), big.end(), [](auto const& a, auto const& b2) { return a.first > b2.first; });
            for(std::size_t k = 0; k < big.size() && k < 12; ++k) {
                for(auto const& m : culture_mix(state, *nm.people, big[k].second)) by.mix.push_back({m.first, m.second});
            }
        }
        by.key = text_hash(s->domain);
    }
    bylines const* byp = by.pools ? &by : nullptr;
    switch(s->kind) {
    case site_kind::engine: {
        pg.style = page::layout::engine;
        add(pg, block::kind::search, s->name + " Search", s->domain, s->domain);
        // Censorship: an engine run under heavy information control hides watchdogs.
        const bool censored = s->nation && w.nation_get_info_control(s->nation) > 0.5f;
        if(query.empty()) {
            add(pg, block::kind::heading, "Trending");
            // headlines of the sector's most popular news sites
            std::vector<headline> trend;
            for(auto const& x : dir.sites) {
                if((x.kind != site_kind::paper && x.kind != site_kind::state) || x.down || (s->sector != 0 && x.sector != s->sector)) continue;
                for(auto const& h : domestic_news(state, nm, events, x.nation, x.propaganda, facts_of(state, x.nation), text_hash(x.domain))) {
                    if(std::none_of(trend.begin(), trend.end(), [&](headline const& t) { return t.text == h.text; })) {
                        trend.push_back(h);
                        break;
                    }
                }
                if(trend.size() >= 8) break;
            }
            add_headlines(pg, state, trend, 8);
            add(pg, block::kind::heading, s->sector == 0 ? "Popular sites" : "Popular in " + nation_name(state, nm, s->nation));
            std::vector<site const*> pop;
            for(auto const& x : dir.sites) {
                if(&x == s || (s->sector != 0 && x.sector != s->sector) || (censored && x.kind == site_kind::watchdog)) continue;
                pop.push_back(&x);
            }
            site_links(pop, 10);
        } else {
            add(pg, block::kind::heading, "Results for \"" + query + "\"");
            std::size_t n_res = 0, removed = 0;
            for(auto const& x : dir.sites) {
                const std::string nation = x.nation ? nation_name(state, nm, x.nation) : std::string{};
                if(!(contains(x.name, query) || contains(x.domain, query) || (!nation.empty() && contains(nation, query)))) continue;
                if(censored && (x.kind == site_kind::watchdog || (x.sector != s->sector && x.kind == site_kind::social))) {
                    ++removed;
                    continue;
                }
                if(n_res++ >= 20) break;
                std::string snippet = x.down ? std::string("This page is not responding.") : std::string{};
                if(snippet.empty() && x.nation) {
                    const auto hs = domestic_news(state, nm, events, x.nation, x.propaganda, facts_of(state, x.nation), text_hash(x.domain));
                    if(!hs.empty()) snippet = hs.front().text;
                }
                pg.blocks.push_back({block::kind::result, x.name, x.domain + (snippet.empty() ? std::string{} : " - " + snippet), x.domain, 0});
            }
            // Headlines mentioning the query.
            for(auto const& e : events) {
                if(n_res >= 30) break;
                if(contains(e.neutral, query)) {
                    pg.blocks.push_back({block::kind::headline, e.neutral, e.tag + " · " + date_of(state, e.tick), {}, 0});
                    ++n_res;
                }
            }
            if(n_res == 0) add(pg, block::kind::text, "Your search did not match any documents.");
            if(removed) add(pg, block::kind::banner, fmt("%.0f results have been removed in accordance with local law.", static_cast<double>(removed)), "", "", 0);
        }
        break;
    }
    case site_kind::social: {
        pg.style = page::layout::social;
        add(pg, block::kind::text, fmt("%.0f million people use ", s->users / 1e6) + s->name + ".");
        add(pg, block::kind::heading, "Trending topics");
        const bool controlled = s->nation && w.nation_get_info_control(s->nation) > 0.5f;
        std::size_t shown = 0, hidden = 0;
        for(auto const& e : events) {
            if(shown >= 10) break;
            if(s->sector != 0) {
                const auto sec = dir.sector_of[static_cast<std::size_t>(e.n.index())];
                if(sec != s->sector) continue;
            }
            if(controlled && (e.sensitive || e.tone < 0) && e.n == s->nation) {
                ++hidden;
                continue;
            }
            std::string tag = "#";
            bool up = true;
            for(char c : e.tag + " " + nation_name(state, nm, e.n)) {
                if(std::isalnum(static_cast<unsigned char>(c))) {
                    tag += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
                    up = false;
                } else {
                    up = true;
                }
            }
            const double posts = s->users * 1e-4 * e.weight * (0.5 + unit(hash(day_key / 7, text_hash(e.neutral))));
            pg.blocks.push_back({block::kind::headline, tag, fmt("%.0fk posts · ", posts / 1e3) + e.neutral
                                     + (byp ? " · top post @" + handle(by.by(text_hash(e.neutral)), text_hash(e.neutral)) : std::string{}), {}, e.tone});
            ++shown;
        }
        if(shown < 5 && s->nation) {
            // Few events: what the hub's own outlets talk about.
            for(auto const& h : domestic_news(state, nm, events, s->nation, facts_of(state, s->nation).propaganda, facts_of(state, s->nation), text_hash(s->domain))) {
                if(shown >= 8) break;
                std::string tag = "#";
                bool up = true;
                for(char c : h.tag + " " + h.text.substr(0, std::min<std::size_t>(h.text.size(), 24))) {
                    if(std::isalnum(static_cast<unsigned char>(c))) {
                        tag += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
                        up = false;
                    } else {
                        up = true;
                    }
                }
                const double posts = s->users * 1e-4 * h.weight * (0.5 + unit(hash(day_key / 7, text_hash(h.text))));
                pg.blocks.push_back({block::kind::headline, tag, fmt("%.0fk posts · ", posts / 1e3) + h.text, {}, h.tone});
                ++shown;
            }
        }
        if(hidden) add(pg, block::kind::banner, "Some topics are unavailable in your region.", "", "", 0);
        if(shown == 0) add(pg, block::kind::text, "Nothing is trending right now.");
        break;
    }
    case site_kind::state: {
        pg.style = page::layout::state;
        const auto n = s->nation;
        if(s->competence < 0.35f) add(pg, block::kind::banner, "This site is under construction. Some services are unavailable.", "", "", 0);
        add(pg, block::kind::text, s->propaganda > 0.6f ? "Official information for citizens. The truth, from the source." : "Official information from the government.");
        if(byp) add(pg, block::kind::text, std::string(s->propaganda > 0.6f ? "Minister of Information: " : "Government spokesperson: ") + by.by(0x5B0));
        add(pg, block::kind::heading, "Announcements");
        auto hs = domestic_news(state, nm, events, n, s->propaganda, facts_of(state, n), text_hash(s->domain));
        // An incompetent portal is stale: only older items.
        if(s->competence < 0.5f) {
            const int32_t stale = state.current_tick - static_cast<int32_t>((0.5f - s->competence) * 120.0f);
            std::erase_if(hs, [&](headline const& h) { return h.tick > stale; });
        }
        add_headlines(pg, state, hs, s->competence < 0.35f ? 4 : 8);
        add(pg, block::kind::heading, "Official statistics");
        for(auto const& st : statistics_of(state, n, s->propaganda)) {
            pg.blocks.push_back({block::kind::stat, st.label, fmt("%.1f", st.official) + st.unit, {}, 0});
        }
        break;
    }
    case site_kind::paper:
    case site_kind::regional: {
        pg.style = page::layout::news;
        const auto n = s->nation;
        auto const f = facts_of(state, n);
        if(s->kind == site_kind::regional) {
            add(pg, block::kind::text, "News for " + region_name(state, nm, s->region) + ", " + nation_name(state, nm, n));
            if(w.region_get_insurgency(s->region) > 0.0f && s->propaganda < 0.5f) add(pg, block::kind::headline, "Fighting reported near " + region_name(state, nm, s->region), "Conflict · " + date_of(state, state.current_tick), {}, -1);
            if(w.region_get_unrest(s->region) > 2.5f && s->propaganda < 0.5f) add(pg, block::kind::headline, "Unrest grips " + region_name(state, nm, s->region), "Society · " + date_of(state, state.current_tick), {}, -1);
        }
        add(pg, block::kind::heading, nation_name(state, nm, n));
        add_headlines(pg, state, domestic_news(state, nm, events, n, s->propaganda, f, text_hash(s->domain)), 8, {}, byp);
        add(pg, block::kind::heading, "World");
        add_headlines(pg, state, world_news(state, events, n, s->propaganda), 6, {}, byp);
        add(pg, block::kind::heading, "Figures");
        for(auto const& st : statistics_of(state, n, s->propaganda * 0.7f)) {
            if(st.label == std::string("Government approval")) continue;
            pg.blocks.push_back({block::kind::stat, st.label, fmt("%.1f", st.official) + st.unit, {}, 0});
        }
        break;
    }
    case site_kind::watchdog: {
        pg.style = page::layout::watchdog;
        add(pg, block::kind::text, "Independent monitoring of rights, freedoms and the facts governments hide. Based in " + nation_name(state, nm, s->nation) + "."
                                       + (byp ? " Director: " + by.by(0xD1) + "." : std::string{}));
        add(pg, block::kind::heading, "Countries of concern");
        struct concern {
            float score;
            dcon::nation_id n;
        };
        std::vector<concern> cs;
        for(auto x : w.in_nation) {
            if(x.get_population() < 1e6f) continue;
            bool war = false;
            for(auto m : w.in_nation) war = war || x.get_at_war(static_cast<uint16_t>(m.id.index())) > 0.0f;
            const float score = (1.0f - x.get_liberal()) + x.get_repression() + std::max(0.0f, x.get_info_control() - x.get_info_control0()) * 2.0f
                                + (war ? 0.5f : 0.0f) + (x.get_rebel_agent() && x.get_rebel_agent().get_strength() > 0.0f ? 0.4f : 0.0f);
            cs.push_back({score, x.id});
        }
        std::sort(cs.begin(), cs.end(), [](concern const& a, concern const& b) { return a.score > b.score; });
        for(std::size_t k = 0; k < cs.size() && k < 8; ++k) {
            const auto n = cs[k].n;
            const std::string A = nation_name(state, nm, n);
            auto const f = facts_of(state, n);
            std::string line;
            if(w.nation_get_repression(n) > 0.3f) line = A + ": protesters met with force";
            else if(w.nation_get_info_control(n) > w.nation_get_info_control0(n) + 0.05f) line = A + " tightens its grip on online media";
            else if(w.nation_get_rebel_agent(n) && w.agent_get_strength(w.nation_get_rebel_agent(n)) > 0.0f) line = A + ": civilians caught between the army and rebels";
            else line = A + ": a press that cannot report freely";
            add(pg, block::kind::headline, line, fmt("Freedom score %.0f/100", 100.0 * w.nation_get_liberal(n)), {}, -1);
            // The facts behind the official figures.
            auto const st = statistics_of(state, n, f.state_prop);
            if(f.state_prop > 0.3f) {
                add(pg, block::kind::stat, "Our estimate of unemployment in " + A, fmt("%.1f%%", st[0].real) + fmt(" (official %.1f%%)", st[0].official));
            }
        }
        add(pg, block::kind::heading, "Latest");
        std::vector<headline> latest;
        for(auto const& e : events) {
            if(e.tone < 0 && e.sensitive) latest.push_back({e.neutral, e.tag, e.tick, -1, e.weight});
        }
        add_headlines(pg, state, latest, 8);
        break;
    }
    case site_kind::sports: {
        pg.style = page::layout::sports;
        const auto n = s->nation;
        add(pg, block::kind::heading, "Major events");
        std::vector<headline> majors;
        for(auto const& e : gather_events(state, nm, 365)) {
            if(e.tag == "Sports") majors.push_back({e.neutral, e.tag, e.tick, e.tone, e.weight});
        }
        if(majors.empty()) add(pg, block::kind::text, "No major tournaments in the past year.");
        add_headlines(pg, state, majors, 6);
        // Results: procedural fixtures, stronger sides (larger, richer) winning more often;
        // controlled outlets never report a defeat.
        add(pg, block::kind::heading, n ? "National team results" : "International results");
        std::vector<dcon::nation_id> pool;
        for(auto x : w.in_nation) {
            if(x.get_population() >= 5e6f) pool.push_back(x.id);
        }
        const uint64_t week = day_key / 7;
        for(int k = 0; k < 6 && pool.size() > 2; ++k) {
            const uint64_t h = hash(text_hash(s->domain), week, static_cast<uint64_t>(k));
            dcon::nation_id a = n ? n : pool[h % pool.size()], b = pool[(h >> 20) % pool.size()];
            if(a == b) continue;
            auto strength = [&](dcon::nation_id x) { return std::log10(std::max(1.0f, w.nation_get_population(x))) + 0.5f * std::log10(std::max(100.0f, w.nation_get_gdp_pc0(x))); };
            const float pa = std::clamp(0.45f + 0.25f * (strength(a) - strength(b)), 0.1f, 0.85f);
            const float u = unit(hash(h, 1));
            int ga = static_cast<int>(hash(h, 2) % 4), gb = static_cast<int>(hash(h, 3) % 4);
            if(u < pa) ga = std::max(ga, gb + 1);
            else if(u < pa + 0.25f) gb = ga;
            else gb = std::max(gb, ga + 1);
            if(n && s->propaganda > 0.6f && gb > ga) continue; // defeats go unreported
            const std::string A = adj_or_name(nation_name(state, nm, a)), B = nation_name(state, nm, b);
            const char* verb = ga > gb ? " beat " : ga == gb ? " draw with " : " lose to ";
            std::string scorer;
            if(nm.people && ga > 0) {
                const auto who = person(*nm.people, culture_mix(state, *nm.people, a), hash(h, 7));
                if(!who.empty()) scorer = " · " + who.substr(who.find(' ') + 1) + (ga > 1 ? fmt(" (%.0f)", static_cast<double>(std::min(ga, 2))) : std::string{});
            }
            add(pg, block::kind::headline, A + verb + B + " " + std::to_string(ga) + "-" + std::to_string(gb), "Football · " + date_of(state, state.current_tick - static_cast<int32_t>(k * 3)) + scorer, {},
                n ? static_cast<int8_t>(ga > gb ? 1 : ga < gb ? -1 : 0) : int8_t(0));
        }
        break;
    }
    case site_kind::forum: {
        pg.style = page::layout::forum;
        const auto n = s->nation;
        add(pg, block::kind::text, s->propaganda > 0.5f ? "Discuss the achievements of our nation. Posts are reviewed before publication." : "Community discussion. Be civil.");
        add(pg, block::kind::heading, "Latest threads");
        static constexpr char const* openers[] = {"[Discussion] ", "Anyone else following this? ", "Thoughts on: ", "Megathread: ", "So... "};
        std::size_t shown = 0;
        auto const hs = n ? domestic_news(state, nm, events, n, s->propaganda, facts_of(state, n), text_hash(s->domain)) : world_news(state, events, {}, 0.0f);
        for(auto const& h : hs) {
            if(shown >= 8) break;
            const uint64_t k = hash(text_hash(h.text), text_hash(s->domain));
            const double replies = 5.0 + static_cast<double>(k % 400) * h.weight;
            const std::string author = byp ? " · posted by " + handle(by.by(k), k) : std::string{};
            add(pg, block::kind::headline, std::string(openers[k % std::size(openers)]) + h.text, fmt("%.0f replies", replies) + author, {}, h.tone);
            ++shown;
        }
        add(pg, block::kind::heading, "Off-topic");
        static constexpr char const* offtopic[] = {"What are you cooking this weekend?", "Best budget phone this year?", "Post your view from the window",
                                                   "Football transfer rumours thread", "Recommend me a book", "Weather thread: is it just me?",
                                                   "Old photos of {N} (text descriptions only, the mods insist)", "How do you fix a squeaky door?"};
        const std::string N = n ? nation_name(state, nm, n) : std::string("the world");
        for(int k = 0; k < 4; ++k) {
            std::string t = offtopic[hash(text_hash(s->domain), day_key / 7, static_cast<uint64_t>(k)) % std::size(offtopic)];
            if(const auto p = t.find("{N}"); p != std::string::npos) t.replace(p, 3, N);
            add(pg, block::kind::headline, t, fmt("%.0f replies", static_cast<double>(hash(text_hash(t), day_key) % 200)), {}, 0);
        }
        break;
    }
    case site_kind::business: {
        pg.style = page::layout::business;
        const auto n = s->nation;
        if(!n) {
            add(pg, block::kind::heading, "Largest economies");
            std::vector<std::pair<double, dcon::nation_id>> big;
            for(auto x : w.in_nation) big.push_back({x.get_gdp0(), x.id});
            std::sort(big.begin(), big.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
            for(std::size_t k = 0; k < big.size() && k < 10; ++k) {
                const auto x = big[k].second;
                add(pg, block::kind::stat, nation_name(state, nm, x),
                    fmt("output %+.1f%% vs normal", 100.0 * w.nation_get_gap_smoothed(x)) + fmt(", unemployment %.1f%%", 100.0 * w.nation_get_unemployment(x)));
            }
            add(pg, block::kind::heading, "Markets news");
            std::vector<headline> mk;
            for(auto const& e : events) {
                if(e.tag == "Markets" || e.tag == "Sanctions" || e.tag == "Diplomacy") mk.push_back({e.neutral, e.tag, e.tick, 0, e.weight});
            }
            add_headlines(pg, state, mk, 8);
            break;
        }
        const float prop = s->propaganda;
        add(pg, block::kind::heading, "Markets");
        add(pg, block::kind::stat, "Policy rate", fmt("%.2f%%", w.nation_get_policy_rate(n)));
        add(pg, block::kind::stat, "10-year yield", fmt("%.2f%%", w.nation_get_yield10(n)));
        add(pg, block::kind::stat, "Exchange rate vs start", fmt("%+.1f%%", 100.0 * (w.nation_get_exchange_rate(n) - 1.0f) + (prop > 0.5f ? 5.0 * prop : 0.0)));
        add(pg, block::kind::stat, "Share prices vs start", fmt("%+.1f%%", 100.0 * (w.nation_get_equity_price(n) - 1.0f) + 10.0 * prop));
        add(pg, block::kind::stat, "House prices vs start", fmt("%+.1f%%", 100.0 * (w.nation_get_house_price(n) - 1.0f)));
        add(pg, block::kind::stat, "Credit growth", fmt("%.1f%%", 100.0 * w.nation_get_credit_growth(n)));
        add(pg, block::kind::stat, "Bad loans", fmt("%.1f%%", 100.0 * w.nation_get_npl(n) * (1.0f - 0.5f * prop)));
        add(pg, block::kind::heading, "Companies");
        std::size_t shown = 0;
        for(auto fm : w.in_firm) {
            if(fm.get_nation() != n || !fm.get_board() || shown >= 5) continue;
            const std::string label = board_label(state, nm, fm.get_board().id);
            const float cash = fm.get_cash_days0() > 0.0f ? fm.get_cash_days() / fm.get_cash_days0() : 1.0f;
            ++shown;
            if(cash < 0.4f && prop < 0.5f) add(pg, block::kind::headline, label + " warns of a cash crunch", "Companies · " + date_of(state, state.current_tick), {}, -1);
            else if(fm.get_investment_drive() > 0.1f) add(pg, block::kind::headline, label + " steps up investment", "Companies · " + date_of(state, state.current_tick), {}, 1);
            else if(fm.get_hoarding() < 0.3f && w.nation_get_unemployment(n) > w.nation_get_unemployment0(n) + 0.01f && prop < 0.5f)
                add(pg, block::kind::headline, label + " cuts jobs", "Companies · " + date_of(state, state.current_tick), {}, -1);
            else add(pg, block::kind::headline, label + fmt(": %.0f days of cash on hand", 30.0 * cash), "Companies · " + date_of(state, state.current_tick), {}, 0);
        }
        add(pg, block::kind::heading, "Economy");
        auto hs = domestic_news(state, nm, events, n, prop, facts_of(state, n), text_hash(s->domain));
        std::erase_if(hs, [](headline const& h) { return h.tag != "Economy" && h.tag != "Markets" && h.tag != "Sanctions"; });
        add_headlines(pg, state, hs, 6);
        break;
    }
    case site_kind::tabloid: {
        pg.style = page::layout::tabloid;
        const auto n = s->nation;
        const std::string A = nation_name(state, nm, n), Adj = adj_or_name(A);
        if(s->propaganda >= 0.5f) {
            // A propaganda sheet: the rivals' troubles, real or invented, and national glory.
            std::vector<std::pair<float, dcon::nation_id>> rivals;
            for(auto x : w.in_nation) {
                if(x.id == n || x.get_population() < 5e6f) continue;
                const float t = w.nation_get_trust(n, static_cast<uint16_t>(x.id.index()));
                if(t < 0.35f) rivals.push_back({t, x.id});
            }
            std::sort(rivals.begin(), rivals.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
            add(pg, block::kind::heading, "THE TRUTH THEY HIDE");
            for(std::size_t k = 0; k < rivals.size() && k < 5; ++k) {
                const auto r = rivals[k].second;
                const std::string R = nation_name(state, nm, r);
                std::string t;
                for(auto const& e : events) {
                    if(e.n == r && e.tone < 0) {
                        t = "CHAOS IN " + R + ": " + e.neutral;
                        break;
                    }
                }
                if(t.empty()) {
                    static constexpr char const* lies[] = {"{R} ON THE BRINK OF COLLAPSE", "{R}'S ECONOMY IN RUINS, insiders say", "Citizens of {R} flee in their thousands",
                                                           "{R} leaders panic as their plots fail"};
                    t = lies[hash(text_hash(s->domain), r.index(), day_key / 7) % std::size(lies)];
                    t.replace(t.find("{R}"), 3, R);
                }
                add(pg, block::kind::headline, t, "Exclusive · " + date_of(state, state.current_tick), {}, -1);
            }
            add(pg, block::kind::heading, "GLORY");
            auto hs = domestic_news(state, nm, events, n, 1.0f, facts_of(state, n), text_hash(s->domain));
            std::erase_if(hs, [](headline const& h) { return h.tone < 0; });
            add_headlines(pg, state, hs, 5);
        } else {
            // A sensational tabloid in a free press: the same news, louder.
            add(pg, block::kind::heading, "SHOCK NEWS");
            auto hs = domestic_news(state, nm, events, n, 0.0f, facts_of(state, n), text_hash(s->domain));
            std::size_t shown = 0;
            for(auto const& h : hs) {
                if(shown >= 6) break;
                std::string t = h.text;
                for(auto& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                add(pg, block::kind::headline, (h.tone < 0 ? "SHOCK: " : "AMAZING: ") + t + "!",
                    h.tag + " · " + date_of(state, h.tick) + (byp ? " · by " + by.by(text_hash(h.text)) : std::string{}), {}, h.tone);
                ++shown;
            }
            add(pg, block::kind::headline, fmt("Leader's popularity at %.0f%%: what it means for YOU", 100.0 * w.nation_get_approval(n)), "Politics · " + date_of(state, state.current_tick), {}, 0);
            add(pg, block::kind::heading, "Around the world");
            add_headlines(pg, state, world_news(state, events, n, 0.0f), 4);
        }
        break;
    }
    }
    // Footer: more from the same nation or sector.
    add(pg, block::kind::rule, "");
    add(pg, block::kind::heading, "More sites");
    std::vector<site const*> more;
    for(auto const& x : dir.sites) {
        if(&x == s) continue;
        if((s->nation && x.nation == s->nation) || (!s->nation && x.sector == s->sector)) more.push_back(&x);
    }
    if(more.size() < 3) {
        for(auto const& x : dir.sites) {
            if(&x != s && std::find(more.begin(), more.end(), &x) == more.end() && more.size() < 6) more.push_back(&x);
        }
    }
    site_links(more, 6);
    (void)engine_of_sector;
    return pg;
}

} // namespace geosim::sim::web
