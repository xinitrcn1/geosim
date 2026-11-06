#pragma once
// The in-game internet (docs/08-ui.md#internet): procedurally generated websites that
// report on the simulated world, built from the state on demand (read-only, at the
// sim's sync point). Nothing is stored: sites, names and audiences are derived from
// tracked variables, deterministic for a game (rng_seed) and its state.
//
// - Sectors: the global web, walled-off hubs with their own search engine and social
//   network (China; Russia and its neighbours; a small moddable table), and closed
//   national intranets where press freedom and liberties are near zero (derived; a
//   DPRK-like state runs every site on it).
// - Sites per nation: a state portal (availability and competence from stability,
//   legitimacy and state capacity; propaganda from liberties and information control),
//   one to three newspapers named after the nation ("The Austrian Times"), regional
//   dailies for the largest regions, and watchdogs hosted in free nations that report
//   on unfree ones. Sites go 404 when their nation or region is unstable.
// - Audiences (active users) are derived: internet users = population x social-media
//   share / 0.84 (DataReportal 2020: social-media users are ~84 % of internet users);
//   a hub's own engine and network take a share growing with low media freedom and
//   filtering (Jiang 2014 on Baidu behind the firewall; Yandex's ~55-60 % in Russia);
//   the state outlet's share of online news grows with information control (Reuters
//   Institute Digital News Report 2020; RSF World Press Freedom Index).

#include "sim/game_state.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geosim::sim::web {

// Categories: search engines and social networks (per sector), state portals, newspapers,
// regional dailies, watchdogs, sports, forums, business briefs and tabloids (sensational
// in free nations, propaganda sheets in unfree ones); companies (the largest firms), stock
// exchanges, central banks' exchange rates, torrent trackers (where the black market is
// large and copyright enforcement weak, or speech is protected) and anti-piracy groups
// (copyright-strong nations' rights holders).
enum class site_kind : uint8_t { engine, social, state, paper, regional, watchdog, sports, forum, business, tabloid,
                                 company, exchange, rates, torrent, antipiracy };
[[nodiscard]] char const* category_name(site_kind k);

struct site {
    std::string domain, name;
    site_kind kind = site_kind::paper;
    uint8_t sector = 0;      // index into directory::sectors
    dcon::nation_id nation;  // home nation (global engines and networks: none)
    dcon::region_id region;  // regional dailies
    double users = 0;        // active users (people)
    float propaganda = 0;    // 0 reports reality .. 1 pure propaganda
    float competence = 1;    // upkeep and layout (state portals)
    bool down = false;       // 404 this week
    uint32_t accent = 0x3366CC;
    dcon::agent_id owner;    // who runs it: a government (executive) or a firm's board; none: independent
    std::string owner_label; // "Government of Austria", "Austrian Manufacturing Holdings", "independent"
    dcon::firm_id firm;      // company sites: the firm
};

struct sector {
    std::string key;
    dcon::nation_id hub;                  // none: the global web
    std::vector<dcon::nation_id> members; // empty for the global web (everyone else)
    std::string engine, social;           // their domains
    char flavour = 'g';                   // name pools: g global, s Chinese, r Russian, i closed intranet
    bool closed = false;                  // a national intranet: every site state-run
};

struct directory {
    std::vector<sector> sectors;          // [0] global
    std::vector<site> sites;              // by users, most first
    std::vector<uint8_t> sector_of;       // per nation index
    [[nodiscard]] site const* find(std::string const& domain) const;
};

// People's names (assets/defs/names.csv, name_cultures.csv): pools of given names and
// surnames per culture, and which cultures each identity group (social_group key, by
// nation or any nation) and each nation's default population use. A nation's names are
// drawn by its groups' current population, so who lives there decides how its
// journalists, officials and forum users are called.
struct name_pools {
    std::vector<std::string> cultures;
    std::vector<std::vector<std::string>> given, surname; // per culture
    struct rule {
        std::string iso3, group; // "*" = any nation / the nation's default population
        int culture = 0;
        float weight = 1;
    };
    std::vector<rule> rules;
    [[nodiscard]] bool empty() const noexcept { return cultures.empty(); }
};
// Loads both files from `defs_dir` (assets/defs); false with the reason in `error`.
bool load_name_pools(std::string const& defs_dir, name_pools& out, std::string& error);
// The cultures of nation `n` with their weights (its groups' population).
[[nodiscard]] std::vector<std::pair<int, float>> culture_mix(game_state const& state, name_pools const& pools, dcon::nation_id n);
// A person of that mix ("Given Surname"; "" without pools), deterministic in `seed`.
[[nodiscard]] std::string person(name_pools const& pools, std::vector<std::pair<int, float>> const& mix, uint64_t seed);

