// Accumulators for annual indicators, the income-distribution Gini, and
// small helpers shared by the systems.

#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace geosim::sim {

float growth_factor(game_state const& state, dcon::nation_id n) noexcept {
    const float g = state.world.nation_get_trend_growth(n);
    return std::exp(std::log1p(g) * static_cast<float>(state.current_tick) / 365.0f);
}

float npi_fraction(game_state const& state, dcon::nation_id n, uint8_t k) noexcept {
    const float max = state.world.npi_indicator_get_max_level(dcon::npi_indicator_id{k});
    return max > 0.0f ? std::clamp(state.world.nation_get_npi(n, k) / max, 0.0f, 1.0f) : 0.0f;
}

dcon::region_id capital_region(game_state const& state, dcon::nation_id n) noexcept {
    for(auto own : state.world.nation_get_region_ownership(n)) {
        return own.get_region().id;
    }
    return {};
}

float reported_deaths_per_million_daily(game_state const& state, dcon::nation_id n) noexcept {
    double deaths = 0.0;
    double pop = 0.0;
    for(auto own : state.world.nation_get_region_ownership(n)) {
        deaths += own.get_region().get_deaths7();
        pop += own.get_region().get_population();
    }
    return pop > 0.0 ? static_cast<float>(deaths / pop * 1e6) : 0.0f;
}

namespace {

// Acklam's rational approximation of the inverse standard normal CDF.
double inv_norm(double p) {
    static constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                                   1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                                   6.680131188771972e+01, -1.328068155288572e+01};
    static constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                                   -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                                   3.754408661907416e+00};
    if(p < 0.02425) {
        const double q = std::sqrt(-2 * std::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5])
               / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    }
    if(p > 1 - 0.02425) {
        return -inv_norm(1 - p);
    }
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q
           / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
}

constexpr int quantiles = 24;

} // namespace

float compute_gini(game_state const& state, dcon::nation_id n, float sigma_override) {
    auto const& w = state.world;
    struct point {
        double x;
        double weight;
    };
    std::vector<point> pts;
    std::array<double, quantiles> z{};
    for(int j = 0; j < quantiles; ++j) {
        z[static_cast<std::size_t>(j)] = inv_norm((j + 0.5) / quantiles);
    }

    // Floor for people with no measured income (informal earnings, family support).
    double total_income = 0.0;
    double total_people = 0.0;
    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) {
            auto p = loc.get_pop();
            total_income += std::max(0.0f, p.get_income());
            total_people += p.get_size();
        }
    }
    const double floor = total_people > 0.0 ? 0.1 * total_income / total_people : 1.0;

    auto add_group = [&](double people, double income, double sigma) {
        if(people <= 0.0) {
            return;
        }
        const double mean = std::max(floor, income / people);
        const double mu = std::log(mean) - 0.5 * sigma * sigma;
        for(int j = 0; j < quantiles; ++j) {
            pts.push_back({std::exp(mu + sigma * z[static_cast<std::size_t>(j)]), people / quantiles});
        }
    };

    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) {
            auto p = loc.get_pop();
            const double sigma = sigma_override >= 0.0f ? sigma_override : p.get_inc_sigma();
            if(p.get_kind() < worker_kinds) {
                double employed = 0.0;
                for(uint8_t s = 0; s < state.derived.sector_count; ++s) {
                    employed += p.get_employed(s);
                }
                const double unemployed = p.get_unemployed();
                const double inactive = p.get_inactive();
                const double people = employed + unemployed + inactive;
                const double other_per_head = people > 0.0 ? p.get_inc_other() / people : 0.0;
                add_group(employed, p.get_inc_work() + other_per_head * employed, sigma);
                add_group(unemployed, p.get_inc_benefit() + other_per_head * unemployed, sigma);
                add_group(inactive, other_per_head * inactive, sigma);
            } else {
                add_group(p.get_size(), std::max(0.0f, p.get_income()), sigma);
            }
        }
    }
    if(pts.empty()) {
        return 0.0f;
    }
    std::sort(pts.begin(), pts.end(), [](point const& a, point const& b) { return a.x < b.x; });
    double weight_total = 0.0;
    double income_total = 0.0;
    for(auto const& pt : pts) {
        weight_total += pt.weight;
        income_total += pt.weight * pt.x;
    }
    // G = 1 - sum_i w_i (C_{i-1} + C_i) / (W * C_total), C = cumulative income.
    double cum = 0.0;
    double area = 0.0;
    for(auto const& pt : pts) {
        const double prev = cum;
        cum += pt.weight * pt.x;
        area += pt.weight * (prev + cum);
    }
    return static_cast<float>(1.0 - area / (weight_total * income_total));
}

void update_indicators(game_state& state) {
    auto& w = state.world;
    for(auto n : w.in_nation) {
        double real = 0.0;
        for(uint8_t s = 0; s < state.derived.sector_count; ++s) {
            real += n.get_output(s);
        }
        n.set_acc_gdp_real(n.get_acc_gdp_real() + real);
        n.set_acc_gdp_nominal(n.get_acc_gdp_nominal() + real * n.get_core_price());
        n.set_acc_unemployment(n.get_acc_unemployment() + n.get_unemployment());
        n.set_acc_cpi(n.get_acc_cpi() + n.get_cpi());
        n.set_acc_days(n.get_acc_days() + 1.0);
        if(state.current_tick % 28 == n.id.index() % 28) {
            n.set_gini(compute_gini(state, n.id));
        }
    }
}

} // namespace geosim::sim
