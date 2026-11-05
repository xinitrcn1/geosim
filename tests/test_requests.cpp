// Requests between agents (requests.cpp): AI targets answer at once; requests to an agent
// the player controls (and answers for) wait until their window closes, can be answered
// through the command queue, and are otherwise answered by the agent's AI.
#include "sim/commands.hpp"
#include "sim/model.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/systems.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

static int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if(!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while(0)

using namespace geosim;

int main() {
    auto s = std::make_unique<game_state>();
    const auto report = sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
    if(!report.ok()) {
        std::fprintf(stderr, "scenario: %s\n", report.error.c_str());
        return EXIT_FAILURE;
    }
    auto& w = s->world;
    // The player controls every government and answers its requests.
    for(auto n : w.in_nation) {
        if(auto g = n.get_executive_seat().get_agent()) {
            g.set_player_controlled(1);
            g.set_answer_requests(1);
        }
    }
    static sim::command_queue in;
    static sim::result_queue out;
    int max_open = 0, answered_by_player = 0, late = 0, immediate_to_player = 0, total = 0;
    uint32_t seq = 1;
    for(int d = 0; d < 500; ++d) {
        int open = 0;
        for(auto r : w.in_request) {
            ++open;
            CHECK(r.get_to().get_player_controlled() != 0);           // only the player's requests wait
            CHECK(r.get_expires() > s->current_tick - 1);              // never past their window
            CHECK(r.get_expires() - r.get_created() == static_cast<int32_t>(s->params.request_window_days));
            // Answer every other one ourselves, through the queue.
            if(answered_by_player < 3 && (r.id.index() % 2) == 0) {
                CHECK(in.push(sim::make_answer(r.id, r.get_to().id, true, s->current_tick, seq++)));
                ++answered_by_player;
            }
        }
        max_open = std::max(max_open, open);
        sim::drain_commands(*s, in, out);
        while(auto res = out.pop()) std::printf("answered: %s\n", res->message);
        sim::advance_day(*s);
    }
    for(auto dd : w.in_decision) {
        if(dd.get_lever() != sim::journal_request) continue;
        ++total;
        const bool to_gov = dd.get_agent() && dd.get_agent().get_kind() == static_cast<uint8_t>(sim::agent_kind::executive);
        if(to_gov && dd.get_prior() == 0.0f) ++immediate_to_player;
        if(dd.get_prior() == 2.0f) ++late;
    }
    std::printf("requests %d, open at once at most %d, answered by the player %d, by the AI after the window %d\n", total, max_open, answered_by_player, late);
    CHECK(total > 0);
    CHECK(max_open > 0);
    CHECK(immediate_to_player == 0); // the governments' AIs never answered before the window closed
    CHECK(late > 0);
    // Dropping the toggle: pending requests are answered at once, new ones immediately.
    for(auto n : w.in_nation) {
        if(auto g = n.get_executive_seat().get_agent()) g.set_answer_requests(0);
    }
    sim::advance_day(*s);
    int left = 0;
    for(auto r : w.in_request) {
        (void)r;
        ++left;
    }
    CHECK(left == 0);
    std::printf("test_requests: %s\n", failures ? "FAILED" : "ok");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
