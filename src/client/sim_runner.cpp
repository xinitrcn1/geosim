#include "sim_runner.hpp"

#include "flags.hpp"

#include "core/csv.hpp"
#include "core/diagnostics.hpp"
#include "core/keys.hpp"
#include "sim/commands.hpp"
#include "sim/model.hpp"
#include "sim/report.hpp"
#include "sim/save.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/web.hpp"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_map>

namespace geosim::client {

namespace {

using clock = std::chrono::steady_clock;


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
    case journal_request:
        return true;
    default:
        return false;
    }
}

} // namespace

// Every controllable agent with its nation and a label. Executives and central
// banks come from their seats, unions / militaries / rebels from their nation,
// governors from their region, boards from their firm (nation + sector).
std::vector<agent_info> collect_agents(game_state const& s, std::vector<std::string> const& nation_names,
                                       std::vector<std::string> const& region_names, std::vector<std::string> const& state_names,
                                       sim::web::names const* web_names) {
    using sim::agent_kind;
    auto const& w = s.world;
    std::vector<agent_info> out;
    auto name_of = [&](uint32_t n) { return n < nation_names.size() ? nation_names[n] : std::string{}; };
    auto add = [&](dcon::agent_id a, agent_kind k, uint32_t n, std::string label) {
        if(!a) return;
        agent_info i;
        i.agent = static_cast<uint32_t>(a.index());
        i.kind = static_cast<uint8_t>(k);
        i.nation = n;
        if(n < w.nation_size()) i.iso3 = core::unpack_key(w.nation_get_iso3(dcon::nation_id{static_cast<dcon::nation_id::value_base_t>(n)}));
        i.label = std::move(label);
        out.push_back(std::move(i));
    };
    std::vector<uint8_t> bank_seen(w.agent_size(), 0);
    for(auto n : w.in_nation) {
        const auto ni = static_cast<uint32_t>(n.id.index());
        const std::string name = name_of(ni);
        if(auto seat = n.get_executive_seat()) add(seat.get_agent().id, agent_kind::executive, ni, name + " Government");
        if(auto ma = n.get_monetary_authority()) {
            const auto bank = ma.get_bank();
            if(bank && !bank_seen[bank.id.index()]) {
                bank_seen[bank.id.index()] = 1;
                int members = 0;
                for(auto m : bank.get_monetary_authority()) {
                    (void)m;
                    ++members;
                }
                add(bank.id, agent_kind::central_bank, ni,
                    members > 1 ? "Central Bank of " + name + " and " + std::to_string(members - 1) + " more" : name + " Central Bank");
            }
        }
        add(n.get_union_agent().id, agent_kind::union_federation, ni, name + " Unions");
        add(n.get_military_agent().id, agent_kind::military, ni, name + " Military");
        add(n.get_rebel_agent().id, agent_kind::rebel, ni, name + " Rebels");
    }
    // Governors: one per state (its clusters share it), named after the state.
    std::vector<uint8_t> seen_gov(w.agent_size(), 0);
    for(auto r : w.in_region) {
        if(!r.get_governor()) continue;
        const auto g = static_cast<std::size_t>(r.get_governor().id.index());
        if(g < seen_gov.size() && seen_gov[g]) continue;
        if(g < seen_gov.size()) seen_gov[g] = 1;
        const auto owner = r.get_owner();
        const auto ni = owner ? static_cast<uint32_t>(owner.id.index()) : 0xFFFFFFFFu;
        const auto st = r.get_state();
        std::string place = st && static_cast<std::size_t>(st.id.index()) < state_names.size() ? state_names[static_cast<std::size_t>(st.id.index())] : std::string{};
        if(place.empty()) place = static_cast<std::size_t>(r.id.index()) < region_names.size() ? region_names[static_cast<std::size_t>(r.id.index())] : std::string{};
        add(r.get_governor().id, agent_kind::governor, ni, (place.empty() ? name_of(ni) + " region" : place) + " Governor");
        out.back().region = static_cast<uint32_t>(r.id.index());
    }
    for(auto f : w.in_firm) {
        if(!f.get_board()) continue;
        const auto ni = f.get_nation() ? static_cast<uint32_t>(f.get_nation().id.index()) : 0xFFFFFFFFu;
        add(f.get_board().id, agent_kind::board, ni, web_names ? sim::web::firm_name(s, *web_names, f.id) + " board" : name_of(ni) + " board");
        out.back().firm = static_cast<uint32_t>(f.id.index());
    }
    return out;
}

