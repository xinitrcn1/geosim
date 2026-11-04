// Metapopulation SEIR with two age groups (docs/04-society.md#health-and-epidemics).
//
// 1. Scalar pre-pass per region: effective transmission (NPIs, behaviour,
//    tracing, testing, season), imported infections, hospital overload.
// 2. SIMD pass over all regions (DCON execute_parallel_over_region, TBB):
//    S -> E -> I -> {R, H}, H -> D.
// 3. Scalar post-pass: reported deaths and 7-day averages.

#include "core/calendar.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace geosim::sim {

void update_epidemic(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;

    // World prevalence (serial, index order: deterministic).
    double world_i = 0.0;
    double world_n = 0.0;
    for(auto r : w.in_region) {
        for(uint8_t a = 0; a < age_groups; ++a) {
            world_i += r.get_ep_i(a);
            world_n += r.get_ep_s(a) + r.get_ep_e(a) + r.get_ep_i(a) + r.get_ep_r(a);
        }
    }

    const auto date = core::date_from_tick(state.current_tick);
    const float season_phase = std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(core::day_of_year(date) - 15) / 365.0f);
    const float gamma = 1.0f / static_cast<float>(P.infectious_days);

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

    // ---- 1. pre-pass
    for(auto r : w.in_region) {
        const auto n = r.get_region_ownership().get_nation();
        const float pop = std::max(1.0f, r.get_population());

        // Compliance: lower with informality (people must keep working), and fading with
        // fatigue (second-wave NPI effects were about half the first-wave ones).
        const float fatigue = static_cast<float>(P.fatigue_max)
                              * std::min(1.0f, w.nation_get_npi_exposure(n) / static_cast<float>(P.fatigue_days));
        const float tti = w.nation_get_tti_strength(n);
        // An enforced suppression strategy restores compliance.
        const float compliance = std::max(tti, static_cast<float>(P.npi_compliance)
                                 * (1.0f - static_cast<float>(P.informality_compliance_loss) * w.nation_get_informality(n))
                                 * (1.0f - fatigue));
        float npi_factor = 1.0f;
        for(uint8_t k = 0; k < D.npi_count; ++k) {
            // Regional measures target hotspots, so they reach most (not all) transmission.
            const float reach = 1.0f - (1.0f - std::max(static_cast<float>(P.npi_targeted_transmission), tti)) * w.nation_get_npi_targeted(n, k);
            npi_factor *= 1.0f - compliance * w.npi_indicator_get_transmission_effect(dcon::npi_indicator_id{k})
                                     * npi_fraction(state, n, k) * reach;
        }
        const float deaths_pm = r.get_deaths7() / pop * 1e6f;
        const float voluntary = 1.0f / (1.0f + static_cast<float>(P.voluntary_k) * (1.0f - fatigue) * deaths_pm);
        const float testing = D.npi_testing != 0xFF ? npi_fraction(state, n, D.npi_testing) : 0.0f;
        const float tracing = D.npi_tracing != 0xFF ? npi_fraction(state, n, D.npi_tracing) : 0.0f;
        const float detected_share = 0.2f + 0.8f * testing; // relative to max testing
        const float detect = static_cast<float>(P.detection_ratio) * detected_share;
        // Outbreaks are spatially concentrated: tracing saturates on the case load of a
        // cluster-sized population, not on the national average (matters for large nations).
        // A suppression strategy (mass testing, centralised quarantine) multiplies tracing
        // capacity and strength and makes borders tight.
        const float cluster = std::min(pop, static_cast<float>(P.tracing_cluster_pop));
        const float cases_pm = r.get_new_infections() * detect / cluster * 1e6f;
        const float capacity = static_cast<float>(P.tracing_capacity) * (1.0f + static_cast<float>(P.tti_capacity_mult) * tti);
        const float trace_room = std::max(0.0f, 1.0f - cases_pm / capacity);
        // Only comprehensive tracing (upper half of the H3 scale) is effective at scale.
        const float trace_level = std::max({0.0f, 2.0f * tracing - 1.0f, tti});
        const float trace_max = static_cast<float>(P.tracing_max) + static_cast<float>(P.tti_extra_effect) * tti;
        // Tracing can only follow detected chains.
        const float trace_factor = 1.0f - trace_max * trace_level * trace_room * std::max(detected_share, tti);
        const float test_factor = 1.0f - static_cast<float>(P.testing_effect) * testing;
        // Centralised isolation of cases works even when tracing is saturated (Pan et al. 2020).
        const float isolation = 1.0f - static_cast<float>(P.tti_isolation) * tti;
        const float season = 1.0f + static_cast<float>(P.seasonality) * season_phase * r.get_hemisphere();
        r.set_beta(static_cast<float>(P.r0) * gamma * npi_factor * voluntary * trace_factor * test_factor * isolation * season);

        double own_i = 0.0;
        double own_n = 0.0;
        for(uint8_t a = 0; a < age_groups; ++a) {
            own_i += r.get_ep_i(a);
            own_n += r.get_ep_s(a) + r.get_ep_e(a) + r.get_ep_i(a) + r.get_ep_r(a);
        }
        const double other_n = world_n - own_n;
        const float prevalence = other_n > 0.0 ? static_cast<float>((world_i - own_i) / other_n) : 0.0f;
        const float border = D.npi_travel != 0xFF ? npi_fraction(state, n, D.npi_travel) : 0.0f;
        const float quarantine = static_cast<float>(P.quarantine_effect) * w.nation_get_tti_strength(n);
        r.set_imported(static_cast<float>(P.travel_rate) * pop * prevalence * (1.0f - static_cast<float>(P.border_effect) * border)
                       * (1.0f - quarantine));

        const float infectious = r.get_ep_i(age_young) + r.get_ep_i(age_old);
        const float beds = r.get_beds_per_1000() / 1000.0f * pop * static_cast<float>(P.covid_bed_share);
        const float overload = beds > 0.0f ? std::max(0.0f, static_cast<float>(P.hosp_share) * infectious / beds - 1.0f) : 0.0f;
        r.set_ifr_mult(1.0f + static_cast<float>(P.overload_ifr_mult) * std::min(1.0f, overload));
    }

    // ---- 2. SIMD SEIR step (dt = 1 day)
    const ve::fp_vector sigma(1.0f / static_cast<float>(P.incubation_days));
    const ve::fp_vector gam(gamma);
    const ve::fp_vector delay(1.0f / static_cast<float>(P.death_delay_days));
    const ve::fp_vector ifr_y(static_cast<float>(P.ifr_young));
    const ve::fp_vector ifr_o(static_cast<float>(P.ifr_old));
    const ve::fp_vector c_old(static_cast<float>(P.old_contact_factor));
    const ve::fp_vector one(1.0f);
    const ve::fp_vector zero(0.0f);

    w.execute_parallel_over_region([&](auto ids) {
        const auto beta = w.region_get_beta(ids);
        const auto ifr_mult = w.region_get_ifr_mult(ids);
        auto sy = w.region_get_ep_s(ids, age_young);
        auto ey = w.region_get_ep_e(ids, age_young);
        auto iy = w.region_get_ep_i(ids, age_young);
        auto ry = w.region_get_ep_r(ids, age_young);
        auto hy = w.region_get_ep_h(ids, age_young);
        auto dy = w.region_get_ep_d(ids, age_young);
        auto so = w.region_get_ep_s(ids, age_old);
        auto eo = w.region_get_ep_e(ids, age_old);
        auto io = w.region_get_ep_i(ids, age_old);
        auto ro = w.region_get_ep_r(ids, age_old);
        auto ho = w.region_get_ep_h(ids, age_old);
        auto d_o = w.region_get_ep_d(ids, age_old);

        const auto ny = sy + ey + iy + ry;
        const auto no = so + eo + io + ro;
        const auto mix = (iy + c_old * io) / ve::max(ny + c_old * no, one);
        const auto inf_y = ve::min(sy, beta * mix * sy);
        const auto inf_o = ve::min(so, beta * c_old * mix * so);
        const auto imported = ve::max(zero, ve::min(w.region_get_imported(ids), sy - inf_y));

        const auto onset_y = sigma * ey;
        const auto onset_o = sigma * eo;
        const auto out_y = gam * iy;
        const auto out_o = gam * io;
        const auto fatal_y = ve::min(one, ifr_y * ifr_mult) * out_y;
        const auto fatal_o = ve::min(one, ifr_o * ifr_mult) * out_o;
        const auto die_y = delay * hy;
        const auto die_o = delay * ho;

        sy = sy - inf_y - imported;
        ey = ey + inf_y + imported - onset_y;
        iy = iy + onset_y - out_y;
        ry = ry + out_y - fatal_y;
        hy = hy + fatal_y - die_y;
        dy = dy + die_y;
        so = so - inf_o;
        eo = eo + inf_o - onset_o;
        io = io + onset_o - out_o;
        ro = ro + out_o - fatal_o;
        ho = ho + fatal_o - die_o;
        d_o = d_o + die_o;

        w.region_set_ep_s(ids, age_young, sy);
        w.region_set_ep_e(ids, age_young, ey);
        w.region_set_ep_i(ids, age_young, iy);
        w.region_set_ep_r(ids, age_young, ry);
        w.region_set_ep_h(ids, age_young, hy);
        w.region_set_ep_d(ids, age_young, dy);
        w.region_set_ep_s(ids, age_old, so);
        w.region_set_ep_e(ids, age_old, eo);
        w.region_set_ep_i(ids, age_old, io);
        w.region_set_ep_r(ids, age_old, ro);
        w.region_set_ep_h(ids, age_old, ho);
        w.region_set_ep_d(ids, age_old, d_o);
        w.region_set_new_infections(ids, inf_y + inf_o + imported);
        w.region_set_new_deaths(ids, die_y + die_o);
    });

    // ---- 3. post-pass: reporting
    for(auto r : w.in_region) {
        const auto n = r.get_region_ownership().get_nation();
        const float reported = r.get_new_deaths() * w.nation_get_death_reporting(n);
        r.set_deaths_reported(r.get_deaths_reported() + reported);
        r.set_deaths7(r.get_deaths7() + (reported - r.get_deaths7()) / 7.0f);
        r.set_cum_infections(r.get_cum_infections() + r.get_new_infections());
    }
}

} // namespace geosim::sim
