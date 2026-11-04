#include "sim_runner.hpp"

#include "core/keys.hpp"
#include "sim/model.hpp"
#include "sim/report.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace geosim::client {

namespace {

using clock = std::chrono::steady_clock;

// ISO3 -> ISO2 for the emoji flag (regional indicator pair). Covers the
// shipped scenarios; unknown codes fall back to a globe.
struct iso_pair {
    char const* iso3;
    char const* iso2;
};
constexpr iso_pair iso_table[] = {
    {"USA", "US"}, {"CHN", "CN"}, {"JPN", "JP"}, {"DEU", "DE"}, {"GBR", "GB"}, {"FRA", "FR"}, {"ITA", "IT"},
    {"ESP", "ES"}, {"CAN", "CA"}, {"KOR", "KR"}, {"AUS", "AU"}, {"BRA", "BR"}, {"MEX", "MX"}, {"IND", "IN"},
    {"RUS", "RU"}, {"TUR", "TR"}, {"IDN", "ID"}, {"SAU", "SA"}, {"ZAF", "ZA"}, {"ARG", "AR"}, {"NLD", "NL"},
    {"CHE", "CH"}, {"SWE", "SE"}, {"POL", "PL"}, {"BEL", "BE"}, {"NOR", "NO"}, {"AUT", "AT"}, {"IRN", "IR"},
    {"ISR", "IL"}, {"EGY", "EG"}, {"NGA", "NG"}, {"PAK", "PK"}, {"UKR", "UA"}, {"GRC", "GR"}, {"PRT", "PT"},
};

std::string flag_for(std::string const& iso3) {
    for(auto const& p : iso_table) {
        if(iso3 == p.iso3) {
            std::string out;
            for(int k = 0; k < 2; ++k) {
                const uint32_t cp = 0x1F1E6u + static_cast<uint32_t>(p.iso2[k] - 'A');
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            return out;
        }
    }
    return "\xF0\x9F\x8C\x8D"; // U+1F30D globe
}

std::string fmt(char const* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

bool is_alert_code(uint8_t c) {
    using namespace sim;
    switch(c) {
    case journal_election: case journal_coup: case journal_war: case journal_peace: case journal_sanction:
    case journal_default: case journal_chokepoint: case journal_cyber: case journal_insurgency: case journal_no_confidence:
    case journal_court: case journal_regime_change: case journal_disaster: case journal_dispute: case journal_concession:
    case journal_variant: case journal_term_limit: case journal_agreement: case journal_rebels: case journal_alliance_call:
        return true;
    default:
        return false;
    }
}

} // namespace

sim_runner::sim_runner() = default;
sim_runner::~sim_runner() { stop(); }

void sim_runner::start(std::string assets_dir, std::string scenario, std::string nation_iso3) {
    assets_ = std::move(assets_dir);
    scenario_ = std::move(scenario);
    iso3_ = std::move(nation_iso3);
    flag_ = flag_for(iso3_);
    thread_ = std::jthread([this] { run(); });
}

void sim_runner::stop() {
    if(!thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        quit_.store(true);
    }
    wake_.notify_all();
    thread_.join();
}

void sim_runner::set_speed(int s) noexcept {
    {
        std::lock_guard lock(mutex_);
        speed_.store(std::clamp(s, 0, max_speed));
    }
    wake_.notify_all();
}

core::civil_date sim_runner::date() const noexcept { return core::date_from_tick(tick(), epoch_days_); }

void sim_runner::post(std::function<void(game_state&)> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    wake_.notify_all();
}

std::future<journal_batch> sim_runner::request_journal(int32_t since_tick) {
    auto promise = std::make_shared<std::promise<journal_batch>>();
    auto future = promise->get_future();
    post([promise, since_tick](game_state& s) {
        journal_batch batch;
        batch.scanned_to = s.current_tick;
        auto const& w = s.world;
        for(auto d : w.in_decision) {
            const uint8_t code = d.get_lever();
            if(!is_alert_code(code) || d.get_tick() < since_tick) continue;
            journal_entry e;
            e.tick = d.get_tick();
            e.code = code;
            if(d.get_target()) e.nation = core::unpack_key(d.get_target().get_iso3());
            const float p = d.get_pragmatic();
            const bool has_partner = code == sim::journal_war || code == sim::journal_peace || code == sim::journal_dispute
                                     || code == sim::journal_alliance_call || code == sim::journal_concession
                                     || code == sim::journal_cyber || code == sim::journal_agreement;
            if(has_partner && std::isfinite(p) && p >= 0.0f && p < static_cast<float>(w.nation_size())) {
                e.partner = core::unpack_key(w.nation_get_iso3(dcon::nation_id{static_cast<dcon::nation_id::value_base_t>(p)}));
            }
            e.before = d.get_before();
            e.after = d.get_after();
            batch.entries.push_back(std::move(e));
        }
        promise->set_value(std::move(batch));
    });
    return future;
}

void sim_runner::run() {
    auto state = std::make_unique<game_state>();
    const auto report = sim::build_scenario(*state, assets_, scenario_);
    if(!report.ok()) {
        error_ = "scenario " + scenario_ + ": " + report.error;
        SDL_Log("%s", error_.c_str());
        status_.store(sim_status::failed, std::memory_order_release);
        return;
    }
    SDL_Log("scenario %s: %zu events, %zu warnings", scenario_.c_str(), report.events, report.warnings.size());
    sim::begin_measurement(*state);
    const auto n = sim::find_nation(*state, iso3_);
    if(n) {
        nation_ = n.index();
        has_nation_ = true;
        unemployment0_ = state->world.nation_get_unemployment(n);
        yield0_ = state->world.nation_get_yield10(n);
    } else {
        SDL_Log("nation %s not in scenario %s; indicators disabled", iso3_.c_str(), scenario_.c_str());
    }
    epoch_days_ = state->epoch_days;
    tick_.store(state->current_tick);
    state_ = std::move(state);
    status_.store(sim_status::running, std::memory_order_release);

    game_state& s = *state_;
    auto last_day = clock::now();
    auto rate_start = clock::now();
    int rate_days = 0;
    std::vector<std::function<void(game_state&)>> jobs;
    while(true) {
        // Sync point: between two days nothing is being written.
        {
            std::unique_lock lock(mutex_);
            if(quit_) break;
            jobs.swap(jobs_);
        }
        for(auto& job : jobs) job(s);
        jobs.clear();

        const int sp = speed_.load();
        const auto now = clock::now();
        if(sp == 0) {
            measured_rate_.store(0.0);
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || !jobs_.empty() || speed_.load() != 0; });
            last_day = clock::now();
            rate_start = last_day;
            rate_days = 0;
            continue;
        }
        if(sp < max_speed) {
            const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / days_per_second[sp]));
            const auto due = last_day + period;
            if(now < due) {
                std::unique_lock lock(mutex_);
                wake_.wait_until(lock, due, [&] { return quit_ || !jobs_.empty() || speed_.load() != sp; });
                continue;
            }
            // No catch-up bursts after a stall: at most one period behind.
            last_day = std::max(due, now - period);
        }
        sim::advance_day(s);
        tick_.store(s.current_tick, std::memory_order_relaxed);
        ++rate_days;
        const double elapsed = std::chrono::duration<double>(clock::now() - rate_start).count();
        if(elapsed >= 1.0) {
            measured_rate_.store(rate_days / elapsed);
            rate_start = clock::now();
            rate_days = 0;
        }
    }
    // Fail outstanding requests' promises cleanly by running them once more.
    for(auto& job : jobs_) job(s);
    jobs_.clear();
}