// The player -> sim command stream and its results (sim/commands.hpp).
struct sim_runner::command_io {
    sim::command_queue in;                  // UI thread pushes, sim thread drains
    sim::result_queue out;                  // sim thread pushes, UI thread pops
    std::deque<sim::command> backlog;       // UI thread: not yet accepted by a full queue
    std::unordered_map<uint32_t, action_outcome> results; // UI thread: action results by seq
};

sim_runner::sim_runner() : io_(std::make_unique<command_io>()) {}

uint32_t sim_runner::push_command(sim::command c) {
    c.seq = ++seq_;
    c.tick = tick(); // applies before the next tick (lockstep will add an input delay)
    if(!io_->backlog.empty() || !io_->in.push(c)) io_->backlog.push_back(c);
    {
        std::lock_guard lock(mutex_); // wakes a paused sim without a lost notification
    }
    wake_.notify_all();
    return c.seq;
}

void sim_runner::pump() {
    bool pushed = false;
    while(!io_->backlog.empty() && io_->in.push(io_->backlog.front())) {
        io_->backlog.pop_front();
        pushed = true;
    }
    if(pushed) {
        {
            std::lock_guard lock(mutex_);
        }
        wake_.notify_all();
    }
    while(auto r = io_->out.pop()) {
        if(r->tag == sim::command_tag::action) {
            io_->results[r->seq] = {r->accepted != 0, std::string(r->message, strnlen(r->message, sizeof(r->message)))};
        } else if(!r->accepted) {
            SDL_Log("command %u rejected: %s", r->seq, r->message);
        }
    }
}

bool sim_runner::take_result(uint32_t seq, action_outcome& out) {
    auto it = io_->results.find(seq);
    if(it == io_->results.end()) return false;
    out = std::move(it->second);
    io_->results.erase(it);
    return true;
}

uint32_t sim_runner::set_override(uint32_t agent, uint8_t lever, uint8_t mode, float value) {
    return push_command(sim::make_set_override(dcon::agent_id{static_cast<dcon::agent_id::value_base_t>(agent)}, lever,
                                               static_cast<sim::override_mode>(mode), value, 0, 0));
}

uint32_t sim_runner::perform(action_order o) {
    sim::action_request q;
    q.kind = static_cast<sim::action_kind>(o.kind);
    q.actor = dcon::agent_id{static_cast<dcon::agent_id::value_base_t>(o.agent)};
    if(o.nation != 0xFFFFFFFFu) q.target = dcon::nation_id{static_cast<dcon::nation_id::value_base_t>(o.nation)};
    if(o.organisation != 0xFFFFFFFFu) q.organisation = dcon::organisation_id{static_cast<dcon::organisation_id::value_base_t>(o.organisation)};
    if(o.target_agent != 0xFFFFFFFFu) q.target_agent = dcon::agent_id{static_cast<dcon::agent_id::value_base_t>(o.target_agent)};
    if(o.region != 0xFFFFFFFFu) q.region = dcon::region_id{static_cast<dcon::region_id::value_base_t>(o.region)};
    if(o.state_to != 0xFFFFFFFFu) q.state_to = dcon::state_id{static_cast<dcon::state_id::value_base_t>(o.state_to)};
    q.treaty = o.treaty < static_cast<uint8_t>(sim::treaty_kind::count) ? static_cast<sim::treaty_kind>(o.treaty) : sim::treaty_kind::count;
    q.lever = o.lever;
    q.value = o.value;
    return push_command(sim::make_action(q, 0, 0));
}

std::future<redraw_view> sim_runner::request_redraw(uint32_t region, uint32_t agent) {
    auto promise = std::make_shared<std::promise<redraw_view>>();
    auto future = promise->get_future();
    post([this, promise, region, agent](game_state& s) {
        redraw_view out;
        out.region = region;
        auto const& w = s.world;
        const auto r = region < w.region_size() ? dcon::region_id{static_cast<dcon::region_id::value_base_t>(region)} : dcon::region_id{};
        const auto a = agent < w.agent_size() ? dcon::agent_id{static_cast<dcon::agent_id::value_base_t>(agent)} : dcon::agent_id{};
        if(!r || !a) {
            promise->set_value(std::move(out));
            return;
        }
        auto name_of = [&](dcon::state_id st) {
            if(!st) return std::string("a new state");
            const auto i = static_cast<std::size_t>(st.index());
            if(i >= state_names_.size()) state_names_.resize(i + 1);
            if(state_names_[i].empty()) state_names_[i] = "New state " + std::to_string(i);
            return state_names_[i];
        };
        out.state = name_of(w.region_get_state(r));
        for(auto st : sim::redraw_targets(s, r)) {
            const auto v = sim::check_redraw(s, a, r, st);
            out.options.push_back({static_cast<uint32_t>(st.index()), "Move to " + name_of(st), v.ok, v.reason});
        }
        const auto v = sim::check_redraw(s, a, r, dcon::state_id{});
        out.options.push_back({0xFFFFFFFFu, "Make it a new state", v.ok, v.reason});
        promise->set_value(std::move(out));
    });
    return future;
}

