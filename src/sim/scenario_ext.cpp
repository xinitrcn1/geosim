// Scenario extension loaders: the container-shipping network, non-oil commodity
// markets, and per-nation trade tables (ports, overland shares, logistics,
// commodity shares). Every table is optional: without it the mechanism runs on
// defaults (the 2020 slice is a calibration device; the full-world data comes
// later and must not need code changes).

#include "core/calendar.hpp"
#include "core/csv.hpp"
#include "core/defs.hpp"
#include "core/keys.hpp"
#include "sim/model.hpp"
#include "sim/scenario.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <numbers>
#include <sstream>
#include <system_error>

namespace geosim::sim {

namespace {

bool exists(std::string const& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

dcon::sea_node_id find_node(game_state const& state, std::string_view key) {
    const auto k = core::pack_key(key);
    for(auto n : state.world.in_sea_node) {
        if(n.get_key() == k) return n.id;
    }
    return {};
}

dcon::chokepoint_id find_chokepoint(game_state const& state, std::string_view key) {
    const auto k = core::pack_key(key);
    for(auto c : state.world.in_chokepoint) {
        if(c.get_key() == k) return c.id;
    }
    return {};
}

float great_circle_km(float lat1, float lon1, float lat2, float lon2) {
    constexpr float rad = std::numbers::pi_v<float> / 180.0f;
    const float la1 = lat1 * rad, la2 = lat2 * rad;
    const float dlat = la2 - la1, dlon = (lon2 - lon1) * rad;
    const float h = std::sin(dlat / 2) * std::sin(dlat / 2) + std::cos(la1) * std::cos(la2) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0f * 6371.0f * std::asin(std::min(1.0f, std::sqrt(h)));
}

} // namespace

void load_sea_network(game_state& state, std::string const& assets_dir, core::diagnostics& diag) {
    const std::string path = assets_dir + "/defs/sea_lanes.txt";
    if(!exists(path)) {
        return; // no logistics network: trade uses great-circle distances
    }
    auto& w = state.world;
    const auto nodes = core::load_defs(path, diag);
    for(auto const& node : nodes) {
        if(node.key == "node") {
            const auto key = node.req("key", path, diag);
            if(key.empty() || key.size() > 4 || find_node(state, key)) {
                diag.fail(path, node.line, "sea node keys must be unique, 1-4 characters");
                return;
            }
            const auto id = w.create_sea_node();
            w.sea_node_set_key(id, core::pack_key(key));
            w.sea_node_set_lat(id, static_cast<float>(node.num_or("lat", 0.0, path, diag)));
            w.sea_node_set_lon(id, static_cast<float>(node.num_or("lon", 0.0, path, diag)));
        } else if(node.key == "chokepoint") {
            const auto key = node.req("key", path, diag);
            if(key.empty() || key.size() > 4 || find_chokepoint(state, key)) {
                diag.fail(path, node.line, "chokepoint keys must be unique, 1-4 characters");
                return;
            }
            const auto id = w.create_chokepoint();
            auto num = [&](char const* k, double d) { return static_cast<float>(node.num_or(k, d, path, diag)); };
            w.chokepoint_set_key(id, core::pack_key(key));
            w.chokepoint_set_capacity(id, num("capacity", 100.0));
            w.chokepoint_set_traffic0(id, num("traffic", 50.0));
            w.chokepoint_set_traffic(id, num("traffic", 50.0));
            w.chokepoint_set_canal_days(id, num("canal_days", 0.0));
            w.chokepoint_set_incident_rate(id, num("incident_rate", 0.0));
            w.chokepoint_set_incident_cut(id, std::clamp(num("incident_cut", 1.0), 0.0f, 1.0f));
            w.chokepoint_set_size_elasticity(id, num("size_elasticity", 0.0));
            w.chokepoint_set_duration_median(id, num("duration_median", 1.0));
            w.chokepoint_set_duration_sigma(id, num("duration_sigma", 0.5));
            w.chokepoint_set_season_amp(id, num("season_amp", 0.0));
            w.chokepoint_set_season_peak(id, num("season_peak", 0.0));
            w.chokepoint_set_drought_rate(id, num("drought_rate", 0.0));
            w.chokepoint_set_drought_cut(id, std::clamp(num("drought_cut", 0.0), 0.0f, 1.0f));
            w.chokepoint_set_drought_days(id, num("drought_days", 0.0));
            w.chokepoint_set_open(id, 1.0f);
            w.chokepoint_set_incident_open(id, 1.0f);
            if(auto lit = node.find("littoral")) {
                std::istringstream ss(lit->text);
                std::string iso;
                while(ss >> iso) {
                    if(auto n = find_nation(state, iso)) w.force_create_littoral(id, n); // others: not in this scenario
                }
            }
        }
    }
    for(auto const& node : nodes) {
        if(node.key != "lane") continue;
        const auto a = find_node(state, node.req("a", path, diag));
        const auto b = find_node(state, node.req("b", path, diag));
        if(!a || !b) {
            diag.fail(path, node.line, "lane between unknown sea nodes");
            return;
        }
        const auto id = w.create_sea_lane();
        w.sea_lane_set_from(id, a);
        w.sea_lane_set_to(id, b);
        w.sea_lane_set_nm(id, static_cast<float>(node.num_or("nm", 1000.0, path, diag)));
        if(auto c = node.find("chokepoint")) {
            const auto cp = find_chokepoint(state, c->text);
            if(!cp) {
                diag.fail(path, c->line, "unknown chokepoint '" + c->text + "'");
                return;
            }
            w.sea_lane_set_chokepoint(id, cp);
        }
    }
    if(w.sea_node_size() > 0 && w.shipping_size() == 0) {
        (void)w.create_shipping();
    }
}

void load_commodity_defs(game_state& state, std::string const& assets_dir, core::diagnostics& diag) {
    const std::string path = assets_dir + "/defs/commodities.txt";
    if(!exists(path)) {
        return;
    }
    auto& w = state.world;
    const auto S = static_cast<uint32_t>(w.sector_size());
    w.commodity_resize_sector_weight(S);
    for(auto const& node : core::load_defs(path, diag)) {
        if(node.key != "commodity") {
            diag.fail(path, node.line, "expected commodity{...}");
            return;
        }
        const auto key = node.req("key", path, diag);
        if(key.empty() || key.size() > 4 || key == "oil") {
            diag.fail(path, node.line, "commodity keys must be 1-4 characters and not 'oil' (crude has its own market)");
            return;
        }
        const auto id = w.create_commodity();
        auto num = [&](char const* k, double d) { return static_cast<float>(node.num_or(k, d, path, diag)); };
        w.commodity_set_key(id, core::pack_key(key));
        w.commodity_set_price0(id, 1.0f);
        w.commodity_set_price(id, 1.0f);
        w.commodity_set_demand0(id, 1.0f); // world-demand units
        w.commodity_set_world_demand(id, 1.0f);
        w.commodity_set_supply(id, 1.0f);
        w.commodity_set_supply_index(id, 1.0f);
        w.commodity_set_harvest(id, 1.0f);
        w.commodity_set_world_value(id, num("world_value_bn", 100.0));
        w.commodity_set_demand_elasticity(id, num("demand_elasticity", -0.1));
        w.commodity_set_supply_elasticity(id, num("supply_elasticity", 0.05));
        w.commodity_set_inventory_days(id, num("inventory_days", 30.0));
        w.commodity_set_flow_sensitivity(id, num("flow_sensitivity", 3.0));
        w.commodity_set_inventory_sensitivity(id, num("inventory_sensitivity", 1.0));
        w.commodity_set_price_speed(id, num("price_speed", 0.05));
        w.commodity_set_harvest_sd(id, num("harvest_sd", 0.0));
        w.commodity_set_biofuel_share(id, num("biofuel_share", 0.0));
        w.commodity_set_income_elasticity(id, num("income_elasticity", 0.0));
        if(auto wt = node.find("weights")) {
            for(auto const& c : wt->children) {
                bool found = false;
                for(auto s : w.in_sector) {
                    if(s.get_key() == core::pack_key(c.key)) {
                        w.commodity_set_sector_weight(id, static_cast<uint8_t>(s.id.index()),
                                                      static_cast<float>(core::parse_number(c.text, path, c.line, diag).value_or(0.0)));
                        found = true;
                    }
                }
                if(!found) {
                    diag.fail(path, c.line, "unknown sector '" + c.key + "'");
                    return;
                }
            }
        }
    }
}

void load_technology_defs(game_state& state, std::string const& assets_dir, core::diagnostics& diag) {
    const std::string path = assets_dir + "/defs/technologies.txt";
    if(!exists(path)) {
        return;
    }
    auto& w = state.world;
    for(auto const& node : core::load_defs(path, diag)) {
        if(node.key != "tech") {
            diag.fail(path, node.line, "expected tech{...}");
            return;
        }
        const auto key = node.req("key", path, diag);
        if(key.empty() || key.size() > 4) {
            diag.fail(path, node.line, "technology keys must be 1-4 characters");
            return;
        }
        const auto id = w.create_technology();
        auto num = [&](char const* k, double d) { return static_cast<float>(node.num_or(k, d, path, diag)); };
        w.technology_set_key(id, core::pack_key(key));
        w.technology_set_frontier_year(id, num("frontier_year", 2030.0));
        w.technology_set_speed(id, num("speed", 0.3));
        w.technology_set_lag_years(id, num("lag_years", 2.0));
        w.technology_set_oil_cut(id, num("oil_cut", 0.0));
        w.technology_set_emission_cut(id, num("emission_cut", 0.0));
        w.technology_set_connectivity_gain(id, num("connectivity_gain", 0.0));
        w.technology_set_sector_gain(id, num("sector_gain", 0.0));
        w.technology_set_vaccine_speedup(id, num("vaccine_speedup", 0.0));
        if(auto sec = node.find("sector")) {
            bool found = false;
            for(auto s : w.in_sector) {
                if(s.get_key() == core::pack_key(sec->text)) {
                    w.technology_set_sector(id, s.id);
                    found = true;
                }
            }
            if(!found) {
                diag.fail(path, sec->line, "unknown sector '" + sec->text + "'");
                return;
            }
        }
    }
}

void load_organisation_defs(game_state& state, std::string const& assets_dir, core::diagnostics& diag) {
    const std::string path = assets_dir + "/defs/organisations.txt";
    if(!exists(path)) return;
    auto& w = state.world;
    for(auto const& node : core::load_defs(path, diag)) {
        if(node.key != "org") {
            diag.fail(path, node.line, "expected org{...}");
            return;
        }
        const auto key = node.req("key", path, diag);
        if(key.empty() || key.size() > 4) {
            diag.fail(path, node.line, "organisation keys must be 1-4 characters");
            return;
        }
        const auto o = w.create_organisation();
        w.organisation_set_key(o, core::pack_key(key));
        const std::string kind(node.req("kind", path, diag));
        const std::string rule(node.req("rule", path, diag));
        static constexpr char const* kinds[] = {"assembly", "security", "trade", "union", "rights", "health",
                                                "alliance", "club", "cartel", "event", "development", "regional"};
        static_assert(sizeof(kinds) / sizeof(kinds[0]) == static_cast<std::size_t>(org_kind::count));
        static constexpr char const* rules[] = {"majority", "veto", "qualified", "consensus"};
        int ki = -1, ri = -1;
        for(int i = 0; i < static_cast<int>(org_kind::count); ++i) ki = kind == kinds[i] ? i : ki;
        for(int i = 0; i < 4; ++i) ri = rule == rules[i] ? i : ri;
        if(ki < 0 || ri < 0) {
            diag.fail(path, node.line, "unknown kind or rule");
            return;
        }
        w.organisation_set_kind(o, static_cast<uint8_t>(ki));
        w.organisation_set_rule(o, static_cast<uint8_t>(ri));
        w.organisation_set_threshold(o, static_cast<float>(node.num_or("threshold", 0.5, path, diag)));
        w.organisation_set_population_threshold(o, static_cast<float>(node.num_or("population_threshold", 0.0, path, diag)));
        w.organisation_set_votes_needed(o, static_cast<uint8_t>(node.num_or("votes_needed", 0.0, path, diag)));
        w.organisation_set_seats(o, static_cast<uint16_t>(node.num_or("seats", 0.0, path, diag)));
        w.organisation_set_elected_seats(o, static_cast<uint16_t>(node.num_or("elected_seats", 0.0, path, diag)));
        w.organisation_set_term_years(o, static_cast<float>(node.num_or("term_years", 0.0, path, diag)));
        w.organisation_set_last_session(o, -1);
        auto num = [&](char const* k, double d) { return static_cast<float>(node.num_or(k, d, path, diag)); };
        w.organisation_set_guideline(o, num("guideline", 0.0));
        w.organisation_set_summit_month(o, static_cast<uint8_t>(num("summit_month", 0.0)));
        w.organisation_set_cycle_years(o, num("cycle_years", 0.0));
        w.organisation_set_cycle_phase(o, num("cycle_phase", 0.0));
        w.organisation_set_event_month(o, static_cast<uint8_t>(num("event_month", 6.0)));
        w.organisation_set_event_day(o, static_cast<uint8_t>(num("event_day", 1.0)));
        w.organisation_set_duration_days(o, num("duration_days", 0.0));
        w.organisation_set_lead_years(o, num("lead_years", 0.0));
        w.organisation_set_event_cost_bn(o, num("event_cost_bn", 0.0));
        w.organisation_set_visitor_spend_bn(o, num("visitor_spend_bn", 0.0));
        w.organisation_set_price_low(o, num("price_low", 0.0));
        w.organisation_set_price_high(o, num("price_high", 0.0));
        w.organisation_set_capacity_bn(o, num("capacity_bn", 0.0));
        w.organisation_set_quota(o, 1.0f);
        uint16_t flags = 0;
        if(auto f = node.find("flags")) {
            static constexpr std::pair<char const*, uint16_t> names[] = {
                {"democracy_clause", org_democracy_clause}, {"sanctions", org_sanctions}, {"non_interference", org_non_interference},
                {"crisis_coordination", org_crisis_coordination}, {"sanction_alignment", org_sanction_alignment},
                {"counter_hegemonic", org_counter_hegemonic}, {"regime_security", org_regime_security}, {"mediation", org_mediation},
                {"customs_union", org_customs_union}};
            std::size_t pos = 0;
            const std::string text(f->text);
            while(pos < text.size()) {
                const auto end = std::min(text.find(' ', pos), text.size());
                const std::string word = text.substr(pos, end - pos);
                pos = end + 1;
                if(word.empty()) continue;
                bool known = false;
                for(auto const& [name, bit] : names) {
                    if(word == name) {
                        flags = static_cast<uint16_t>(flags | bit);
                        known = true;
                    }
                }
                if(!known) {
                    diag.fail(path, f->line, "unknown organisation flag '" + word + "'");
                    return;
                }
            }
        }
        w.organisation_set_flags(o, flags);
    }
}

void load_conflict_tables(game_state& state, std::string const& dir, core::diagnostics& diag) {
    auto& w = state.world;
    if(exists(dir + "/conflict_curated.csv")) {
        const auto t = core::csv_table::load(dir + "/conflict_curated.csv", diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            if(const auto n = find_nation(state, t.get(row, "iso3"))) w.nation_set_nuclear(n, static_cast<float>(t.number_or(row, "nuclear_warheads", 0.0)));
        }
    }
    if(exists(dir + "/rivalries_curated.csv")) {
        const auto t = core::csv_table::load(dir + "/rivalries_curated.csv", diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            const auto a = find_nation(state, t.get(row, "a"));
            const auto b = find_nation(state, t.get(row, "b"));
            if(!a || !b) continue;
            const auto r = static_cast<float>(std::clamp(t.number_or(row, "rivalry", 0.0), 0.0, 1.0));
            w.nation_set_rivalry(a, static_cast<uint16_t>(b.index()), r);
            w.nation_set_rivalry(b, static_cast<uint16_t>(a.index()), r);
        }
    }
    // Memberships of international organisations (defs/organisations.txt).
    if(exists(dir + "/organisations_curated.csv")) {
        const auto t = core::csv_table::load(dir + "/organisations_curated.csv", diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            const auto key = core::pack_key(t.get(row, "org"));
            dcon::organisation_id org;
            for(auto o : w.in_organisation) {
                if(o.get_key() == key) org = o.id;
            }
            if(!org) {
                diag.fail(dir + "/organisations_curated.csv: unknown organisation '" + std::string(t.get(row, "org")) + "'");
                return;
            }
            const auto role_s = t.get(row, "role");
            const auto role = role_s == "permanent" ? org_role::permanent : role_s == "elected" ? org_role::elected : org_role::member;
            auto add = [&](dcon::nation_id n) {
                const auto m = w.force_create_membership(org, n);
                w.membership_set_role(m, static_cast<uint8_t>(role));
                w.membership_set_term_end(m, -1);
                w.membership_set_exit_tick(m, -1);
                w.membership_set_join_tick(m, -1);
                w.membership_set_suspended_until(m, -1);
                if(auto d = core::parse_date(t.get(row, "join"))) w.membership_set_join_tick(m, core::tick_from_date(*d, state.epoch_days));
                if(auto d = core::parse_date(t.get(row, "term_end"))) w.membership_set_term_end(m, core::tick_from_date(*d, state.epoch_days));
                if(auto d = core::parse_date(t.get(row, "exit"))) w.membership_set_exit_tick(m, core::tick_from_date(*d, state.epoch_days));
            };
            if(t.get(row, "iso3") == "ALL") {
                for(auto n : w.in_nation) add(n.id);
            } else if(const auto n = find_nation(state, t.get(row, "iso3"))) {
                add(n);
            }
        }
    }
    // Awarded mega-events (FIFA World Cups, Olympic Games): one row per host and edition;
    // later editions are awarded in the simulation (megaevents.cpp).
    if(exists(dir + "/megaevents_curated.csv")) {
        const auto t = core::csv_table::load(dir + "/megaevents_curated.csv", diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            const auto key = core::pack_key(t.get(row, "org"));
            dcon::organisation_id org;
            for(auto o : w.in_organisation) {
                if(o.get_key() == key) org = o.id;
            }
            const auto host = find_nation(state, t.get(row, "iso3"));
            const auto start = core::parse_date(t.get(row, "start"));
            const auto finish = core::parse_date(t.get(row, "end"));
            if(!org || !host || !start || !finish) continue; // host outside this scenario
            const auto e = w.create_mega_event();
            w.mega_event_set_organisation(e, org);
            w.mega_event_set_host(e, host);
            w.mega_event_set_share(e, static_cast<float>(t.number_or(row, "share", 1.0)));
            w.mega_event_set_start(e, core::tick_from_date(*start, state.epoch_days));
            w.mega_event_set_finish(e, core::tick_from_date(*finish, state.epoch_days));
            const auto awarded = core::parse_date(t.get(row, "awarded"));
            w.mega_event_set_awarded(e, awarded ? core::tick_from_date(*awarded, state.epoch_days) : w.mega_event_get_start(e) - 2555);
            w.mega_event_set_status(e, 0);
            w.mega_event_set_capex_bn(e, static_cast<float>(t.number_or(row, "capex_bn", w.organisation_get_event_cost_bn(org)) * t.number_or(row, "share", 1.0)));
        }
    }
    // Further per-nation inputs (society, economy, politics extensions).
    for(auto n : w.in_nation) {
        n.set_rd_pct(1.0f);
        n.set_reserves(6.0f);
        n.set_reserves0(6.0f);
        n.set_terrain(0.2f);
        n.set_decentralisation(0.2f);
        n.set_asylum_openness(1.0f);
        n.set_vintage_tfp(1.0f);
        n.set_tax_indirect0(n.get_tax_indirect());
        n.set_tax_firm0(n.get_tax_firm());
        n.set_willingness(0.5f);
        n.set_identity_legitimacy(0.7f);
        n.set_vaccine_priority(0.8f);
    }
    if(exists(dir + "/nations_ext_curated.csv")) {
        const auto t = core::csv_table::load(dir + "/nations_ext_curated.csv", diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            const auto n = find_nation(state, t.get(row, "iso3"));
            if(!n) continue;
            w.nation_set_rd_pct(n, static_cast<float>(t.number_or(row, "rd_pct", 1.0)));
            w.nation_set_reserves(n, static_cast<float>(t.number_or(row, "reserves_months", 6.0)));
            w.nation_set_reserves0(n, w.nation_get_reserves(n));
            w.nation_set_decentralisation(n, static_cast<float>(t.number_or(row, "decentralisation", 0.2)));
            w.nation_set_terrain(n, static_cast<float>(t.number_or(row, "terrain", 0.2)));
            w.nation_set_term_limit_years(n, static_cast<float>(t.number_or(row, "term_limit_years", 0.0)));
            w.nation_set_language(n, core::pack_key(t.get(row, "language")));
            w.nation_set_religion(n, core::pack_key(t.get(row, "religion")));
        }
    }
    for(auto own : w.in_region_ownership) own.get_region().set_ruggedness(own.get_nation().get_terrain());
    // Arms imports: the share of procurement bought abroad falls with industrial
    // capability (SIPRI arms transfers; large producers import little).
    for(auto n : w.in_nation) n.set_arms_import_share(0.3f);
}

void load_trade_tables(game_state& state, std::string const& dir, core::diagnostics& diag) {
    auto& w = state.world;
    // Great-circle distances between capitals (the gravity fallback; logistics refines them).
    for(auto a : w.in_nation) {
        for(auto b : w.in_nation) {
            const auto kb = static_cast<uint8_t>(b.id.index());
            a.set_trade_distance(kb, a.id == b.id ? 0.0f : great_circle_km(a.get_lat(), a.get_lon(), b.get_lat(), b.get_lon()));
            a.set_land_share(kb, 0.0f);
            a.set_pair_cost(kb, 1.0f);
        }
    }
    if(exists(dir + "/overland_curated.csv")) {
        const auto t = core::csv_table::load(dir + "/overland_curated.csv", diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            const auto a = find_nation(state, t.get(row, "a"));
            const auto b = find_nation(state, t.get(row, "b"));
            if(!a || !b) continue;
            const auto share = static_cast<float>(std::clamp(t.number_or(row, "land_share", 0.0), 0.0, 1.0));
            w.nation_set_land_share(a, static_cast<uint8_t>(b.index()), share);
            w.nation_set_land_share(b, static_cast<uint8_t>(a.index()), share);
        }
    }
    if(exists(dir + "/logistics_curated.csv")) {
        const auto t = core::csv_table::load(dir + "/logistics_curated.csv", diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            if(auto n = find_nation(state, t.get(row, "iso3"))) {
                w.nation_set_port_teu(n, static_cast<float>(t.number_or(row, "port_teu_m", 0.0)));
                w.nation_set_seaborne_share(n, static_cast<float>(std::clamp(t.number_or(row, "seaborne_share", 0.75), 0.0, 1.0)));
            }
        }
    }
    if(exists(dir + "/ports_curated.csv") && w.sea_node_size() > 0) {
        const auto t = core::csv_table::load(dir + "/ports_curated.csv", diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            const auto n = find_nation(state, t.get(row, "iso3"));
            if(!n) continue;
            const auto node = find_node(state, t.get(row, "node"));
            if(!node) {
                diag.fail(t.source() + ": unknown sea node '" + std::string(t.get(row, "node")) + "'");
                return;
            }
            const auto rel = w.force_create_port_access(n, node);
            w.port_access_set_share(rel, static_cast<float>(t.number_or(row, "share", 1.0)));
        }
    }
    // Nations without ports use the nearest sea node.
    if(w.sea_node_size() > 0) {
        for(auto n : w.in_nation) {
            if(n.get_port_access().begin() != n.get_port_access().end()) continue;
            dcon::sea_node_id best;
            float d = 1e30f;
            for(auto s : w.in_sea_node) {
                const float km = great_circle_km(n.get_lat(), n.get_lon(), s.get_lat(), s.get_lon());
                if(km < d) {
                    d = km;
                    best = s.id;
                }
            }
            const auto rel = w.force_create_port_access(n.id, best);
            w.port_access_set_share(rel, 1.0f);
        }
    }

    // Commodity production / consumption shares (ROW takes the remainder).
    const auto C = static_cast<uint32_t>(w.commodity_size());
    w.nation_resize_commodity_prod(C);
    w.nation_resize_commodity_cons(C);
    w.nation_resize_commodity_take(C);
    w.nation_resize_commodity_demand(C);
    // Generated shares (world dataset: energy data) first, curated values override them.
    for(char const* file : {"/commodities.csv", "/commodities_curated.csv"}) {
    if(exists(dir + file)) {
        const auto t = core::csv_table::load(dir + file, diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            const auto n = find_nation(state, t.get(row, "iso3"));
            if(!n) continue;
            const auto key = core::pack_key(t.get(row, "commodity"));
            for(auto c : w.in_commodity) {
                if(c.get_key() != key) continue;
                const auto ci = static_cast<uint8_t>(c.id.index());
                w.nation_set_commodity_prod(n, ci, static_cast<float>(t.number_or(row, "production_share", 0.0)));
                w.nation_set_commodity_cons(n, ci, static_cast<float>(t.number_or(row, "consumption_share", 0.0)));
                if(auto v = t.number(row, "gov_take")) w.nation_set_commodity_take(n, ci, static_cast<float>(*v));
            }
        }
    }
    }
    const auto row_nation = find_nation(state, "ROW");
    for(auto c : w.in_commodity) {
        const auto ci = static_cast<uint8_t>(c.id.index());
        double prod = 0.0, cons = 0.0, gdp = 0.0;
        for(auto n : w.in_nation) {
            prod += n.get_commodity_prod(ci);
            cons += n.get_commodity_cons(ci);
            gdp += n.get_gdp0();
        }
        if(row_nation && w.nation_get_commodity_prod(row_nation, ci) == 0.0f && w.nation_get_commodity_cons(row_nation, ci) == 0.0f) {
            w.nation_set_commodity_prod(row_nation, ci, static_cast<float>(std::max(0.0, 1.0 - prod)));
            w.nation_set_commodity_cons(row_nation, ci, static_cast<float>(std::max(0.0, 1.0 - cons)));
        } else if(cons <= 0.0 && gdp > 0.0) {
            // No consumption data at all: consumption follows GDP.
            for(auto n : w.in_nation) n.set_commodity_cons(ci, static_cast<float>(n.get_gdp0() / gdp));
        }
    }
}

} // namespace geosim::sim