std::vector<agent_panel> sim_runner::read_agents() {
    std::vector<agent_panel> out;
    if(status() != sim_status::running || !has_nation_) return out;
    auto const& w = state_->world;
    const dcon::nation_id n{static_cast<dcon::nation_id::value_base_t>(nation_)};
    const int32_t t = tick();

    // Trailing-year means from the sim's running accumulators (acc_*), sampled
    // whenever the UI sees a new tick (days may be skipped at high speed).
    const double acc_gdp = w.nation_get_acc_gdp_real(n), acc_cpi = w.nation_get_acc_cpi(n), acc_days = w.nation_get_acc_days(n);
    if(history_.empty() || history_.back().tick != t) {
        if(!history_.empty() && t < history_.back().tick) history_.clear(); // reloaded / rewound
        history_.push_back({t, acc_gdp, acc_cpi, acc_days});
        const auto old = std::find_if(history_.begin(), history_.end(), [&](sample const& s) { return s.tick >= t - 800; });
        history_.erase(history_.begin(), old);
    }
    auto at = [&](int32_t target) -> sample const* { // latest sample with tick <= target
        sample const* best = nullptr;
        for(auto const& s : history_) {
            if(s.tick <= target) best = &s;
        }
        return best;
    };
    auto mean_between = [](sample const& a, sample const& b, bool gdp) {
        const double days = b.acc_days - a.acc_days;
        if(days <= 0.5) return std::nan("");
        return ((gdp ? b.acc_gdp - a.acc_gdp : b.acc_cpi - a.acc_cpi)) / days;
    };

    const double gdp0_daily = w.nation_get_gdp0(n) / 365.0;
    const double cpi_2019 = std::pow(1.0 + w.nation_get_inflation_trend(n), -0.5);
    double growth = std::nan(""), inflation = std::nan("");
    const double days = std::max(1.0, acc_days);
    if(acc_days < 365.0 || history_.size() < 2) {
        // First year: the run-to-date mean vs 2019, as sim::summarize computes it.
        growth = (acc_gdp / days / gdp0_daily - 1.0) * 100.0;
        inflation = (acc_cpi / days / cpi_2019 - 1.0) * 100.0;
    } else {
        sample const& now = history_.back();
        sample const* y1 = at(t - 365);
        sample const* y2 = at(t - 730);
        if(y1) {
            const double g1 = mean_between(*y1, now, true), c1 = mean_between(*y1, now, false);
            const double g0 = y2 ? mean_between(*y2, *y1, true) : gdp0_daily;
            const double c0 = y2 ? mean_between(*y2, *y1, false) : cpi_2019;
            growth = (g1 / g0 - 1.0) * 100.0;
            inflation = (c1 / c0 - 1.0) * 100.0;
        }
    }
    const double unemployment = w.nation_get_unemployment(n) * 100.0;
    const double trend_infl = w.nation_get_inflation_trend(n) * 100.0;
    // Incumbent party's vote intention (nation.approval is this relative to its 2019 share).
    const auto incumbent = w.nation_get_incumbent(n);
    const double support = incumbent ? w.party_get_vote_share(incumbent) * 100.0 : std::nan("");
    const double support0 = w.nation_get_approval0(n) * 100.0;
    const double nominal = w.nation_get_acc_gdp_nominal(n);
    // Run-to-date ratio: too noisy to show during the first month.
    const double balance = nominal > 0 && acc_days >= 30.0 ? (w.nation_get_bal_gov(n) - w.nation_get_acc_bal_gov_start(n)) / nominal * 100.0
                                                           : std::nan("");

    auto valued = [](double v, char const* f) { return std::isfinite(v) ? fmt(f, v) : std::string("-"); };
    auto tone_of = [](bool good, bool bad) { return bad ? tone::bad : (good ? tone::good : tone::neutral); };

    agent_panel gov;
    gov.icon = "\xF0\x9F\x8F\x9B"; // U+1F3DB classical building
    gov.title = "Government";
    gov.items.push_back({"Support", valued(support, "%.1f%%"), tone_of(support > support0 + 1.0, support < support0 - 1.0),
                         "Incumbent's vote share now (start of the run: " + fmt("%.1f%%", support0) + ")"});
    gov.items.push_back({"GDP", valued(growth, "%+.1f%%"), tone_of(growth > 1.0, growth < 0.0),
                         "Real GDP growth: first year vs 2019, then trailing 12 months vs the year before"});
    gov.items.push_back({"Unemp.", valued(unemployment, "%.1f%%"),
                         tone_of(unemployment < unemployment0_ * 100.0 - 0.3, unemployment > unemployment0_ * 100.0 + 0.3),
                         "Unemployment rate (start: " + fmt("%.1f%%", unemployment0_ * 100.0) + ")"});
    gov.items.push_back({"Budget", valued(balance, "%+.1f%%"), tone_of(balance > -1.0, balance < -4.0),
                         "Change in the government balance since the start, % of GDP over the same period"});
    if(w.nation_get_competitive(n)) {
        const int32_t next = w.nation_get_next_election(n);
        if(next > t) {
            gov.items.push_back({"Election", fmt("%.0f d", static_cast<double>(next - t)), tone::neutral, "Days to the next scheduled election"});
        }
    }
    out.push_back(std::move(gov));

    agent_panel cb;
    cb.icon = "\xF0\x9F\x8F\xA6"; // U+1F3E6 bank
    cb.title = "Central bank";
    const double gap = inflation - trend_infl;
    cb.items.push_back({"Inflation", valued(inflation, "%.1f%%"), tone_of(std::fabs(gap) < 1.0, std::fabs(gap) > 2.5),
                        "CPI inflation (same window as GDP); trend " + fmt("%.1f%%", trend_infl)});
    cb.items.push_back({"Rate", valued(w.nation_get_policy_rate(n), "%.2f%%"), tone::neutral, "Policy interest rate"});
    const double y10 = w.nation_get_yield10(n);
    cb.items.push_back({"10y", valued(y10, "%.2f%%"), tone_of(y10 < yield0_ - 0.25, y10 > yield0_ + 0.5),
                        "10-year government bond yield (start: " + fmt("%.2f%%", yield0_) + ")"});
    const double fx = (w.nation_get_exchange_rate(n) - 1.0) * 100.0;
    cb.items.push_back({"FX", valued(fx, "%+.1f%%"), tone::neutral, "Real exchange rate vs the start"});
    out.push_back(std::move(cb));
    return out;
}

} // namespace geosim::client