std::future<std::vector<agent_info>> sim_runner::request_agents() {
    auto promise = std::make_shared<std::promise<std::vector<agent_info>>>();
    auto future = promise->get_future();
    post([this, promise](game_state& s) { promise->set_value(collect_agents(s, nation_names_, region_names_, state_names_, &web_names_ref())); });
    return future;
}

sim_runner::controlled& sim_runner::track(uint32_t nation) {
    for(auto& c : controlled_) {
        if(c.index == nation) return c;
    }
    controlled c;
    c.index = nation;
    auto const& w = state_->world;
    const dcon::nation_id n{static_cast<dcon::nation_id::value_base_t>(nation)};
    c.iso3 = core::unpack_key(w.nation_get_iso3(n));
    // The nation's flag (flags.csv), else a white flag emoji.
    c.flag = flags_ ? flags_->token(c.iso3) : std::string{};
    if(c.flag.empty()) c.flag = "\xF0\x9F\x8F\xB3"; // U+1F3F3 white flag
    c.name = nation < nation_names_.size() && !nation_names_[nation].empty() ? nation_names_[nation] : c.iso3;
    c.unemployment0 = w.nation_get_unemployment(n);
    c.yield0 = w.nation_get_yield10(n);
    controlled_.push_back(std::move(c));
    return controlled_.back();
}

void sim_runner::refresh_nations() {
    iso3s_.clear();
    for(auto const& a : agents_) {
        if(a.nation >= state_->world.nation_size()) continue;
        track(a.nation);
        if(std::find(iso3s_.begin(), iso3s_.end(), a.iso3) == iso3s_.end()) iso3s_.push_back(a.iso3);
    }
    // Forget nations without a controlled agent (player_at_war iterates controlled_).
    controlled_.erase(std::remove_if(controlled_.begin(), controlled_.end(),
                                     [&](controlled const& c) {
                                         return std::none_of(agents_.begin(), agents_.end(), [&](agent_info const& a) { return a.nation == c.index; });
                                     }),
                      controlled_.end());
}

bool sim_runner::controls_agent(uint32_t agent) const noexcept {
    return std::any_of(agents_.begin(), agents_.end(), [&](agent_info const& a) { return a.agent == agent; });
}

void sim_runner::add_control(agent_info const& a) {
    if(status() != sim_status::running || controls_agent(a.agent)) return;
    agents_.push_back(a);
    ai_.push_back(1);
    answer_.push_back(1);
    refresh_nations();
    // The sim learns the agent is the player's: requests to it wait for an answer.
    push_command(sim::make_set_control(dcon::agent_id{static_cast<dcon::agent_id::value_base_t>(a.agent)}, true, true, 0, 0));
    SDL_Log("control: %s", a.label.c_str());
}

