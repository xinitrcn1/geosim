// Simultaneous epidemics: one two-age-group SEIR per pathogen on the same
// population (docs/04-society.md#health-and-epidemics). Pathogens are data
// (assets/defs/pathogens.txt). They interact through
//   - shared hospital capacity: overload from all pathogens raises every IFR,
//   - cross-immunity: recovery from q removes a share of susceptibility to p,
//   - deaths: people dying of one pathogen leave every pathogen's compartments,
//   - behaviour and policy, which respond to the combined (severity-weighted) burden.
//
// 1. Scalar pre-pass per region: transmission per pathogen (NPIs, behaviour,
//    tracing, testing, suppression, season), imports, hospital overload.
// 2. SIMD pass over all regions (DCON execute_parallel_over_region, TBB).
// 3. Scalar post-pass: reported deaths and 7-day averages.
// Compartment arrays are indexed epi_index(pathogen, age).

#include "core/calendar.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace geosim::sim {

namespace {
constexpr int max_pathogens = 8;
}

void update_epidemic(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const uint8_t NP = D.pathogen_count;
    if(NP == 0) {
        return;
    }

    // Pandemic fatigue: accumulated days under containment (C-indicator mean level).
    for(auto n : w.in_nation) {
        float sum = 0.0f;
        int count = 0;
        for(uint8_t k = 0; k < D.npi_count; ++k) {
            if(w.npi_indicator_get_transmission_effect(dcon::npi_indicator_id{k}) > 0.0f) {
                sum += npi_fraction(state, n.id, k);
                ++count;
            }
        }
        n.set_npi_exposure(n.get_npi_exposure() + (count ? sum / static_cast<float>(count) : 0.0f));
    }

    // World prevalence per pathogen (serial, index order: deterministic).
    std::array<double, max_pathogens> world_i{};
    double world_n = 0.0;
    for(auto r : w.in_region) {
        for(uint8_t a = 0; a < age_groups; ++a) {
            const uint8_t i0 = epi_index(0, a);
            world_n += r.get_ep_s(i0) + r.get_ep_e(i0) + r.get_ep_i(i0) + r.get_ep_r(i0);
            for(uint8_t p = 0; p < NP; ++p) {
                world_i[p] += r.get_ep_i(epi_index(p, a));
            }
        }
    }

    const auto date = core::date_from_tick(state.current_tick);
    const float season_phase = std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(core::day_of_year(date) - 15) / 365.0f);

    // ---- 1. pre-pass
    for(auto r : w.in_region) {
        const auto n = r.get_region_ownership().get_nation();
        const float pop = std::max(1.0f, r.get_population());

        // Compliance: lower with informality (people must keep working), fading with
        // fatigue (second-wave NPI effects were about half the first-wave ones);
        // an enforced suppression strategy restores it.
        const float fatigue = static_cast<float>(P.fatigue_max)
                              * std::min(1.0f, w.nation_get_npi_exposure(n) / static_cast<float>(P.fatigue_days));
        const float tti = w.nation_get_tti_strength(n);
        const float compliance = std::max(tti, static_cast<float>(P.npi_compliance)
                                                   * (1.0f - static_cast<float>(P.informality_compliance_loss) * w.nation_get_informality(n))
                                                   * (1.0f - fatigue));
        const float deaths_pm = r.get_deaths7() / pop * 1e6f;
        const float voluntary = 1.0f / (1.0f + static_cast<float>(P.voluntary_k) * (1.0f - fatigue) * deaths_pm);
        const float testing = D.npi_testing != 0xFF ? npi_fraction(state, n, D.npi_testing) : 0.0f;
        const float tracing = D.npi_tracing != 0xFF ? npi_fraction(state, n, D.npi_tracing) : 0.0f;
        const float detected_share = 0.2f + 0.8f * testing; // relative to max testing
        // Outbreaks are spatially concentrated: tracing saturates on the case load of a
        // cluster-sized population (all pathogens compete for the same tracers).
        float detected = 0.0f;
        for(uint8_t p = 0; p < NP; ++p) {
            detected += r.get_infections_p(p) * w.pathogen_get_detection_ratio(dcon::pathogen_id{p}) * detected_share;
        }
        const float cluster = std::min(pop, static_cast<float>(P.tracing_cluster_pop));
        const float capacity = static_cast<float>(P.tracing_capacity) * (1.0f + static_cast<float>(P.tti_capacity_mult) * tti);
        const float trace_room = std::max(0.0f, 1.0f - detected / cluster * 1e6f / capacity);
        const float trace_level = std::max({0.0f, 2.0f * tracing - 1.0f, tti});
        const float trace_max = static_cast<float>(P.tracing_max) + static_cast<float>(P.tti_extra_effect) * tti;
        const float test_factor = 1.0f - static_cast<float>(P.testing_effect) * testing;
        // Centralised isolation of cases works even when tracing is saturated (Pan et al. 2020).
        const float isolation = 1.0f - static_cast<float>(P.tti_isolation) * tti;
        const float border = D.npi_travel != 0xFF ? npi_fraction(state, n, D.npi_travel) : 0.0f;
        const float quarantine = static_cast<float>(P.quarantine_effect) * tti;

        float own_n = 0.0f;
        for(uint8_t a = 0; a < age_groups; ++a) {
            const uint8_t i0 = epi_index(0, a);
            own_n += r.get_ep_s(i0) + r.get_ep_e(i0) + r.get_ep_i(i0) + r.get_ep_r(i0);
        }
        float hospital_need = 0.0f;
        for(uint8_t p = 0; p < NP; ++p) {
            const dcon::pathogen_id pid{p};
            const float sens = w.pathogen_get_npi_sensitivity(pid);
            float npi_factor = 1.0f;
            for(uint8_t k = 0; k < D.npi_count; ++k) {
                // Regional measures target hotspots, so they reach most (not all) transmission.
                const float reach = 1.0f - (1.0f - std::max(static_cast<float>(P.npi_targeted_transmission), tti)) * w.nation_get_npi_targeted(n, k);
                npi_factor *= 1.0f - compliance * sens * w.npi_indicator_get_transmission_effect(dcon::npi_indicator_id{k})
                                         * npi_fraction(state, n, k) * reach;
            }
            // Tracing only follows detected chains of this pathogen.
            const float traceable = std::min(1.0f, w.pathogen_get_detection_ratio(pid) / static_cast<float>(P.tracing_reference_detection));
            const float trace_factor = 1.0f - trace_max * trace_level * trace_room * std::max(detected_share, tti) * traceable;
            const float season = 1.0f + w.pathogen_get_seasonality(pid) * season_phase * r.get_hemisphere();
            const float gamma = 1.0f / w.pathogen_get_infectious_days(pid);
            r.set_beta(p, w.pathogen_get_r0(pid) * gamma * npi_factor * voluntary * trace_factor * test_factor * isolation * season);

            double own_i = 0.0;
            for(uint8_t a = 0; a < age_groups; ++a) {
                own_i += r.get_ep_i(epi_index(p, a));
            }
            const double other_n = world_n - own_n;
            const float prevalence = other_n > 0.0 ? static_cast<float>((world_i[p] - own_i) / other_n) : 0.0f;
            r.set_imported(p, static_cast<float>(P.travel_rate) * pop * prevalence * (1.0f - static_cast<float>(P.border_effect) * border)
                                  * (1.0f - quarantine));
            hospital_need += w.pathogen_get_hosp_share(pid) * static_cast<float>(own_i);
        }
        // Shared hospital capacity: overload from any pathogen raises every IFR.
        const float beds = r.get_beds_per_1000() / 1000.0f * pop * static_cast<float>(P.epidemic_bed_share);
        const float overload = beds > 0.0f ? std::max(0.0f, hospital_need / beds - 1.0f) : 0.0f;
        r.set_ifr_mult(1.0f + static_cast<float>(P.overload_ifr_mult) * std::min(1.0f, overload));
    }

    // ---- 2. SIMD SEIR step (dt = 1 day), all pathogens per lane
    struct pathogen_consts {
        ve::fp_vector sigma, gamma, delay, ifr_y, ifr_o, c_old, waning, severity;
        std::array<ve::fp_vector, max_pathogens> cross;
    };
    std::array<pathogen_consts, max_pathogens> pc;
    for(uint8_t p = 0; p < NP; ++p) {
        const dcon::pathogen_id pid{p};
        pc[p].sigma = ve::fp_vector(1.0f / w.pathogen_get_incubation_days(pid));
        pc[p].gamma = ve::fp_vector(1.0f / w.pathogen_get_infectious_days(pid));
        pc[p].delay = ve::fp_vector(1.0f / w.pathogen_get_death_delay_days(pid));
        pc[p].ifr_y = ve::fp_vector(w.pathogen_get_ifr_young(pid));
        pc[p].ifr_o = ve::fp_vector(w.pathogen_get_ifr_old(pid));
        pc[p].c_old = ve::fp_vector(w.pathogen_get_old_contact_factor(pid));
        const float wd = w.pathogen_get_waning_days(pid);
        pc[p].waning = ve::fp_vector(wd > 0.0f ? 1.0f / wd : 0.0f);
        pc[p].severity = ve::fp_vector(w.pathogen_get_severity(pid));
        for(uint8_t q = 0; q < NP; ++q) {
            pc[p].cross[q] = ve::fp_vector(w.pathogen_get_cross_immunity(pid, q));
        }
    }
    const ve::fp_vector one(1.0f);
    const ve::fp_vector zero(0.0f);

    w.execute_parallel_over_region([&](auto ids) {
        const auto ifr_mult = w.region_get_ifr_mult(ids);
        // Recovered share per pathogen (before the step), for cross-immunity.
        std::array<ve::fp_vector, max_pathogens> recovered_share{};
        for(uint8_t q = 0; q < NP; ++q) {
            ve::fp_vector rec(0.0f), alive(0.0f);
            for(uint8_t a = 0; a < age_groups; ++a) {
                const uint8_t i = epi_index(q, a);
                rec = rec + w.region_get_ep_r(ids, i);
                alive = alive + w.region_get_ep_s(ids, i) + w.region_get_ep_e(ids, i) + w.region_get_ep_i(ids, i) + w.region_get_ep_r(ids, i);
            }
            recovered_share[q] = rec / ve::max(alive, one);
        }
        std::array<std::array<ve::fp_vector, age_groups>, max_pathogens> died{};
        ve::fp_vector infections_total(0.0f), severe_total(0.0f), deaths_total(0.0f);
        std::array<ve::fp_vector, age_groups> deaths_age{zero, zero};

        for(uint8_t p = 0; p < NP; ++p) {
            auto const& c = pc[p];
            const uint8_t y = epi_index(p, age_young);
            const uint8_t o = epi_index(p, age_old);
            const auto beta = w.region_get_beta(ids, p);
            auto sy = w.region_get_ep_s(ids, y), ey = w.region_get_ep_e(ids, y), iy = w.region_get_ep_i(ids, y);
            auto ry = w.region_get_ep_r(ids, y), hy = w.region_get_ep_h(ids, y), dy = w.region_get_ep_d(ids, y);
            auto so = w.region_get_ep_s(ids, o), eo = w.region_get_ep_e(ids, o), io = w.region_get_ep_i(ids, o);
            auto ro = w.region_get_ep_r(ids, o), ho = w.region_get_ep_h(ids, o), d_o = w.region_get_ep_d(ids, o);

            ve::fp_vector protection(0.0f);
            for(uint8_t q = 0; q < NP; ++q) {
                if(q != p) protection = protection + c.cross[q] * recovered_share[q];
            }
            const auto susceptibility = ve::max(zero, one - protection);
            const auto ny = sy + ey + iy + ry;
            const auto no = so + eo + io + ro;
            const auto mix = (iy + c.c_old * io) / ve::max(ny + c.c_old * no, one);
            const auto inf_y = ve::min(sy, beta * mix * sy * susceptibility);
            const auto inf_o = ve::min(so, beta * c.c_old * mix * so * susceptibility);
            const auto imported = ve::max(zero, ve::min(w.region_get_imported(ids, p), sy - inf_y));
            const auto onset_y = c.sigma * ey, onset_o = c.sigma * eo;
            const auto out_y = c.gamma * iy, out_o = c.gamma * io;
            const auto fatal_y = ve::min(one, c.ifr_y * ifr_mult) * out_y;
            const auto fatal_o = ve::min(one, c.ifr_o * ifr_mult) * out_o;
            const auto die_y = c.delay * hy, die_o = c.delay * ho;
            const auto wane_y = c.waning * ry, wane_o = c.waning * ro;

            sy = sy - inf_y - imported + wane_y;
            ey = ey + inf_y + imported - onset_y;
            iy = iy + onset_y - out_y;
            ry = ry + out_y - fatal_y - wane_y;
            hy = hy + fatal_y - die_y;
            dy = dy + die_y;
            so = so - inf_o + wane_o;
            eo = eo + inf_o - onset_o;
            io = io + onset_o - out_o;
            ro = ro + out_o - fatal_o - wane_o;
            ho = ho + fatal_o - die_o;
            d_o = d_o + die_o;

            w.region_set_ep_s(ids, y, sy); w.region_set_ep_e(ids, y, ey); w.region_set_ep_i(ids, y, iy);
            w.region_set_ep_r(ids, y, ry); w.region_set_ep_h(ids, y, hy); w.region_set_ep_d(ids, y, dy);
            w.region_set_ep_s(ids, o, so); w.region_set_ep_e(ids, o, eo); w.region_set_ep_i(ids, o, io);
            w.region_set_ep_r(ids, o, ro); w.region_set_ep_h(ids, o, ho); w.region_set_ep_d(ids, o, d_o);

            const auto infections = inf_y + inf_o + imported;
            w.region_set_infections_p(ids, p, infections);
            w.region_set_deaths_p(ids, p, die_y + die_o);
            infections_total = infections_total + infections;
            severe_total = severe_total + infections * c.severity;
            deaths_total = deaths_total + die_y + die_o;
            died[p][age_young] = die_y;
            died[p][age_old] = die_o;
            deaths_age[age_young] = deaths_age[age_young] + die_y;
            deaths_age[age_old] = deaths_age[age_old] + die_o;
        }
        // People who died of another pathogen leave this pathogen's compartments too.
        if(NP > 1) {
            for(uint8_t p = 0; p < NP; ++p) {
                for(uint8_t a = 0; a < age_groups; ++a) {
                    const uint8_t i = epi_index(p, a);
                    const auto others = deaths_age[a] - died[p][a];
                    const auto alive = w.region_get_ep_s(ids, i) + w.region_get_ep_e(ids, i) + w.region_get_ep_i(ids, i)
                                       + w.region_get_ep_r(ids, i);
                    const auto keep = ve::max(zero, one - others / ve::max(alive, one));
                    w.region_set_ep_s(ids, i, w.region_get_ep_s(ids, i) * keep);
                    w.region_set_ep_e(ids, i, w.region_get_ep_e(ids, i) * keep);
                    w.region_set_ep_i(ids, i, w.region_get_ep_i(ids, i) * keep);
                    w.region_set_ep_r(ids, i, w.region_get_ep_r(ids, i) * keep);
                }
            }
        }
        w.region_set_new_infections(ids, infections_total);
        w.region_set_severe_infections(ids, severe_total);
        w.region_set_new_deaths(ids, deaths_total);
        // Deaths by age, removed from the pops by demography.
        w.region_set_epi_deaths_young(ids, w.region_get_epi_deaths_young(ids) + deaths_age[age_young]);
        w.region_set_epi_deaths_old(ids, w.region_get_epi_deaths_old(ids) + deaths_age[age_old]);
    });

    // ---- 3. post-pass: reporting
    for(auto r : w.in_region) {
        const auto n = r.get_region_ownership().get_nation();
        const float reporting = w.nation_get_death_reporting(n);
        float reported = 0.0f;
        for(uint8_t p = 0; p < NP; ++p) {
            const float rp = r.get_deaths_p(p) * reporting;
            r.set_deaths_reported_p(p, r.get_deaths_reported_p(p) + rp);
            r.set_cum_infections_p(p, r.get_cum_infections_p(p) + r.get_infections_p(p));
            reported += rp;
        }
        r.set_deaths_reported(r.get_deaths_reported() + reported);
        r.set_deaths7(r.get_deaths7() + (reported - r.get_deaths7()) / 7.0f);
    }
}

} // namespace geosim::sim
