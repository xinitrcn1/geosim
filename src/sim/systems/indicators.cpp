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

dcon::nation_id peg_anchor(game_state const& state, dcon::nation_id n) noexcept {
    const auto rel = state.world.nation_get_currency_peg_as_pegged(n);
    return rel ? state.world.currency_peg_get_anchor(rel) : dcon::nation_id{};
}

dcon::nation_id sovereign_of(game_state const& state, dcon::nation_id n, float& autonomy) noexcept {
    autonomy = 1.0f;
    const auto rel = state.world.nation_get_sovereignty_as_subject(n);
    if(!rel) {
        return {};
    }
    const dcon::nation_id sov = state.world.sovereignty_get_sovereign(rel);
    if(sov) {
        autonomy = state.world.sovereignty_get_autonomy(rel);
    }
    return sov;
}

// Most populous region: entry point for seeds and international migration.
dcon::region_id capital_region(game_state const& state, dcon::nation_id n) noexcept {
    dcon::region_id best;
    float pop = -1.0f;
    for(auto own : state.world.nation_get_region_ownership(n)) {
        if(own.get_region().get_population() > pop) {
            pop = own.get_region().get_population();
            best = own.get_region().id;
        }
    }
    return best;
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

namespace {
struct point {
    double x;
    double weight;
};
std::vector<point> income_points(game_state const& state, dcon::nation_id n, float sigma_override);
} // namespace

float compute_gini(game_state const& state, dcon::nation_id n, float sigma_override) {
    auto pts = income_points(state, n, sigma_override);
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

// Palma ratio (income share of the top 10% / bottom 40%) and relative poverty
// (share below half the median), from the same quadrature as the Gini.
void compute_distribution(game_state const& state, dcon::nation_id n, float& palma, float& poverty) {
    auto pts = income_points(state, n, -1.0f);
    palma = 0.0f;
    poverty = 0.0f;
    if(pts.empty()) return;
    std::sort(pts.begin(), pts.end(), [](point const& a, point const& b) { return a.x < b.x; });
    double wt = 0.0, inc = 0.0;
    for(auto const& pt : pts) {
        wt += pt.weight;
        inc += pt.weight * pt.x;
    }
    double cum_w = 0.0, bottom = 0.0, top = 0.0, median = 0.0;
    bool have_median = false;
    for(auto const& pt : pts) {
        const double before = cum_w;
        cum_w += pt.weight;
        const double share_lo = std::clamp((0.4 * wt - before) / pt.weight, 0.0, 1.0);
        bottom += share_lo * pt.weight * pt.x;
        const double share_hi = std::clamp((cum_w - 0.9 * wt) / pt.weight, 0.0, 1.0);
        top += share_hi * pt.weight * pt.x;
        if(!have_median && cum_w >= 0.5 * wt) {
            median = pt.x;
            have_median = true;
        }
    }
    palma = bottom > 0.0 ? static_cast<float>(top / bottom) : 0.0f;
    double poor = 0.0;
    for(auto const& pt : pts) {
        if(pt.x < 0.5 * median) poor += pt.weight;
    }
    poverty = static_cast<float>(poor / wt);
    (void)inc;
}

namespace {

std::vector<point> income_points(game_state const& state, dcon::nation_id n, float sigma_override) {
    auto const& w = state.world;
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
    return pts;
}

} // namespace

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
            float palma = 0.0f, poverty = 0.0f;
            compute_distribution(state, n.id, palma, poverty);
            n.set_palma(palma);
            n.set_poverty(poverty);
            if(n.get_poverty0() <= 0.0f) n.set_poverty0(poverty);
        }
    }
}

} // namespace geosim::sim