void sim_runner::remove_control(uint32_t agent) {
    for(std::size_t i = 0; i < agents_.size(); ++i) {
        if(agents_[i].agent != agent) continue;
        if(!ai_[i]) set_ai(agent, true); // hand the levers back to the AI
        // pending requests to it are answered by its AI at the next tick
        push_command(sim::make_set_control(dcon::agent_id{static_cast<dcon::agent_id::value_base_t>(agent)}, false, false, 0, 0));
        agents_.erase(agents_.begin() + static_cast<std::ptrdiff_t>(i));
        ai_.erase(ai_.begin() + static_cast<std::ptrdiff_t>(i));
        answer_.erase(answer_.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
    refresh_nations();
}

void sim_runner::set_ai(uint32_t agent, bool on) {
    for(std::size_t i = 0; i < agents_.size(); ++i) {
        if(agents_[i].agent == agent) ai_[i] = on ? 1 : 0;
    }
    // The sim applies it: AI off directs every lever at its current choice, AI on clears
    // the overrides (sim/commands.cpp).
    push_command(sim::make_set_ai(dcon::agent_id{static_cast<dcon::agent_id::value_base_t>(agent)}, on, 0, 0));
    SDL_Log("agent %u: AI %s", agent, on ? "on" : "off (full control)");
}

void sim_runner::set_answer_requests(uint32_t agent, bool on) {
    for(std::size_t i = 0; i < agents_.size(); ++i) {
        if(agents_[i].agent == agent) answer_[i] = on ? 1 : 0;
    }
    push_command(sim::make_set_control(dcon::agent_id{static_cast<dcon::agent_id::value_base_t>(agent)}, true, on, 0, 0));
    SDL_Log("agent %u: answer requests %s", agent, on ? "on (they wait for you)" : "off (its AI answers at once)");
}

uint32_t sim_runner::answer(uint32_t request, uint32_t agent, bool accept) {
    return push_command(sim::make_answer(dcon::request_id{static_cast<dcon::request_id::value_base_t>(request)},
                                         dcon::agent_id{static_cast<dcon::agent_id::value_base_t>(agent)}, accept, 0, 0));
}

std::future<web_view> sim_runner::request_web(std::string url) {
    auto promise = std::make_shared<std::promise<web_view>>();
    auto future = promise->get_future();
    std::vector<uint32_t> nations;
    for(auto const& a : agents_) nations.push_back(a.nation);
    post([this, promise, url = std::move(url), nations = std::move(nations)](game_state& s) {
        if(!people_) {
            people_ = std::make_unique<sim::web::name_pools>();
            std::string error;
            if(!sim::web::load_name_pools(assets_ + "/defs", *people_, error)) SDL_Log("names: %s (the internet's people stay anonymous)", error.c_str());
        }
        if(!domains_) {
            domains_ = std::make_unique<sim::web::domain_table>();
            std::string error;
            if(!sim::web::load_domain_table(assets_ + "/defs", *domains_, error)) SDL_Log("tlds: %s (sites fall back to com.<iso3>)", error.c_str());
        }
        if(!text_) {
            text_ = std::make_unique<sim::web::web_text>();
            std::string error;
            if(!sim::web::load_web_text(assets_ + "/defs", *text_, error)) SDL_Log("web text: %s (built-in fallbacks)", error.c_str());
        }
        sim::web::names nm{&nation_names_, &region_names_, people_.get(), domains_.get(), text_.get()};
        const auto dir = sim::web::build_directory(s, nm);
        std::vector<dcon::nation_id> mine;
        for(auto i : nations) {
            if(i < s.world.nation_size()) mine.push_back(dcon::nation_id{static_cast<dcon::nation_id::value_base_t>(i)});
        }
        web_view v;
        v.engine = sim::web::default_engine(s, dir, mine);
        if(auto const* e = dir.find(v.engine)) v.engine_name = e->name;
        const auto pg = sim::web::render(s, nm, dir, url, v.engine, mine);
        v.url = pg.url;
        v.title = pg.title;
        v.host = pg.host;
        v.category = pg.category;
        v.owner = pg.owner;
        v.accent = pg.accent;
        v.style = static_cast<uint8_t>(pg.style);
        v.ads_blocked = pg.ads_blocked;
        for(auto const& b : pg.blocks) {
            v.blocks.push_back({static_cast<uint8_t>(b.k), b.text, b.sub, b.url, b.tone, b.author,
                                flags_ && !b.iso3.empty() ? flags_->token(b.iso3) : std::string{}});
        }
        for(std::size_t i = 0; i < dir.sites.size() && v.bookmarks.size() < 5; ++i) v.bookmarks.push_back({dir.sites[i].name, dir.sites[i].domain});
        promise->set_value(std::move(v));
    });
    return future;
}

std::future<std::vector<pending_request>> sim_runner::request_pending() {
    auto promise = std::make_shared<std::promise<std::vector<pending_request>>>();
    auto future = promise->get_future();
    post([promise](game_state& s) {
        std::vector<pending_request> out;
        for(auto r : s.world.in_request) {
            if(!r.get_to() || !r.get_to().get_player_controlled()) continue;
            pending_request p;
            p.request = static_cast<uint32_t>(r.id.index());
            p.to = static_cast<uint32_t>(r.get_to().id.index());
            p.kind = r.get_kind();
            const auto text = sim::describe_request(s, r.id);
            p.title = text.title;
            p.detail = text.detail;
            p.created = r.get_created();
            p.expires = r.get_expires();
            p.ai_accepts = sim::request_ai_answer(s, r.id);
            out.push_back(std::move(p));
        }
        // the soonest to expire first
        std::stable_sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.expires < b.expires; });
        promise->set_value(std::move(out));
    });
    return future;
}

