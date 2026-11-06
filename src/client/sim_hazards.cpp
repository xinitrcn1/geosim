// sim_runner::request_hazards: live cyclones, recent earthquakes and disaster
// records, copied at the simulation's sync point (storms and disasters are
// erasable objects, so they are never read racily from the UI thread).
#include "sim_runner.hpp"

#include "core/keys.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include "core/calendar.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::client {

namespace {

uint64_t mix_key(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

uint64_t hash_string(uint64_t h, std::string const& s) {
    for(char c : s) h = mix_key(h, static_cast<uint8_t>(c));
    return h;
}

constexpr int32_t quake_ring_days = 7;

} // namespace

std::future<hazard_snapshot> sim_runner::request_hazards() {
    auto promise = std::make_shared<std::promise<hazard_snapshot>>();
    auto future = promise->get_future();
    post([this, promise](game_state& s) {
        hazard_snapshot out;
        out.tick = s.current_tick;
        auto const& w = s.world;
        for(auto st : w.in_storm) {
            if(out.storms.size() >= map_max_storms) break;
            out.storms.push_back({st.get_lon(), st.get_lat(), st.get_wind(), st.get_radius_km(), st.get_heading(), st.get_speed_kmd(),
                                  st.get_serial()});
        }
        for(auto d : w.in_disaster) {
            const uint8_t kind = d.get_kind();
            const int32_t start = d.get_start();
            if(kind == static_cast<uint8_t>(sim::disaster_kind::earthquake) && s.current_tick - start <= quake_ring_days
               && out.quakes.size() < map_max_quakes) {
                map_quake q;
                q.key = mix_key(mix_key(0x51u, static_cast<uint64_t>(start)), static_cast<uint64_t>(std::lround(d.get_lat() * 100.0f + 1e5f)));
                q.key = mix_key(q.key, static_cast<uint64_t>(std::lround(d.get_lon() * 100.0f + 1e5f)));
                q.lon = d.get_lon();
                q.lat = d.get_lat();
                q.magnitude = d.get_magnitude();
                q.age_days = static_cast<float>(s.current_tick - start);
                out.quakes.push_back(q);
            }
            const sim::disaster_text text = sim::describe_disaster(s, d.id);
            disaster_note n;
            n.kind = kind;
            n.status = d.get_status();
            n.start = start;
            const auto nation = text.nation;
            if(nation) n.nation = core::unpack_key(w.nation_get_iso3(nation));
            n.title = text.title;
            n.place = place_name(nation ? static_cast<uint32_t>(nation.index()) : 0xFFFFFFFFu,
                                 text.region ? static_cast<uint32_t>(text.region.index()) : 0xFFFFFFFFu);
            n.date = text.date;
            n.deaths = text.deaths;
            n.affected = text.affected;
            n.damage_usd = text.damage_usd;
            n.gdp_loss_pct = text.gdp_loss_pct;
            n.potential = text.potential;
            n.lat = d.get_lat();
            n.lon = d.get_lon();
            n.magnitude = d.get_magnitude();
            n.region = text.region ? static_cast<uint32_t>(text.region.index()) : 0xFFFFFFFFu;
            n.id = static_cast<uint32_t>(d.id.index());
            n.key = hash_string(mix_key(mix_key(0xD15A57E2ull, kind), static_cast<uint64_t>(start)), n.title); // hazard_feed makes it per record
            out.disasters.push_back(std::move(n));
        }
        promise->set_value(std::move(out));
    });
    return future;
}

std::future<region_snapshot> sim_runner::request_regions(bool with_faults) {
    auto promise = std::make_shared<std::promise<region_snapshot>>();
    auto future = promise->get_future();
    post([this, promise, with_faults](game_state& s) {
        region_snapshot out;
        out.tick = s.current_tick;
        // State names (sim thread): states created by redrawing get a name of their own.
        if(state_names_.size() < s.world.state_size()) state_names_.resize(s.world.state_size());
        for(std::size_t i = 0; i < state_names_.size(); ++i) {
            if(state_names_[i].empty()) state_names_[i] = "New state " + std::to_string(i);
        }
        out.state_names = state_names_;
        auto const& w = s.world;
        const core::civil_date today = core::date_from_tick(s.current_tick, s.epoch_days);
        out.month = static_cast<int>(today.month);
        const float cap = static_cast<float>(s.params.soil_capacity_mm);
        out.regions.resize(w.region_size());
        for(auto r : w.in_region) {
            region_row& row = out.regions[r.id.index()];
            row.crop_loss = r.get_crop_loss();
            row.drought_record = bool(r.get_drought_disaster());
            row.spei = r.get_spei();
            row.flood_cooldown = r.get_flood_cooldown();
            row.soil_frac = cap > 0.0f ? r.get_soil_moisture() / cap : 0.0f;
            row.floodplain = r.get_floodplain_share();
            row.precip_anom = r.get_precip_anom();
            // Wet season as the sim's climatology has it: this month's mean >= the annual mean / 12.
            const float peak = static_cast<float>(std::max<uint8_t>(1, r.get_precip_peak_month()));
            row.wet_season = r.get_precip_mm() > 0.0f && std::cos(6.2831853f * (static_cast<float>(out.month) - peak) / 12.0f) >= 0.0f;
            row.storm_seen = r.get_storm_seen();
            row.storm_wind = r.get_storm_wind();
            row.insurgency = r.get_insurgency();
            row.unrest = r.get_unrest();
            row.grievance = r.get_grievance();
            row.protest = r.get_protest();
            row.population = r.get_population();
            row.state = r.get_state() ? static_cast<uint32_t>(r.get_state().id.index()) : 0xFFFFFFFFu;
            row.infections_100k = row.population > 0.0f ? r.get_new_infections() / row.population * 1e5f : 0.0f;
        }
        const auto count = static_cast<uint16_t>(w.nation_size());
        out.nations.resize(count);
        for(auto n : w.in_nation) {
            nation_row& row = out.nations[n.id.index()];
            const double days = std::max(1.0, static_cast<double>(n.get_acc_days()));
            const double gdp0_daily = n.get_gdp0() / 365.0;
            const double cpi_2019 = std::pow(1.0 + n.get_inflation_trend(), -0.5);
            const bool any = n.get_acc_days() > 0.5f;
            row.growth = any && gdp0_daily > 0 ? static_cast<float>((n.get_acc_gdp_real() / days / gdp0_daily - 1.0) * 100.0) : 0.0f;
            row.inflation = any ? static_cast<float>((n.get_acc_cpi() / days / cpi_2019 - 1.0) * 100.0) : 0.0f;
            row.unemployment = n.get_unemployment() * 100.0f;
            row.policy_rate = n.get_policy_rate(); // already in percent
            row.yield10 = n.get_yield10();
            const double nominal = n.get_acc_gdp_nominal();
            row.budget = nominal > 0 && n.get_acc_days() >= 30.0f
                             ? static_cast<float>((n.get_bal_gov() - n.get_acc_bal_gov_start()) / nominal * 100.0) : 0.0f;
            row.gap = n.get_gap_smoothed() * 100.0f;
            row.credit_growth = n.get_credit_growth() * 100.0f;
            row.wage_real = n.get_wage_real();
            row.house_price = n.get_house_price();
            row.equity_price = n.get_equity_price();
            row.gini = n.get_gini();
            const float gdp = n.get_gdp_real();
            row.exports_pct = gdp > 0.0f ? n.get_exports() / gdp * 100.0f : 0.0f;
            row.imports_pct = gdp > 0.0f ? n.get_imports() / gdp * 100.0f : 0.0f;
            row.tariff = n.get_tariff() * 100.0f;
            row.seaborne = n.get_seaborne_share() * 100.0f;
            row.freight = n.get_freight_cost();
            row.logistics_delay = n.get_logistics_delay();
            row.oil_production = n.get_oil_production();
            row.oil_demand = n.get_oil_demand();
            row.fossil_share = n.get_fossil_share() * 100.0f;
            row.carbon_price = n.get_carbon_price();
            row.insurgent = bool(n.get_rebel_agent());
            float total = 0.0f;
            for(uint16_t k = 0; k < count; ++k) {
                if(k == n.id.index()) continue;
                if(n.get_sanction(k) > 0.0f) ++row.sanction_links;
                if(n.get_at_war(k) > 0.0f) row.at_war = true;
                const float x = n.get_bilateral_exports(k);
                total += std::max(0.0f, x);
                for(int j = 0; j < 5; ++j) { // insert into the top five
                    if(row.partner[j] == 0xFFFF || x > row.partner_share[j]) {
                        for(int m = 4; m > j; --m) {
                            row.partner[m] = row.partner[m - 1];
                            row.partner_share[m] = row.partner_share[m - 1];
                        }
                        row.partner[j] = k;
                        row.partner_share[j] = x;
                        break;
                    }
                }
            }
            for(int j = 0; j < 5; ++j) {
                if(row.partner[j] != 0xFFFF && (total <= 0.0f || row.partner_share[j] <= 0.0f)) row.partner[j] = 0xFFFF;
                row.partner_share[j] = total > 0.0f ? row.partner_share[j] / total * 100.0f : 0.0f;
            }
        }
        if(with_faults) {
            for(auto f : w.in_fault) {
                const float rec = f.get_recurrence_years();
                out.faults.push_back({f.get_lat(), f.get_lon(), f.get_mw_char(), rec > 0.0f ? f.get_elapsed_years() / rec : 0.0f});
            }
        }
        promise->set_value(std::move(out));
    });
    return future;
}

std::future<politics_snapshot> sim_runner::request_politics(uint32_t nation_index) {
    auto promise = std::make_shared<std::promise<politics_snapshot>>();
    auto future = promise->get_future();
    post([promise, nation_index](game_state& s) {
        politics_snapshot out;
        auto const& w = s.world;
        out.tick = s.current_tick;
        if(nation_index >= w.nation_size()) {
            promise->set_value(std::move(out));
            return;
        }
        const dcon::nation_id n{static_cast<dcon::nation_id::value_base_t>(nation_index)};
        out.nation = nation_index;
        out.electoral = w.nation_get_electoral(n);
        out.liberal = w.nation_get_liberal(n);
        out.legitimacy = w.nation_get_legitimacy(n);
        out.military_loyalty = w.nation_get_military_loyalty(n);
        out.repression = w.nation_get_repression(n);
        out.approval = w.nation_get_approval(n);
        out.competitive = w.nation_get_competitive(n) != 0;
        out.next_election = w.nation_get_next_election(n);
        // Parties in local_index order (pop.party_support is indexed the same way); stable, so
        // parties with equal (e.g. zero) indices keep their membership order.
        std::vector<std::pair<uint8_t, dcon::party_id>> parties;
        for(auto m : w.nation_get_party_membership(n)) parties.emplace_back(m.get_party().get_local_index(), m.get_party().id);
        std::stable_sort(parties.begin(), parties.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        if(parties.size() > politics_max_parties) parties.resize(politics_max_parties);
        const auto incumbent = w.nation_get_incumbent(n);
        for(auto const& [li, p] : parties) {
            party_info info;
            info.key = core::unpack_key(w.party_get_key(p));
            info.family = w.party_get_family(p);
            info.vote_share = w.party_get_vote_share(p);
            info.seat_share = w.party_get_seat_share(p);
            info.in_government = w.party_get_in_government(p) != 0;
            info.incumbent = p == incumbent;
            out.parties.push_back(std::move(info));
        }
        constexpr float turnout[4] = {0.5f, 0.6f, 0.75f, 0.7f}; // by pop.kind & 3, as politics.cpp
        for(auto own : w.nation_get_region_ownership(n)) {
            auto r = own.get_region();
            politics_region pr;
            pr.region = static_cast<uint32_t>(r.id.index());
            double grievance = 0.0;
            for(auto loc : r.get_pop_location()) {
                auto p = loc.get_pop();
                const float size = p.get_size();
                const float v = size * turnout[p.get_kind() & 3];
                pr.electorate += size;
                pr.voters += v;
                grievance += static_cast<double>(size) * p.get_grievance();
                for(std::size_t j = 0; j < out.parties.size(); ++j) pr.share[j] += v * p.get_party_support(static_cast<uint8_t>(j));
            }
            float total = 0.0f;
            for(std::size_t j = 0; j < out.parties.size(); ++j) total += pr.share[j];
            for(std::size_t j = 0; j < out.parties.size(); ++j) pr.share[j] = total > 0.0f ? pr.share[j] / total : 0.0f;
            pr.grievance = pr.electorate > 0.0f ? static_cast<float>(grievance / pr.electorate) : r.get_grievance();
            pr.trust = r.get_trust_r();
            pr.unrest = r.get_unrest();
            pr.protest = r.get_protest();
            pr.insurgency = r.get_insurgency();
            out.regions.push_back(pr);
        }
        promise->set_value(std::move(out));
    });
    return future;
}

} // namespace geosim::client
