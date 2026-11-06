// Accumulators for annual indicators, the income-distribution Gini, and
// small helpers shared by the systems.

#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <unordered_map>
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

namespace {
// Gini of sorted points.
float gini_of(std::vector<point> const& pts) {
    if(pts.empty()) {
        return 0.0f;
    }
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
// Palma and poverty of sorted points.
void distribution_of(std::vector<point> const& pts, float& palma, float& poverty) {
    palma = 0.0f;
    poverty = 0.0f;
    if(pts.empty()) return;
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

std::vector<point> sorted_points(game_state const& state, dcon::nation_id n, float sigma_override) {
    auto pts = income_points(state, n, sigma_override);
    std::sort(pts.begin(), pts.end(), [](point const& a, point const& b) { return a.x < b.x; });
    return pts;
}
} // namespace

float compute_gini(game_state const& state, dcon::nation_id n, float sigma_override) {
    return gini_of(sorted_points(state, n, sigma_override));
}

void compute_distribution(game_state const& state, dcon::nation_id n, float& palma, float& poverty) {
    distribution_of(sorted_points(state, n, -1.0f), palma, poverty);
}

namespace {

std::vector<point> income_points(game_state const& state, dcon::nation_id n, float sigma_override) {
    auto const& w = state.world;
    std::vector<point> pts;
    static const std::array<double, quantiles> z = [] {
        std::array<double, quantiles> q{};
        for(int j = 0; j < quantiles; ++j) q[static_cast<std::size_t>(j)] = inv_norm((j + 0.5) / quantiles);
        return q;
    }();

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

    // Groups are binned (0.5 % of mean income, 0.005 of sigma) before the quantiles are
    // expanded: thousands of regions' pops collapse into a few hundred groups per nation,
    // which keeps the sort (and the calibration's bisection) cheap; the Gini moves < 1e-4.
    std::unordered_map<uint64_t, std::array<double, 3>> bins; // people, income, sigma x people
    auto add_group = [&](double people, double income, double sigma) {
        if(people <= 0.0) {
            return;
        }
        const double mean = std::max(floor, income / people);
        const auto lb = static_cast<int64_t>(std::floor(std::log(mean) / 0.005));
        const auto sb = static_cast<int64_t>(std::floor(sigma / 0.005));
        auto& b = bins[(static_cast<uint64_t>(lb) << 20) ^ static_cast<uint64_t>(sb)];
        b[0] += people;
        b[1] += mean * people;
        b[2] += sigma * people;
    };
    auto expand = [&] {
        pts.reserve(bins.size() * quantiles);
        for(auto const& [key, b] : bins) {
            (void)key;
            const double mean = b[1] / b[0], sigma = b[2] / b[0];
            const double mu = std::log(mean) - 0.5 * sigma * sigma;
            for(int j = 0; j < quantiles; ++j) pts.push_back({std::exp(mu + sigma * z[static_cast<std::size_t>(j)]), b[0] / quantiles});
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
                // Disposable-income Ginis are of household income per person (LIS, WIID): the
                // inactive (children, carers, students) live in the households of the employed
                // and the unemployed and share their income, in proportion. As separate people
                // with almost no income they made the between-group gap alone exceed the
                // national Gini in 56 nations.
                const double labour = employed + unemployed;
                if(labour > 0.0) {
                    const double r = inactive / labour; // dependants per labour-force member
                    // About half of the unemployed live with an earner and share that income
                    // (Gregg & Wadsworth 2008 on jobless households; OECD); the rest live on benefits.
                    constexpr double with_earner = 0.5;
                    const double pooled = with_earner * unemployed;
                    add_group((employed + pooled) * (1.0 + r), p.get_inc_work() + with_earner * p.get_inc_benefit() + other_per_head * (employed + pooled) * (1.0 + r), sigma);
                    add_group((unemployed - pooled) * (1.0 + r), (1.0 - with_earner) * p.get_inc_benefit() + other_per_head * (unemployed - pooled) * (1.0 + r), sigma);
                } else {
                    add_group(people, p.get_inc_work() + p.get_inc_benefit() + p.get_inc_other(), sigma);
                }
            } else {
                add_group(p.get_size(), std::max(0.0f, p.get_income()), sigma);
            }
        }
    }
    expand();
    // Pareto top tail (Piketty & Saez 2003; WID): the top 1% hold the data share,
    // moving with the capital-income share of income. The lognormal body gets the rest.
    const float top0 = w.nation_get_top1_share0(n);
    if(top0 > 0.0f && !pts.empty()) {
        const float gdp_index = w.nation_get_gdp_real(n) / std::max(1.0f, w.nation_get_gdp0_daily(n) * w.nation_get_gf(n));
        const float profit_index = w.nation_get_profit0(n) > 0.0f ? w.nation_get_profit_smoothed(n) / (w.nation_get_profit0(n) * std::max(0.01f, w.nation_get_gf(n) * w.nation_get_core_price(n))) : 1.0f;
        const float cap_rel = gdp_index > 0.0f ? std::max(0.1f, profit_index / gdp_index) : 1.0f;
        const double share = std::clamp(static_cast<double>(top0) * std::pow(cap_rel, static_cast<float>(state.params.top_share_capital)), 0.02, 0.5);
        std::sort(pts.begin(), pts.end(), [](point const& a, point const& b) { return a.x < b.x; });
        double weight = 0.0, income = 0.0;
        for(auto const& pt : pts) {
            weight += pt.weight;
            income += pt.weight * pt.x;
        }
        // Split points at the 99th percentile (a straddling point is split in two).
        std::vector<point> body, tail;
        double cum = 0.0;
        for(auto const& pt : pts) {
            const double before = cum;
            cum += pt.weight;
            const double in_tail = std::clamp((cum - 0.99 * weight) / pt.weight, 0.0, 1.0);
            if(in_tail < 1.0) body.push_back({pt.x, pt.weight * (1.0 - in_tail)});
            if(in_tail > 0.0) tail.push_back({pt.x, pt.weight * in_tail});
            (void)before;
        }
        double body_inc = 0.0, tail_inc = 0.0;
        for(auto const& pt : body) body_inc += pt.weight * pt.x;
        for(auto const& pt : tail) tail_inc += pt.weight * pt.x;
        if(body_inc > 0.0 && tail_inc > 0.0) {
            for(auto& pt : body) pt.x *= (1.0 - share) * income / body_inc;
            for(auto& pt : tail) pt.x *= share * income / tail_inc;
            pts = body;
            pts.insert(pts.end(), tail.begin(), tail.end());
        }
    }
    return pts;
}

} // namespace

void update_indicators(game_state& state) {
    auto& w = state.world;
    // Distribution of this day's shard: one sorted set of income points per nation for
    // the Gini, Palma and poverty, nations in parallel (each writes only its own fields).
    std::vector<dcon::nation_id> due;
    for(auto n : w.in_nation) {
        if(state.current_tick % 28 == static_cast<int32_t>(n.id.index() % 28)) due.push_back(n.id);
    }
    concurrency::parallel_for(std::size_t(0), due.size(), [&](std::size_t i) {
        const auto n = due[i];
        const auto pts = sorted_points(state, n, -1.0f);
        float palma = 0.0f, poverty = 0.0f;
        distribution_of(pts, palma, poverty);
        w.nation_set_gini(n, gini_of(pts));
        w.nation_set_palma(n, palma);
        w.nation_set_poverty(n, poverty);
        if(w.nation_get_poverty0(n) <= 0.0f) w.nation_set_poverty0(n, poverty);
    }, concurrency::static_partitioner());
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
    }
}

} // namespace geosim::sim