std::future<std::string> sim_runner::request_save(std::string path) {
    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();
    post([promise, path = std::move(path)](game_state& s) {
        core::diagnostics diag;
        promise->set_value(sim::save_game(s, path, diag) ? std::string{} : diag.error());
    });
    return future;
}
sim_runner::~sim_runner() { stop(); }

void sim_runner::start(std::string assets_dir, std::string scenario, std::string nations, std::string save_path) {
    if(thread_.joinable()) return; // one session per sim_runner
    assets_ = std::move(assets_dir);
    scenario_ = std::move(scenario);
    save_path_ = std::move(save_path);
    iso3s_.clear();
    std::size_t at = 0;
    while(at <= nations.size()) {
        const std::size_t comma = std::min(nations.find(',', at), nations.size());
        std::string iso = nations.substr(at, comma - at);
        if(!iso.empty() && std::find(iso3s_.begin(), iso3s_.end(), iso) == iso3s_.end()) iso3s_.push_back(std::move(iso));
        at = comma + 1;
    }
    sim::set_thread_limit(2); // the sim's parallel systems share two cores with the renderer
    thread_ = std::jthread([this] { run(); });
}

bool sim_runner::controls(std::string const& iso3) const noexcept { return std::find(iso3s_.begin(), iso3s_.end(), iso3) != iso3s_.end(); }

bool sim_runner::player_at_war() const noexcept {
    if(status() != sim_status::running) return false;
    auto const& w = state_->world;
    const auto count = static_cast<uint16_t>(w.nation_size());
    for(controlled const& c : controlled_) {
        const dcon::nation_id n{static_cast<dcon::nation_id::value_base_t>(c.index)};
        for(uint16_t k = 0; k < count; ++k) {
            if(k != c.index && w.nation_get_at_war(n, k) > 0.0f) return true;
        }
    }
    return false;
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
                                     || code == sim::journal_cyber || code == sim::journal_agreement || code == sim::journal_request;
            if(has_partner && std::isfinite(p) && p >= 0.0f && p < static_cast<float>(w.nation_size())) {
                e.partner = core::unpack_key(w.nation_get_iso3(dcon::nation_id{static_cast<dcon::nation_id::value_base_t>(p)}));
            }
            e.before = d.get_before();
            e.after = d.get_after();
            e.extra = d.get_prior();
            batch.entries.push_back(std::move(e));
        }
        promise->set_value(std::move(batch));
    });
    return future;
}

std::future<sim_map_world> sim_runner::request_map_world() {
    auto promise = std::make_shared<std::promise<sim_map_world>>();
    auto future = promise->get_future();
    post([promise](game_state& s) {
        sim_map_world out;
        auto const& w = s.world;
        for(auto n : w.in_nation) out.nation_iso3.push_back(core::unpack_key(n.get_iso3()));
        for(auto r : w.in_region) {
            out.lat.push_back(r.get_lat());
            out.lon.push_back(r.get_lon());
            const auto owner = r.get_owner();
            out.owner.push_back(owner ? static_cast<uint16_t>(owner.id.index()) : uint16_t{0xFFFF});
        }
        promise->set_value(std::move(out));
    });
    return future;
}

void sim_runner::read_region_owners(std::vector<uint16_t>& out) const noexcept {
    if(status() != sim_status::running) return;
    auto const& w = state_->world;
    const auto count = std::min<std::size_t>(out.size(), w.region_size());
    for(std::size_t i = 0; i < count; ++i) {
        const auto owner = w.region_get_owner(dcon::region_id{static_cast<dcon::region_id::value_base_t>(i)});
        out[i] = owner ? static_cast<uint16_t>(owner.id.index()) : uint16_t{0xFFFF};
    }
}

std::string sim_runner::place_name(uint32_t nation_index, uint32_t region_index) const {
    const std::string nation = nation_index < nation_names_.size() ? nation_names_[nation_index] : std::string{};
    const std::string region = region_index < region_names_.size() ? region_names_[region_index] : std::string{};
    if(region.empty() || region == nation) return nation;
    return nation.empty() ? region : region + ", " + nation;
}