// Internet domains per nation (assets/defs/tlds.csv): the ccTLD and the second-level
// domains of commercial, government, organisation and academic sites (co.uk, gov.uk,
// org.uk, ac.uk). Nations without a ccTLD, or not listed, use com.<iso3> and so on.
struct domain_table {
    struct entry {
        std::string iso3, tld, commercial, government, organisation, academic;
    };
    std::vector<entry> entries;
    [[nodiscard]] entry of(std::string const& iso3) const;
};
bool load_domain_table(std::string const& defs_dir, domain_table& out, std::string& error);

// Text pools (assets/defs/web_text.csv: kind, text): social posts by style, username parts,
// torrent names and titles. Moddable; missing kinds fall back to a built-in line.
struct web_text {
    std::vector<std::pair<std::string, std::vector<std::string>>> pools;
    // Per pool, per line: how a post renders its topic {T} (the optional transform column:
    // headline, lossy, loud, serious, stupid, fear, place; "" = headline).
    std::vector<std::vector<std::string>> transforms;
    [[nodiscard]] std::vector<std::string> const* of(std::string_view kind) const;
    [[nodiscard]] std::string const* transform_of(std::string_view kind, std::size_t line) const;
};
bool load_web_text(std::string const& defs_dir, web_text& out, std::string& error);

// Display names (nations.csv names by nation index, region names by region index);
// empty entries fall back to codes. `people`: name pools (optional).
struct names {
    std::vector<std::string> const* nations = nullptr;
    std::vector<std::string> const* regions = nullptr;
    name_pools const* people = nullptr;
    domain_table const* domains = nullptr;
    web_text const* text = nullptr;
};

[[nodiscard]] directory build_directory(game_state const& state, names const& nm);

// Page content: a list of blocks the browser lays out.
struct block {
    enum class kind : uint8_t { heading, text, headline, link, stat, banner, rule, search, result, post };
    kind k = kind::text;
    std::string text, sub, url; // headline: text + sub (dateline / tag); link and result: url
    int8_t tone = 0;            // -1 bad, 0 neutral, 1 good; banner: -1 error, 0 notice
    std::string author{}, iso3{}; // post: the poster's name and nation (its flag)
    std::string slang{}, gloss{};  // post: a slang term kept through the translator, and its meaning
    int16_t replies = 0;           // post: replies that follow it (blocks with `reply` set)
    bool reply = false;            // post: a reply to the last post without `reply`
};
struct page {
    std::string url, title, host; // host: the site's name ("The Austrian Times")
    uint32_t accent = 0x3366CC;
    enum class layout : uint8_t { home, engine, social, news, state, watchdog, sports, forum, business, tabloid, error,
                                  company, exchange, torrent } style = layout::news;
    std::string category;          // the site's category ("News", "Sports", ...)
    std::string owner;             // "Run by ..."
    int ads_blocked = 0;           // advertisements the browser's blocker removed (pages are text only)
    std::vector<block> blocks;
};

// The engine most used across `nations` (weighted by internet users), or worldwide when
// the list is empty: its domain.
[[nodiscard]] std::string default_engine(game_state const& state, directory const& dir, std::vector<dcon::nation_id> const& nations);

// Renders `url` ("about:home", a site's domain, or an engine's domain + "/search?q=...").
// `mine`: the player's agents' nations (the start page's own news section).
[[nodiscard]] page render(game_state const& state, names const& nm, directory const& dir, std::string const& url, std::string const& engine,
                          std::vector<dcon::nation_id> const& mine = {});

// A firm's name, deterministic for a game: the nation, a place (one of its most populous
// regions) or a founder's surname (name pools), a word of its sector and a corporate suffix
// ("Herat Construction Group", "Karimi Agro", "Afghan Energy Corporation").
[[nodiscard]] std::string firm_name(game_state const& state, names const& nm, dcon::firm_id f);

// Piracy and copyright (the torrent and anti-piracy sites; docs/08-ui.md#internet).
struct piracy_facts {
    float rate = 0;        // share of software and media used unlicensed
    float enforcement = 0; // copyright enforcement 0..1
    float hosting = 0;     // attractiveness as a lax torrent host (a large pirate audience, weak enforcement)
    float haven = 0;       // as a free-speech haven (strong speech and privacy protection, a weak rights lobby)
    float copyright = 0;   // strength of the rights-holder lobby 0..1
};
[[nodiscard]] piracy_facts piracy_of(game_state const& state, dcon::nation_id n);

// The adjective of a nation's name ("Austria" -> "Austrian"); empty when unknown.
[[nodiscard]] std::string adjective(std::string const& name);

} // namespace geosim::sim::web
