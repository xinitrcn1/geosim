// The action tabs' data (docs/08-ui.md#action-tabs): read-only sim_runner jobs that copy
// the acting agent's levers, a nation's budget, trade, power structure and diplomatic
// options out of the simulation at its sync point. Writes (overrides, actions) are
// commands (sim_runner.cpp push_command, sim/commands.hpp).

#include "sim_runner.hpp"

#include "parliament.hpp"

#include "core/keys.hpp"
#include "sim/game_state.hpp"
#include "sim/levers.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>


namespace geosim::client {

namespace {

using sim::agent_kind;
using sim::lever_tab;

dcon::nation_id nid(uint32_t i) { return dcon::nation_id{static_cast<dcon::nation_id::value_base_t>(i)}; }
dcon::agent_id aid(uint32_t i) { return dcon::agent_id{static_cast<dcon::agent_id::value_base_t>(i)}; }

std::string fmt(char const* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

// OxCGRT indicator names (assets/defs/npi.txt `name{}`; the sim keeps only the key).
std::string npi_name(std::string const& key) {
    static constexpr std::pair<char const*, char const*> names[] = {
        {"C1", "School closing"}, {"C2", "Workplace closing"}, {"C3", "Cancel public events"}, {"C4", "Restrictions on gatherings"},
        {"C5", "Close public transport"}, {"C6", "Stay at home requirements"}, {"C7", "Restrictions on internal movement"},
        {"C8", "International travel controls"}, {"E1", "Income support"}, {"H2", "Testing policy"}, {"H3", "Contact tracing"},
        {"H6", "Facial coverings"},
    };
    for(auto const& [k, n] : names) {
        if(key == k) return std::string(n) + " (" + key + ")";
    }
    return "NPI " + key;
}

lever_row make_row(game_state const& s, dcon::agent_id a, uint8_t index) {
    auto const& w = s.world;
    lever_row r;
    r.index = index;
    const auto& D = s.derived;
    if(index < D.npi_count) {
        const dcon::npi_indicator_id ind{index};
        r.npi = true;
        r.name = npi_name(core::unpack_key(w.npi_indicator_get_key(ind)));
        r.lo = 0.0f;
        r.hi = w.npi_indicator_get_max_level(ind);
        r.step = 1.0f;
        r.unit = "level 0-" + fmt("%.0f", r.hi);
    } else {
        auto const& m = sim::lever_table[static_cast<std::size_t>(index - D.lever_base)];
        r.name = m.name;
        r.unit = m.unit;
        r.lo = m.lo;
        r.hi = m.hi;
        r.step = m.step;
        r.bill = m.bill;
    }
    if(a) {
        r.current = w.agent_get_last_choice(a, index);
        r.ai_choice = w.agent_get_last_ai_choice(a, index);
        r.value = w.agent_get_override_value(a, index);
        r.mode = w.agent_get_override_mode(a, index);
    }
    return r;
}

// The levers `kind` owns on tab `t`; NPIs belong with the executive's policies.
void collect_levers(game_state const& s, dcon::agent_id a, agent_kind kind, std::initializer_list<lever_tab> tabs, bool npis,
                    std::vector<lever_row>& out) {
    const auto& D = s.derived;
    if(npis) {
        for(uint8_t k = 0; k < D.npi_count; ++k) out.push_back(make_row(s, a, k));
    }
    for(auto const& m : sim::lever_table) {
        if(m.owner != kind || std::find(tabs.begin(), tabs.end(), m.tab) == tabs.end()) continue;
        const auto index = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(m.id));
        if(index < D.lever_count) out.push_back(make_row(s, a, index));
    }
}

void add_lever(game_state const& s, dcon::agent_id a, sim::lever l, std::vector<lever_row>& out) {
    const auto index = static_cast<uint8_t>(s.derived.lever_base + static_cast<uint8_t>(l));
    if(index < s.derived.lever_count) out.push_back(make_row(s, a, index));
}

fiscal_summary read_fiscal(game_state const& s, dcon::nation_id n) {
    auto const& w = s.world;
    fiscal_summary f;
    const double nominal = w.nation_get_acc_gdp_nominal(n);
    const double days = w.nation_get_acc_days(n);
    f.days = static_cast<float>(days);
    using sim::flow;
    double revenue = 0.0;
    for(flow k : {flow::tax_households, flow::tax_indirect, flow::tax_firms, flow::tax_resource, flow::tariffs, flow::carbon_tax, flow::wealth_tax}) {
        if(static_cast<std::size_t>(k) < w.nation_get_flows_size()) revenue += w.nation_get_flows(n, static_cast<uint8_t>(k));
    }
    const double gdp0 = w.nation_get_gdp0(n);
    const double annual = days >= 1.0 && nominal > 0.0 ? nominal / days * 365.0 : gdp0;
    if(nominal > 0.0 && days >= 1.0) {
        f.valid = true;
        f.revenue = static_cast<float>(revenue / nominal * 100.0);
        f.balance = static_cast<float>((w.nation_get_bal_gov(n) - w.nation_get_acc_bal_gov_start(n)) / nominal * 100.0);
        f.spending = f.revenue - f.balance;
    }
    f.debt = annual > 0.0 ? static_cast<float>(std::max(0.0, -w.nation_get_bal_gov(n)) / annual * 100.0) : 0.0f;
    f.debt0 = gdp0 > 0.0 ? static_cast<float>(w.nation_get_gov_debt0(n) / gdp0 * 100.0) : 0.0f;
    f.yield10 = w.nation_get_yield10(n);
    f.spread = w.nation_get_spread(n);
    f.tax_scale = w.nation_get_tax_scale(n);
    return f;
}

// Top `top` entries by value, the rest summed into "Other".
void top_slices(std::vector<trade_slice>& v, std::size_t top) {
    std::sort(v.begin(), v.end(), [](auto const& a, auto const& b) { return a.value > b.value; });
    v.erase(std::remove_if(v.begin(), v.end(), [](auto const& x) { return !(x.value > 0.0f); }), v.end());
    if(v.size() <= top + 1) return;
    trade_slice other;
    other.label = "Other";
    for(std::size_t i = top; i < v.size(); ++i) other.value += v[i].value;
    v.resize(top);
    v.push_back(other);
}

// Sector names (assets/defs/sectors.txt `name{}`; the sim keeps only the key).
std::string sector_name(std::string const& key) {
    static constexpr std::pair<char const*, char const*> names[] = {
        {"agr", "Agriculture"}, {"ene", "Mining, energy, utilities"}, {"man", "Manufacturing"}, {"con", "Construction"},
        {"csv", "Contact-intensive services"}, {"osv", "Business and other services"}, {"gov", "Public services"},
    };
    for(auto const& [k, n] : names) {
        if(key == k) return n;
    }
    return key;
}

char const* goal_name(uint8_t g) {
    static constexpr char const* names[] = {"none", "contain the epidemic", "restore growth", "regime security", "deter a threat", "consolidate", "secure energy"};
    return g < std::size(names) ? names[g] : "?";
}

} // namespace

std::future<tab_snapshot> sim_runner::request_tab(tab_request rq) {
    auto promise = std::make_shared<std::promise<tab_snapshot>>();
    auto future = promise->get_future();
    post([this, promise, rq](game_state& s) {
        tab_snapshot out;
        auto const& w = s.world;
        out.tick = s.current_tick;
        const dcon::agent_id a = rq.agent < w.agent_size() ? aid(rq.agent) : dcon::agent_id{};
        out.agent = rq.agent;
        if(a) out.kind = w.agent_get_kind(a);
        const auto kind = static_cast<agent_kind>(out.kind);
        uint32_t home = rq.home;
        if(home >= w.nation_size() && a && w.agent_get_home(a)) home = static_cast<uint32_t>(w.agent_get_home(a).id.index());
        out.nation = home < w.nation_size() ? home : 0xFFFFFFFFu;
        const dcon::nation_id n = out.nation < w.nation_size() ? nid(out.nation) : dcon::nation_id{};
        out.nation_name = nation_name(out.nation);
        const uint32_t count = static_cast<uint32_t>(w.nation_size());

        switch(rq.tab) {
        case tab_kind::policies:
            if(!a) break;
            switch(kind) {
            case agent_kind::executive: collect_levers(s, a, kind, {lever_tab::policies}, true, out.levers); break;
            case agent_kind::central_bank: collect_levers(s, a, kind, {lever_tab::monetary}, false, out.levers); break;
            case agent_kind::board: collect_levers(s, a, kind, {lever_tab::firm}, false, out.levers); break;
            case agent_kind::union_federation: collect_levers(s, a, kind, {lever_tab::labour}, false, out.levers); break;
            case agent_kind::governor: collect_levers(s, a, kind, {lever_tab::regional}, false, out.levers); break;
            default: break;
            }
            break;
        case tab_kind::fiscal:
            if(a && kind == agent_kind::executive) collect_levers(s, a, kind, {lever_tab::fiscal}, false, out.levers);
            if(n) out.fiscal = read_fiscal(s, n);
            break;
        case tab_kind::trade: {
            if(!n) break;
            if(a && kind == agent_kind::executive) {
                add_lever(s, a, sim::lever::tariff, out.levers);
                add_lever(s, a, sim::lever::capital_controls, out.levers);
            }
            trade_snapshot& t = out.trade;
            t.valid = true;
            for(uint32_t k = 0; k < count; ++k) {
                if(k == out.nation) continue;
                const dcon::nation_id m = nid(k);
                const float x = w.nation_get_bilateral_exports(n, static_cast<uint16_t>(k));
                const float im = w.nation_get_bilateral_exports(m, static_cast<uint16_t>(out.nation));
                if(x > 0.0f) t.exports.push_back({nation_name(k), x, k});
                if(im > 0.0f) t.imports.push_back({nation_name(k), im, k});
                if(w.nation_get_sanction(n, static_cast<uint16_t>(k)) > 0.05f) {
                    t.sanctions_by.push_back(nation_name(k) + fmt(" %.2f", w.nation_get_sanction(n, static_cast<uint16_t>(k))));
                }
                if(w.nation_get_sanction(m, static_cast<uint16_t>(out.nation)) > 0.05f) {
                    t.sanctions_on.push_back(nation_name(k) + fmt(" %.2f", w.nation_get_sanction(m, static_cast<uint16_t>(out.nation))));
                }
            }
            for(uint8_t sec = 0; sec < s.derived.sector_count && sec < w.nation_get_comp_x_size(); ++sec) {
                const dcon::sector_id sid{sec};
                t.sectors.push_back({sector_name(core::unpack_key(w.sector_get_key(sid))), w.nation_get_comp_x(n, sec)});
            }
            top_slices(t.exports, 8);
            top_slices(t.imports, 8);
            top_slices(t.sectors, 8);
            const float gdp = w.nation_get_gdp_real(n);
            t.exports_pct = gdp > 0.0f ? w.nation_get_exports(n) / gdp * 100.0f : 0.0f;
            t.imports_pct = gdp > 0.0f ? w.nation_get_imports(n) / gdp * 100.0f : 0.0f;
            const float y0 = w.nation_get_c0(n) + w.nation_get_g0(n) + w.nation_get_i0(n) + w.nation_get_x0(n) - w.nation_get_m0(n);
            t.exports0_pct = y0 > 0.0f ? w.nation_get_x0(n) / y0 * 100.0f : 0.0f;
            t.imports0_pct = y0 > 0.0f ? w.nation_get_m0(n) / y0 * 100.0f : 0.0f;
            t.tariff = w.nation_get_tariff(n) * 100.0f;
            t.tariff0 = w.nation_get_tariff0(n) * 100.0f;
            break;
        }
        case tab_kind::military: {
            if(!n) break;
            if(a && kind == agent_kind::executive) add_lever(s, a, sim::lever::defense, out.levers);
            military_snapshot& m = out.military;
            m.valid = true;
            m.loyalty = w.nation_get_military_loyalty(n);
            m.defence = w.nation_get_defense_share(n); // already % of GDP
            m.defence0 = w.nation_get_defense_share0(n);
            m.power = w.nation_get_power(n);
            m.cinc = w.nation_get_cinc(n);
            m.nuclear = w.nation_get_nuclear(n);
            m.war_effort = w.nation_get_war_effort(n);
            m.threat = w.nation_get_threat(n);
            for(uint32_t k = 0; k < count; ++k) {
                if(k == out.nation) continue;
                const auto kk = static_cast<uint16_t>(k);
                if(w.nation_get_at_war(n, kk) > 0.0f) m.wars.push_back(nation_name(k));
                else if(w.nation_get_hostility(n, kk) >= 2.0f) m.disputes.push_back(nation_name(k) + fmt(" (hostility %.0f of 5)", w.nation_get_hostility(n, kk)));
                if(w.nation_get_alliance(n, kk) > 0.0f) m.allies.push_back(nation_name(k));
            }
            break;
        }
        case tab_kind::politics: {
            const uint32_t pn = rq.nation < count ? rq.nation : out.nation;
            if(pn >= count) break;
            const dcon::nation_id p = nid(pn);
            power_snapshot& ps = out.power;
            ps.valid = true;
            ps.nation = pn;
            ps.iso3 = core::unpack_key(w.nation_get_iso3(p));
            ps.name = nation_name(pn);
            ps.electoral_system = w.nation_get_electoral_system(p);
            ps.electoral = w.nation_get_electoral(p);
            ps.liberal = w.nation_get_liberal(p);
            ps.legitimacy = w.nation_get_legitimacy(p);
            ps.approval = w.nation_get_approval(p);
            ps.capture = w.nation_get_capture_index(p);
            ps.winning_coalition = w.nation_get_winning_coalition(p);
            ps.gov_seat_share = w.nation_get_gov_seat_share(p);
            ps.chamber = chamber_size(ps.iso3, w.nation_get_population(p));
            ps.democracy = w.nation_get_competitive(p) != 0 && ps.electoral_system != static_cast<uint8_t>(sim::electoral_system::none) && ps.electoral >= 0.5f;
            const auto incumbent = w.nation_get_incumbent(p);
            for(auto m : w.nation_get_party_membership(p)) {
                auto party = m.get_party();
                party_row r;
                r.key = core::unpack_key(party.get_key());
                r.family = party.get_family();
                r.vote = party.get_vote_share();
                r.seat = party.get_seat_share();
                r.in_government = party.get_in_government() != 0;
                r.incumbent = party.id == incumbent;
                for(uint8_t ax = 0; ax < 3 && ax < w.party_get_position_size(); ++ax) r.position[ax] = party.get_position(ax);
                ps.parties.push_back(std::move(r));
            }
            std::stable_sort(ps.parties.begin(), ps.parties.end(), [](auto const& x, auto const& y) { return x.seat > y.seat; });
            // The figures: the nation's agents and what drives them.
            auto ideals = [&](figure_row& f, dcon::agent_id g) {
                f.items.push_back({"Order", fmt("%+.2f", w.agent_get_order_ideal(g))});
                f.items.push_back({"Fiscal hawk", fmt("%+.2f", w.agent_get_fiscal_hawk(g))});
                f.items.push_back({"Rigidity ρ", fmt("%.2f", w.agent_get_rho(g))});
            };
            if(auto e = w.nation_get_agent_from_executive_seat(p)) {
                figure_row f{"🏛", "Executive", {}, "The head of government: ideals (order vs liberty, fiscal hawkishness), ideological rigidity, legitimacy"};
                ideals(f, e);
                f.items.push_back({"Legitimacy", fmt("%.2f", ps.legitimacy)});
                f.items.push_back({"Approval", fmt("%.2f", ps.approval)});
                f.items.push_back({"Goal", goal_name(w.agent_get_goal(e))});
                ps.figures.push_back(std::move(f));
            }
            if(auto mil = w.nation_get_military_agent(p)) {
                figure_row f{"🎖", "Military", {}, "The armed forces: loyalty to the government (a coup risk below ~0.4), ideals"};
                f.items.push_back({"Loyalty", fmt("%.2f", w.nation_get_military_loyalty(p))});
                ideals(f, mil);
                f.items.push_back({"Defence", fmt("%.1f%% GDP", w.nation_get_defense_share(p))});
                ps.figures.push_back(std::move(f));
            }
            if(auto bank = w.nation_get_bank_from_monetary_authority(p)) {
                figure_row f{"🏦", "Central bank", {}, "Monetary authority: independence from the government and its hawkishness"};
                f.items.push_back({"Independence", fmt("%.2f", w.nation_get_cb_independence(p))});
                f.items.push_back({"Fiscal hawk", fmt("%+.2f", w.agent_get_fiscal_hawk(bank))});
                f.items.push_back({"Rigidity ρ", fmt("%.2f", w.agent_get_rho(bank))});
                f.items.push_back({"Rate", fmt("%.2f%%", w.nation_get_policy_rate(p))});
                ps.figures.push_back(std::move(f));
            }
            if(auto u = w.nation_get_union_agent(p)) {
                figure_row f{"✊", "Union federation", {}, "Organised labour: membership and wage push"};
                f.items.push_back({"Density", fmt("%.0f%%", w.nation_get_union_density(p) * 100.0)});
                f.items.push_back({"Wage push", fmt("%+.3f", w.nation_get_union_push(p))});
                f.items.push_back({"Rigidity ρ", fmt("%.2f", w.agent_get_rho(u))});
                ps.figures.push_back(std::move(f));
            }
            {
                std::vector<std::pair<float, dcon::firm_id>> firms;
                double total = 0.0;
                for(auto f : w.in_firm) {
                    if(f.get_nation() != p) continue;
                    total += f.get_share();
                    if(f.get_board()) firms.emplace_back(f.get_share(), f.id);
                }
                std::sort(firms.begin(), firms.end(), [](auto const& x, auto const& y) { return x.first > y.first; });
                for(std::size_t i = 0; i < firms.size() && i < 3; ++i) {
                    const auto f = firms[i].second;
                    const auto board = w.firm_get_board(f);
                    const std::string sector = w.firm_get_sector(f) ? sector_name(core::unpack_key(w.firm_get_sector(f).get_key())) : std::string("Firm");
                    figure_row r{"🏭", sector + " board", {}, "One of the largest boards: its share of output and its stance"};
                    r.items.push_back({"Output share", fmt("%.0f%%", total > 0.0 ? firms[i].first / total * 100.0 : 0.0)});
                    r.items.push_back({"Hoarding", fmt("%.2f", w.firm_get_hoarding(f))});
                    r.items.push_back({"Investment", fmt("%+.2f", w.firm_get_investment_drive(f))});
                    r.items.push_back({"Rigidity ρ", fmt("%.2f", w.agent_get_rho(board.id))});
                    ps.figures.push_back(std::move(r));
                }
            }
            if(auto rebel = w.nation_get_rebel_agent(p); rebel && w.agent_get_strength(rebel) > 0.0f) {
                figure_row f{"🏴", "Rebels", {}, "An armed rebellion: strength and demand"};
                f.items.push_back({"Strength", fmt("%.2f", w.agent_get_strength(rebel))});
                f.items.push_back({"Demand", fmt("%.2f", w.agent_get_demand(rebel))});
                ps.figures.push_back(std::move(f));
            }
            // The acting board and its firm.
            if(a && kind == agent_kind::board) {
                for(auto f : w.in_firm) {
                    if(f.get_board().id != a) continue;
                    figure_row r{"🏭", "Your board", {}, "The board you control and its firm"};
                    r.items.push_back({"Hoarding", fmt("%.2f", f.get_hoarding())});
                    r.items.push_back({"Investment", fmt("%+.2f", f.get_investment_drive())});
                    r.items.push_back({"Leverage", fmt("%.2f", f.get_leverage())});
                    r.items.push_back({"Cash", fmt("%.0f days", f.get_cash_days())});
                    r.items.push_back({"Exit rate", fmt("%.2f%%", f.get_exit_rate() * 100.0)});
                    r.items.push_back({"State owned", fmt("%.0f%%", f.get_soe_share() * 100.0)});
                    ps.board.push_back(std::move(r));
                    figure_row id{"⚙", "Board", {}, "The board's own ideals"};
                    ideals(id, a);
                    ps.board.push_back(std::move(id));
                    break;
                }
            }
            // Elite strata: pops ranked by income per person; the top 10 % and 25 % of the
            // population against everyone, on the value axes.
            {
                struct pp {
                    float pc, size;
                    dcon::pop_id id;
                };
                std::vector<pp> pops;
                double total = 0.0;
                for(auto own : w.nation_get_region_ownership(p)) {
                    for(auto loc : own.get_region().get_pop_location()) {
                        auto q = loc.get_pop();
                        const float size = q.get_size();
                        if(!(size > 0.0f)) continue;
                        pops.push_back({std::max(0.0f, q.get_income()) / size, size, q.id});
                        total += size;
                    }
                }
                std::sort(pops.begin(), pops.end(), [](auto const& x, auto const& y) { return x.pc > y.pc; });
                auto slice = [&](char const* label, double frac) {
                    leaning_row r;
                    r.label = label;
                    double cum = 0.0, v[3] = {};
                    for(auto const& q : pops) {
                        if(cum >= frac * total) break;
                        const double take = std::min<double>(q.size, frac * total - cum);
                        for(uint8_t ax = 0; ax < 3 && ax < w.pop_get_values_size(); ++ax) v[ax] += take * w.pop_get_values(q.id, ax);
                        cum += take;
                    }
                    r.share = total > 0.0 ? static_cast<float>(cum / total) : 0.0f;
                    for(int ax = 0; ax < 3; ++ax) r.values[ax] = cum > 0.0 ? static_cast<float>(v[ax] / cum) : 0.0f;
                    ps.leanings.push_back(r);
                };
                if(total > 0.0) {
                    slice("Top 10% by income", 0.10);
                    slice("Top 25%", 0.25);
                    slice("Everyone", 1.0);
                }
            }
            break;
        }
        case tab_kind::diplomacy: {
            diplomacy_snapshot& d = out.diplomacy;
            d.valid = true;
            const int sub = rq.subtab;
            if(sub == 0) { // governments: nations (own first for non-executives)
                if(n && kind != agent_kind::executive) d.counterparts.push_back({out.nation, 0xFFFFFFFFu, 0xFFFFFFFFu, out.nation_name, "your government"});
                for(uint32_t k = 0; k < count; ++k) {
                    if(k == out.nation) continue;
                    counterpart_row c{k, 0xFFFFFFFFu, 0xFFFFFFFFu, nation_name(k), {}};
                    if(n && kind == agent_kind::executive) {
                        const auto kk = static_cast<uint16_t>(k);
                        if(w.nation_get_at_war(n, kk) > 0.0f) c.detail = "at war";
                        else if(w.nation_get_alliance(n, kk) > 0.0f) c.detail = "ally";
                        else if(w.nation_get_hostility(n, kk) >= 2.0f) c.detail = fmt("dispute %.0f/5", w.nation_get_hostility(n, kk));
                        if(w.nation_get_sanction(n, kk) > 0.05f) c.detail += (c.detail.empty() ? "" : ", ") + fmt("sanctions %.2f", w.nation_get_sanction(n, kk));
                    }
                    d.counterparts.push_back(std::move(c));
                }
                std::stable_sort(d.counterparts.begin() + (n && kind != agent_kind::executive ? 1 : 0), d.counterparts.end(),
                                 [](auto const& x, auto const& y) { return x.label < y.label; });
            } else if(sub >= 1 && sub <= 6) { // other agents of that kind
                for(auto const& ag : collect_agents(s, nation_names_, region_names_)) {
                    if(ag.kind != sub || ag.agent == rq.agent) continue;
                    counterpart_row c{ag.nation, ag.agent, 0xFFFFFFFFu, ag.label, ag.iso3};
                    if(sub == static_cast<int>(agent_kind::rebel)) {
                        const float st = w.agent_get_strength(aid(ag.agent));
                        if(!(st > 0.0f)) continue; // dormant
                        c.detail = fmt("strength %.2f", st);
                    }
                    d.counterparts.push_back(std::move(c));
                }
            } else { // organisations, and the unilateral climate pledge first
                if(kind == agent_kind::executive && n) {
                    d.counterparts.push_back({0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, "Climate pledge (Paris)", fmt("%.0f USD/t by 2030", w.nation_get_climate_pledge(n))});
                }
                static constexpr char const* kind_names[] = {"assembly", "security council", "trade", "union", "rights", "health", "alliance",
                                                             "club", "cartel", "events", "development", "regional"};
                for(auto o : w.in_organisation) {
                    counterpart_row c{0xFFFFFFFFu, 0xFFFFFFFFu, static_cast<uint32_t>(o.id.index()), core::unpack_key(o.get_key()), {}};
                    const uint8_t ok = o.get_kind();
                    c.detail = ok < std::size(kind_names) ? kind_names[ok] : "organisation";
                    if(n) {
                        const auto m = sim::membership_in(s, o.id, n);
                        if(m) c.detail += sim::membership_active(s, m) ? ", member" : ", suspended";
                    }
                    d.counterparts.push_back(std::move(c));
                }
            }
            // The options toward the selected counterpart: the sim lists exactly the ones
            // toward a nation, an organisation or another agent.
            const counterpart_row& t = rq.target;
            dcon::agent_id pressed; // whose levers a lobby / press acts on
            if(a) {
                using sim::action_kind;
                std::vector<sim::action_option> opts;
                if(sub == 0 && t.nation < count) {
                    opts = sim::available_actions(s, a, nid(t.nation), {});
                    if(n && t.nation == out.nation) pressed = w.nation_get_agent_from_executive_seat(n);
                } else if(sub >= 1 && sub <= 6 && t.agent < w.agent_size()) {
                    pressed = aid(t.agent);
                    opts = sim::available_actions(s, a, {}, {}, pressed);
                } else if(sub == 7) {
                    if(t.organisation < w.organisation_size()) {
                        opts = sim::available_actions(s, a, {}, dcon::organisation_id{static_cast<dcon::organisation_id::value_base_t>(t.organisation)});
                        std::erase_if(opts, [](auto const& o) { return o.kind == action_kind::climate_pledge; });
                    } else if(t.label.rfind("Climate", 0) == 0) {
                        opts = sim::available_actions(s, a, {}, {});
                    }
                }
                for(auto const& o : opts) {
                    action_row row;
                    row.kind = static_cast<uint8_t>(o.kind);
                    row.label = o.label ? o.label : "";
                    row.available = o.available;
                    row.reason = o.reason;
                    switch(o.kind) {
                    case action_kind::set_sanction: row.input = action_input::sanction_level; break;
                    case action_kind::send_aid: row.input = action_input::aid_share; break;
                    case action_kind::climate_pledge: row.input = action_input::pledge; break;
                    case action_kind::propose_treaty: row.input = action_input::treaty; break;
                    default: break;
                    }
                    if(o.input == sim::action_value::lever) row.input = action_input::lobby;
                    if(o.input == sim::action_value::amount) {
                        row.input = action_input::amount;
                        row.unit = o.unit ? o.unit : "";
                        row.lo = o.lo, row.hi = o.hi, row.step = o.step, row.initial = o.initial;
                    }
                    d.actions.push_back(std::move(row));
                }
            }
            // A lobby or a press acts on one of the pressed agent's levers.
            if(pressed) {
                const auto pk = static_cast<agent_kind>(w.agent_get_kind(pressed));
                switch(pk) {
                case agent_kind::executive: collect_levers(s, pressed, pk, {lever_tab::policies, lever_tab::fiscal}, false, d.lobby_levers); break;
                case agent_kind::central_bank: collect_levers(s, pressed, pk, {lever_tab::monetary}, false, d.lobby_levers); break;
                case agent_kind::board: collect_levers(s, pressed, pk, {lever_tab::firm}, false, d.lobby_levers); break;
                case agent_kind::union_federation: collect_levers(s, pressed, pk, {lever_tab::labour}, false, d.lobby_levers); break;
                case agent_kind::governor: collect_levers(s, pressed, pk, {lever_tab::regional}, false, d.lobby_levers); break;
                default: break;
                }
            }
            break;
        }
        }
        promise->set_value(std::move(out));
    });
    return future;
}

} // namespace geosim::client