void sim_runner::run() {
    auto state = std::make_unique<game_state>();
    if(!save_path_.empty()) {
        core::diagnostics load_diag;
        if(!sim::load_game(*state, save_path_, load_diag)) {
            error_ = "save " + save_path_ + ": " + load_diag.error();
            SDL_Log("%s", error_.c_str());
            status_.store(sim_status::failed, std::memory_order_release);
            return;
        }
        SDL_Log("loaded %s (scenario %s, day %d)", save_path_.c_str(), scenario_.c_str(), state->current_tick);
    } else {
        const auto report = sim::build_scenario(*state, assets_, scenario_);
        if(!report.ok()) {
            error_ = "scenario " + scenario_ + ": " + report.error;
            SDL_Log("%s", error_.c_str());
            status_.store(sim_status::failed, std::memory_order_release);
            return;
        }
        SDL_Log("scenario %s: %zu events, %zu warnings", scenario_.c_str(), report.events, report.warnings.size());
        sim::begin_measurement(*state);
    }
    // Display names from the scenario's nations.csv (truncated at a comma), then assets/defs/nation_names.csv.
    core::diagnostics names_diag;
    const auto names = core::csv_table::load(assets_ + "/scenario/" + scenario_ + "/nations.csv", names_diag);
    const auto iso_col = names.column("iso3"), name_col = names.column("name");

    // Names for notifications: every nation, and each region matched to its regions.csv
    // row by owner and nearest coordinates (the sim keeps no region names).
    {
        auto const& w = state->world;
        nation_names_.assign(w.nation_size(), std::string{});
        std::map<std::string, std::size_t> nation_of_iso;
        for(auto n : w.in_nation) {
            const std::string iso = core::unpack_key(n.get_iso3());
            nation_of_iso[iso] = n.id.index();
            nation_names_[n.id.index()] = iso;
        }
        nation_flags_.assign(w.nation_size(), std::string{});
        if(flags_) {
            for(auto n : w.in_nation) nation_flags_[n.id.index()] = flags_->token(core::unpack_key(n.get_iso3()));
        }
        for(std::size_t r = 0; iso_col && name_col && r < names.rows(); ++r) {
            auto it = nation_of_iso.find(std::string(names.get(r, *iso_col)));
            if(it == nation_of_iso.end()) continue;
            std::string full(names.get(r, *name_col));
            const std::size_t comma = full.find(',');
            if(comma != std::string::npos && comma > 0) full.resize(comma);
            if(!full.empty()) nation_names_[it->second] = full;
        }
        // Common English names (assets/defs/nation_names.csv) over the World Bank's formal ones.
        {
            core::diagnostics cdiag;
            const std::string cpath = assets_ + "/defs/nation_names.csv";
            if(std::FILE* probe = std::fopen(cpath.c_str(), "rb")) {
                std::fclose(probe);
                const auto ct = core::csv_table::load(cpath, cdiag);
                for(std::size_t r = 0; r < ct.rows(); ++r) {
                    auto it = nation_of_iso.find(std::string(ct.get(r, "iso3")));
                    if(it != nation_of_iso.end() && !ct.get(r, "name").empty()) nation_names_[it->second] = std::string(ct.get(r, "name"));
                }
            }
        }
        region_names_.assign(w.region_size(), std::string{});
        core::diagnostics rdiag;
        const std::string rpath = assets_ + "/scenario/" + scenario_ + "/regions.csv";
        if(std::FILE* probe = std::fopen(rpath.c_str(), "rb")) {
            std::fclose(probe);
            const auto rt = core::csv_table::load(rpath, rdiag);
            const auto ri = rt.column("iso3"), rn = rt.column("name"), rla = rt.column("lat"), rlo = rt.column("lon");
            std::map<std::string, std::vector<std::size_t>> rows_of;
            for(std::size_t r = 0; ri && rn && r < rt.rows(); ++r) rows_of[std::string(rt.get(r, *ri))].push_back(r);
            for(auto reg : w.in_region) {
                const auto owner = reg.get_owner();
                if(!owner || !rla || !rlo) continue;
                auto it = rows_of.find(core::unpack_key(owner.get_iso3()));
                if(it == rows_of.end()) continue;
                double best = 1e30;
                for(std::size_t r : it->second) {
                    const double dlat = rt.number_or(r, "lat", 0.0) - reg.get_lat();
                    const double dlon = rt.number_or(r, "lon", 0.0) - reg.get_lon();
                    const double d = dlat * dlat + dlon * dlon;
                    if(d < best) {
                        best = d;
                        region_names_[reg.id.index()] = std::string(rt.get(r, *rn));
                    }
                }
            }
        }
        // State names (states.csv rows; the slices' regions are their own states).
        state_names_.assign(w.state_size(), std::string{});
        const std::string spath = assets_ + "/scenario/" + scenario_ + "/states.csv";
        std::vector<std::string> by_row;
        if(std::FILE* probe = std::fopen(spath.c_str(), "rb")) {
            std::fclose(probe);
            const auto st = core::csv_table::load(spath, rdiag);
            for(std::size_t r = 0; r < st.rows(); ++r) by_row.emplace_back(st.get(r, "name"));
        }
        for(auto s : w.in_state) {
            const int32_t row = s.get_row();
            if(row >= 0 && static_cast<std::size_t>(row) < by_row.size()) state_names_[s.id.index()] = by_row[static_cast<std::size_t>(row)];
        }
        for(auto reg : w.in_region) { // own-state regions and new states: the region's name
            const auto s = reg.get_state();
            if(s && state_names_[s.id.index()].empty()) state_names_[s.id.index()] = region_names_[reg.id.index()];
        }
    }
    // A loaded save may carry the last session's control flags: the session starts with
    // the agents marked below only.
    for(auto ag : state->world.in_agent) {
        ag.set_player_controlled(0);
        ag.set_answer_requests(0);
    }
    // --nation: their executives and central banks start controlled.
    {
        const std::vector<std::string> wanted = iso3s_;
        iso3s_.clear();
        const std::vector<agent_info> all = collect_agents(*state, nation_names_, region_names_, state_names_, &web_names_ref());
        for(std::string const& iso : wanted) {
            const auto n = sim::find_nation(*state, iso);
            if(!n) {
                SDL_Log("nation %s not in scenario %s; its indicators are disabled", iso.c_str(), scenario_.c_str());
                continue;
            }
            for(uint8_t kind : {static_cast<uint8_t>(sim::agent_kind::executive), static_cast<uint8_t>(sim::agent_kind::central_bank)}) {
                for(auto const& a : all) {
                    if(a.kind == kind && a.nation == static_cast<uint32_t>(n.index())) {
                        agents_.push_back(a);
                        ai_.push_back(1);
                        answer_.push_back(1);
                        const dcon::agent_id id{static_cast<dcon::agent_id::value_base_t>(a.agent)};
                        state->world.agent_set_player_controlled(id, 1);
                        state->world.agent_set_answer_requests(id, 1);
                        break;
                    }
                }
            }
        }
        state_ = std::move(state); // track() reads state_
        refresh_nations();
        state = std::move(state_);
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
        // The player's commands due now (also while paused: they apply without unpausing).
        sim::drain_commands(s, io_->in, io_->out);

        const int sp = speed_.load();
        const auto now = clock::now();
        if(sp == 0) {
            measured_rate_.store(0.0);
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || !jobs_.empty() || !io_->in.empty() || speed_.load() != 0; });
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
                wake_.wait_until(lock, due, [&] { return quit_ || !jobs_.empty() || !io_->in.empty() || speed_.load() != sp; });
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
    if(status() != sim_status::running) return out;
    for(std::size_t i = 0; i < agents_.size(); ++i) {
        if(agents_[i].nation >= state_->world.nation_size()) continue;
        const std::size_t before = out.size();
        read_agent(agents_[i], ai_[i] != 0, track(agents_[i].nation), out);
        for(std::size_t k = before; k < out.size(); ++k) out[k].answer = answer_[i] != 0;
    }
    return out;
}

void sim_runner::read_agent(agent_info const& info, bool ai, controlled& c, std::vector<agent_panel>& out) {
    using sim::agent_kind;
    const auto kind = static_cast<agent_kind>(info.kind);
    auto const& w = state_->world;
    const dcon::nation_id n{static_cast<dcon::nation_id::value_base_t>(c.index)};
    auto& history_ = c.history;
    const float unemployment0_ = c.unemployment0, yield0_ = c.yield0;
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
    if(acc_days < 0.5) {
        // Day 0: nothing accumulated yet ("-").
    } else if(acc_days < 365.0 || history_.size() < 2) {
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
    gov.agent = info.agent;
    gov.ai = ai;
    gov.flag = c.flag;
    gov.icon = "\xF0\x9F\x8F\x9B"; // U+1F3DB classical building
    gov.title = c.name + " Government";
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
    if(kind == agent_kind::executive) {
        gov.title = info.label;
        out.push_back(std::move(gov));
        return;
    }

    agent_panel cb;
    cb.agent = info.agent;
    cb.ai = ai;
    cb.flag = c.flag;
    cb.icon = "\xF0\x9F\x8F\xA6"; // U+1F3E6 bank
    cb.title = c.name + " Central Bank";
    const double gap = inflation - trend_infl;
    cb.items.push_back({"Inflation", valued(inflation, "%.1f%%"), tone_of(std::fabs(gap) < 1.0, std::fabs(gap) > 2.5),
                        "CPI inflation (same window as GDP); trend " + fmt("%.1f%%", trend_infl)});
    cb.items.push_back({"Rate", valued(w.nation_get_policy_rate(n), "%.2f%%"), tone::neutral, "Policy interest rate"});
    const double y10 = w.nation_get_yield10(n);
    cb.items.push_back({"10y", valued(y10, "%.2f%%"), tone_of(y10 < yield0_ - 0.25, y10 > yield0_ + 0.5),
                        "10-year government bond yield (start: " + fmt("%.2f%%", yield0_) + ")"});
    const double fx = (w.nation_get_exchange_rate(n) - 1.0) * 100.0;
    cb.items.push_back({"FX", valued(fx, "%+.1f%%"), tone::neutral, "Real exchange rate vs the start"});
    if(kind == agent_kind::central_bank) {
        cb.title = info.label;
        out.push_back(std::move(cb));
        return;
    }

    // Other agents: a few indicators of what they act on.
    agent_panel p;
    p.agent = info.agent;
    p.ai = ai;
    p.flag = c.flag;
    p.title = info.label;
    const dcon::agent_id a{static_cast<dcon::agent_id::value_base_t>(info.agent)};
    switch(kind) {
    case agent_kind::union_federation:
        p.icon = "\u270A"; // raised fist
        p.items.push_back({"Unemp.", valued(unemployment, "%.1f%%"), tone::neutral, "Unemployment rate"});
        p.items.push_back({"Real wage", valued(w.nation_get_wage_real(n), "%.3f"), tone::neutral, "Real wage index"});
        p.items.push_back({"Density", valued(w.nation_get_union_density(n) * 100.0, "%.0f%%"), tone::neutral, "Union density"});
        break;
    case agent_kind::military:
        p.icon = "\U0001F396"; // military medal
        p.items.push_back({"Loyalty", valued(w.nation_get_military_loyalty(n), "%.2f"), tone::neutral, "Military loyalty to the government (0..1)"});
        p.items.push_back({"Defence", valued(w.nation_get_defense_share(n), "%.1f%%"), tone::neutral, "Defence spending, % of GDP"});
        break;
    case agent_kind::board:
        p.icon = "\U0001F3ED"; // factory
        if(info.firm < w.firm_size()) {
            const dcon::firm_id f{static_cast<dcon::firm_id::value_base_t>(info.firm)};
            p.items.push_back({"Hoarding", valued(w.firm_get_hoarding(f), "%.2f"), tone::neutral, "Labour hoarding in the downturn (0..1)"});
            p.items.push_back({"Leverage", valued(w.firm_get_leverage(f), "%.2f"), tone::neutral, "Firm leverage"});
        }
        p.items.push_back({"GDP", valued(growth, "%+.1f%%"), tone_of(growth > 1.0, growth < 0.0), "The nation's real GDP growth"});
        break;
    case agent_kind::governor:
        p.icon = "\U0001F3E2"; // office building
        if(info.region < w.region_size()) {
            const dcon::region_id r{static_cast<dcon::region_id::value_base_t>(info.region)};
            p.items.push_back({"Unrest", valued(w.region_get_unrest(r), "%.2f"), tone_of(false, w.region_get_unrest(r) > 1.5f), "Regional unrest"});
            p.items.push_back({"Grievance", valued(w.region_get_grievance(r), "%.2f"), tone::neutral, "Regional grievance"});
            p.items.push_back({"Trust", valued(w.region_get_trust_r(r), "%.2f"), tone::neutral, "Regional trust in institutions"});
        }
        break;
    case agent_kind::rebel:
        p.icon = "\U0001F3F4"; // black flag
        p.items.push_back({"Strength", valued(w.agent_get_strength(a), "%.2f"), tone::neutral, "Rebel strength"});
        p.items.push_back({"Demand", valued(w.agent_get_demand(a), "%.2f"), tone::neutral, "Rebel demand"});
        break;
    default: break;
    }
    out.push_back(std::move(p));
}

} // namespace geosim::client
