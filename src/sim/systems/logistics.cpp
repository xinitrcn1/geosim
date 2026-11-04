// Logistics and transport (docs/03-economy.md#logistics-and-transport), daily.
//
// Ocean shipping is a network: sea nodes (port basins) joined by lanes, some of
// which pass chokepoints (assets/defs/sea_lanes.txt). Every day:
//
// 1. Chokepoints. Blocking incidents arrive at random (counter-based RNG) with a
//    hazard that rises with traffic, mean vessel size (Merk 2015) and the season;
//    durations are lognormal. Droughts cut canal capacity (Panama locks), with a
//    hazard rising with warming. A littoral nation's war or riots close a share
//    of the passage. Nothing is scheduled: a 2021-style Suez grounding is one
//    possible draw, not an event.
// 2. Routing. Each lane costs its sailing days plus, at a chokepoint, the passage
//    time and the *expected* wait a ship arriving today faces: the mean residual
//    duration of a lognormal blockage given how long it has lasted, plus the time
//    to clear the queue ahead. Carriers then take the cheapest route (all-pairs
//    shortest paths), so they wait while a blockage is expected to end soon and
//    divert round the Cape once waiting costs more than the detour.
// 3. Queues. Ships routed through a passage arrive at its traffic rate and are
//    served at its open capacity; the queue drains at the spare capacity after
//    reopening (Ever Given: ~420 ships, about 3.5 days).
// 4. Ports. Container throughput over capacity sets port waiting time through the
//    heavy-traffic queueing approximation (Kingman 1961): waits grow as
//    rho / (1 - rho). Port labour absence (sickness) and closures lower capacity.
// 5. Container freight market (Stopford 2009). Demand for ship-days = cargo x
//    voyage time (longer routes, port waits and ships stuck in queues all absorb
//    capacity); carriers deploy or idle ships with a lag (blank sailings); the spot
//    rate follows a convex supply curve near full utilisation; contract rates lag;
//    high rates order ships delivered after ~2.5 years (Kalouptsidi 2014).
// 6. Effects on nations: freight costs raise import prices (Carriere-Swallow et
//    al. 2023) and act as a trade cost (Hummels 2007; elasticity ~4, Simonovska &
//    Waugh 2014); persistent extra transit days cut bilateral trade (~1%/day,
//    Djankov, Freund & Pham 2010); a lengthening of transit times interrupts the
//    arrival of imported inputs, which the production network (economy.cpp)
//    buffers with input stocks; internal freight capacity falls with movement
//    restrictions and sickness (domestic food distribution, commodities.cpp).

#include "core/keys.hpp"
#include "core/rng.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace geosim::sim {

namespace {

constexpr float inf = std::numeric_limits<float>::infinity();

// Standard normal CDF.
double phi(double x) {
    return 0.5 * std::erfc(-x / std::numbers::sqrt2);
}

// Mean remaining duration of a lognormal(mu, sigma) spell that has lasted t days.
double lognormal_residual(double median, double sigma, double t) {
    const double mu = std::log(std::max(1e-3, median));
    const double mean = std::exp(mu + 0.5 * sigma * sigma);
    if(t <= 1e-6) {
        return mean;
    }
    const double z = (std::log(t) - mu) / sigma;
    const double survive = 1.0 - phi(z);
    if(survive < 1e-9) {
        return 0.5; // far in the tail: a day or so
    }
    const double conditional = mean * phi((mu + sigma * sigma - std::log(t)) / sigma) / survive;
    return std::max(0.0, conditional - t);
}

struct network {
    int K = 0;
    std::vector<float> dist;            // K x K shortest days
    std::vector<int> next;              // next node on the shortest path
    std::vector<int> edge_lane;         // lane used between adjacent nodes
};

network shortest_paths(game_state const& state, bool baseline) {
    auto const& w = state.world;
    network net;
    net.K = static_cast<int>(w.sea_node_size());
    const int K = net.K;
    net.dist.assign(static_cast<std::size_t>(K * K), inf);
    net.next.assign(static_cast<std::size_t>(K * K), -1);
    net.edge_lane.assign(static_cast<std::size_t>(K * K), -1);
    for(int i = 0; i < K; ++i) {
        net.dist[static_cast<std::size_t>(i * K + i)] = 0.0f;
        net.next[static_cast<std::size_t>(i * K + i)] = i;
    }
    const float speed = static_cast<float>(state.params.ship_speed_knots) * 24.0f;
    for(auto lane : w.in_sea_lane) {
        const int a = static_cast<int>(lane.get_from().id.index());
        const int b = static_cast<int>(lane.get_to().id.index());
        float days = lane.get_nm() / speed;
        if(auto c = lane.get_chokepoint()) {
            days += c.get_canal_days();
            if(!baseline) {
                days += c.get_wait_days();
            }
        }
        for(auto [x, y] : {std::pair{a, b}, std::pair{b, a}}) {
            const auto k = static_cast<std::size_t>(x * K + y);
            if(days < net.dist[k]) {
                net.dist[k] = days;
                net.next[k] = y;
                net.edge_lane[k] = static_cast<int>(lane.id.index());
            }
        }
    }
    for(int m = 0; m < K; ++m) {
        for(int i = 0; i < K; ++i) {
            const float dim = net.dist[static_cast<std::size_t>(i * K + m)];
            if(dim == inf) continue;
            for(int j = 0; j < K; ++j) {
                const float alt = dim + net.dist[static_cast<std::size_t>(m * K + j)];
                if(alt < net.dist[static_cast<std::size_t>(i * K + j)]) {
                    net.dist[static_cast<std::size_t>(i * K + j)] = alt;
                    net.next[static_cast<std::size_t>(i * K + j)] = net.next[static_cast<std::size_t>(i * K + m)];
                }
            }
        }
    }
    return net;
}

// Nation -> (node, share) lists, shares normalised.
struct port_list {
    std::vector<std::pair<int, float>> nodes;
};

std::vector<port_list> ports_of(game_state const& state) {
    auto const& w = state.world;
    std::vector<port_list> out(w.nation_size());
    for(auto pa : w.in_port_access) {
        out[pa.get_nation().id.index()].nodes.emplace_back(static_cast<int>(pa.get_node().id.index()), pa.get_share());
    }
    for(auto& p : out) {
        float sum = 0.0f;
        for(auto const& [n, s] : p.nodes) sum += s;
        for(auto& [n, s] : p.nodes) s = sum > 0.0f ? s / sum : 0.0f;
    }
    return out;
}

// Sea days between two nations (share-weighted over their port nodes).
float sea_days(network const& net, port_list const& a, port_list const& b) {
    double t = 0.0, wsum = 0.0;
    for(auto const& [i, si] : a.nodes) {
        for(auto const& [j, sj] : b.nodes) {
            const float d = net.dist[static_cast<std::size_t>(i * net.K + j)];
            if(d == inf) continue;
            t += static_cast<double>(si) * sj * d;
            wsum += static_cast<double>(si) * sj;
        }
    }
    return wsum > 0.0 ? static_cast<float>(t / wsum) : 0.0f;
}

// Real goods flow by sea from a to b (detrended so the baseline is constant): the
// exporter's shipments to b, scaled by how b's own import demand has moved (shifts
// of spending toward goods, restocking of input inventories).
float sea_flow(game_state const& state, dcon::nation_id a, dcon::nation_id b) {
    auto const& w = state.world;
    const auto kb = static_cast<uint8_t>(b.index());
    const float gf = std::max(0.01f, w.nation_get_gf(a));
    const float exports = w.nation_get_exports(a) > 0.0f ? w.nation_get_exports(a) : w.nation_get_x0(a) * gf;
    const float m_base = w.nation_get_m0(b) * std::max(0.01f, w.nation_get_gf(b));
    const float x_index = exports / std::max(1e-6f, w.nation_get_x0(a) * gf);
    const float m_index = m_base > 0.0f && w.nation_get_imports(b) > 0.0f ? w.nation_get_imports(b) / m_base : 1.0f;
    return w.nation_get_x0(a) * std::sqrt(std::max(0.0f, x_index * m_index)) * w.nation_get_goods_export_share(a) * w.nation_get_trade_share(a, kb)
           * (1.0f - w.nation_get_land_share(a, kb)) * w.nation_get_seaborne_share(a);
}

// Ship-days a lane pair needs: ships sail both ways, so capacity covers the heavier
// direction and the lighter one carries empties back (container imbalance).
double lane_demand(float f_ab, float f_ba, float t_ab, float t_ba) {
    return static_cast<double>(std::max(f_ab, f_ba)) * (static_cast<double>(t_ab) + t_ba);
}

float working_age_absence(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    double inf_y = 0.0, young = 0.0;
    for(auto own : w.nation_get_region_ownership(n)) {
        auto r = own.get_region();
        for(uint8_t p = 0; p < state.derived.pathogen_count; ++p) inf_y += r.get_ep_i(epi_index(p, age_young));
        const uint8_t y0 = epi_index(0, age_young);
        young += r.get_ep_s(y0) + r.get_ep_e(y0) + r.get_ep_i(y0) + r.get_ep_r(y0);
    }
    return young > 0.0 ? static_cast<float>(std::min(0.5, 0.5 * inf_y / young)) : 0.0f;
}

void chokepoint_usage(network const& net, std::vector<port_list> const& ports, std::vector<std::vector<float>> const& flows,
                      game_state const& state, std::vector<double>& usage) {
    auto const& w = state.world;
    const int K = net.K;
    std::vector<double> node_flow(static_cast<std::size_t>(K * K), 0.0);
    for(std::size_t a = 0; a < flows.size(); ++a) {
        for(std::size_t b = 0; b < flows.size(); ++b) {
            const float f = flows[a][b];
            if(f <= 0.0f) continue;
            for(auto const& [i, si] : ports[a].nodes) {
                for(auto const& [j, sj] : ports[b].nodes) node_flow[static_cast<std::size_t>(i * K + j)] += static_cast<double>(f) * si * sj;
            }
        }
    }
    usage.assign(w.chokepoint_size(), 0.0);
    for(int i = 0; i < K; ++i) {
        for(int j = 0; j < K; ++j) {
            const double f = node_flow[static_cast<std::size_t>(i * K + j)];
            if(f <= 0.0 || i == j) continue;
            int at = i;
            for(int guard = 0; at != j && guard < K; ++guard) {
                const int nx = net.next[static_cast<std::size_t>(at * K + j)];
                if(nx < 0) break;
                const int lane = net.edge_lane[static_cast<std::size_t>(at * K + nx)];
                if(lane >= 0) {
                    if(auto c = w.sea_lane_get_chokepoint(dcon::sea_lane_id{static_cast<dcon::sea_lane_id::value_base_t>(lane)})) {
                        usage[c.id.index()] += f;
                    }
                }
                at = nx;
            }
        }
    }
}

std::vector<std::vector<float>> all_flows(game_state const& state) {
    auto const& w = state.world;
    const auto N = w.nation_size();
    std::vector<std::vector<float>> f(N, std::vector<float>(N, 0.0f));
    for(auto a : w.in_nation) {
        for(auto b : w.in_nation) {
            if(a.id != b.id) f[a.id.index()][b.id.index()] = sea_flow(state, a.id, b.id);
        }
    }
    return f;
}

} // namespace

// Baseline sea distances between nations (before trade shares exist): transit days
// at the start and the effective trade distance used by gravity (geopolitics.cpp).
void init_logistics_routes(game_state& state) {
    auto& w = state.world;
    if(w.sea_node_size() == 0) {
        return;
    }
    const auto net = shortest_paths(state, true);
    const auto ports = ports_of(state);
    const float km_per_day = static_cast<float>(state.params.ship_speed_knots) * 24.0f * 1.852f;
    for(auto a : w.in_nation) {
        for(auto b : w.in_nation) {
            const auto kb = static_cast<uint8_t>(b.id.index());
            if(a.id == b.id) continue;
            const float days = sea_days(net, ports[a.id.index()], ports[b.id.index()]);
            a.set_transit_days0(kb, days);
            a.set_transit_days(kb, days);
            a.set_pair_cost(kb, 1.0f);
            if(days > 0.0f) {
                // Effective distance: sea route for seaborne trade, road distance (great circle x 1.2)
                // for overland trade; a floor keeps neighbours from dominating.
                const float land = a.get_land_share(kb);
                const float road = 1.2f * a.get_trade_distance(kb); // great-circle km preset by the loader
                a.set_trade_distance(kb, std::max(500.0f, land * road + (1.0f - land) * days * km_per_day));
            }
        }
    }
}

// Baseline flows (after trade shares exist): chokepoint flows, port throughput, the
// freight market at rest.
void init_logistics_flows(game_state& state) {
    auto& w = state.world;
    if(w.sea_node_size() == 0) {
        return;
    }
    const auto net = shortest_paths(state, true);
    const auto ports = ports_of(state);
    const auto flows = all_flows(state);
    std::vector<double> usage;
    chokepoint_usage(net, ports, flows, state, usage);
    for(auto c : w.in_chokepoint) {
        c.set_flow0(static_cast<float>(usage[c.id.index()]));
        c.set_traffic(c.get_traffic0());
        c.set_open(1.0f);
        c.set_incident_open(1.0f);
    }
    for(auto n : w.in_nation) {
        double through = 0.0, imports = 0.0, sea_imports = 0.0;
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            through += flows[n.id.index()][m.id.index()] + flows[m.id.index()][n.id.index()];
            sea_imports += flows[m.id.index()][n.id.index()];
            imports += m.get_x0() * m.get_goods_export_share() * m.get_trade_share(static_cast<uint8_t>(n.id.index()));
        }
        n.set_port_throughput0(static_cast<float>(through));
        n.set_sea_import_share(imports > 0.0 ? static_cast<float>(sea_imports / imports) : 0.0f);
        n.set_port_util(static_cast<float>(state.params.port_util0));
        n.set_port_dwell0(static_cast<float>(state.params.port_time_days));
        n.set_port_dwell(n.get_port_dwell0());
        n.set_input_availability(1.0f);
        n.set_import_cost_index(1.0f);
        n.set_import_cost_prev(1.0f);
        n.set_trade_cost_factor(1.0f);
        n.set_domestic_logistics(1.0f);
        n.set_freight_cost(static_cast<float>(state.params.freight_cost_share0));
    }
    double demand0 = 0.0;
    const float port2 = 2.0f * static_cast<float>(state.params.port_time_days);
    for(auto a : w.in_nation) {
        for(auto b : w.in_nation) {
            if(a.id.index() >= b.id.index()) continue;
            demand0 += lane_demand(flows[a.id.index()][b.id.index()], flows[b.id.index()][a.id.index()],
                                   a.get_transit_days0(static_cast<uint8_t>(b.id.index())) + port2,
                                   b.get_transit_days0(static_cast<uint8_t>(a.id.index())) + port2);
        }
    }
    for(auto s : w.in_shipping) {
        s.set_fleet(1.0f);
        s.set_deployed(static_cast<float>(1.0 - state.params.fleet_idle0));
        s.set_demand0(static_cast<float>(demand0));
        s.set_demand(static_cast<float>(demand0));
        s.set_utilisation(static_cast<float>(state.params.fleet_util0));
        s.set_spot_rate(1.0f);
        s.set_contract_rate(1.0f);
        s.set_orderbook(0.0f);
        s.set_world_trade(1.0f);
    }
}

void update_logistics(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    if(w.sea_node_size() == 0 || w.shipping_size() == 0) {
        return;
    }
    const auto ship = dcon::shipping_id{0};
    const auto tick = static_cast<uint32_t>(state.current_tick);
    const float years = static_cast<float>(state.current_tick) / 365.0f;
    const float doy = static_cast<float>(tick % 365u);
    const float size_index = std::exp(static_cast<float>(P.vessel_size_growth) * years);
    float warming = 0.0f;
    for(auto c : w.in_climate) warming = c.get_temperature() - static_cast<float>(P.temperature0);

    // 1. Chokepoint states.
    for(auto c : w.in_chokepoint) {
        const auto ci = static_cast<uint32_t>(c.id.index());
        float conflict_target = 0.0f;
        for(auto lit : c.get_littoral()) {
            auto n = lit.get_nation();
            float war = 0.0f;
            for(auto m : w.in_nation) war = std::max(war, n.get_at_war(static_cast<uint8_t>(m.id.index())));
            float unrest = 0.0f, pop = 0.0f;
            for(auto own : n.get_region_ownership()) {
                unrest += own.get_region().get_unrest() * own.get_region().get_population();
                pop += own.get_region().get_population();
            }
            unrest = pop > 0.0f ? unrest / pop : 0.0f;
            conflict_target = std::max({conflict_target, static_cast<float>(P.chokepoint_war_closure) * war,
                                        static_cast<float>(P.chokepoint_unrest_closure) * std::max(0.0f, unrest - 3.0f)});
        }
        c.set_conflict(c.get_conflict() + (std::min(1.0f, conflict_target) - c.get_conflict()) / 7.0f);

        if(c.get_drought_left() > 0.0f) {
            c.set_drought_left(std::max(0.0f, c.get_drought_left() - 1.0f));
        } else if(c.get_drought_rate() > 0.0f) {
            const float p = static_cast<float>(P.incident_rate_scale) * c.get_drought_rate() / 365.0f
                            * std::max(0.0f, 1.0f + static_cast<float>(P.drought_temp_sensitivity) * warming);
            if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::logistics), tick, ci, 3) < p) {
                c.set_drought_left(c.get_drought_days());
            }
        }

        if(c.get_blocked_left() > 0.0f) {
            c.set_blocked_left(std::max(0.0f, c.get_blocked_left() - 1.0f));
            c.set_blocked_elapsed(c.get_blocked_elapsed() + 1.0f);
            if(c.get_blocked_left() <= 0.0f) {
                c.set_blocked_elapsed(0.0f);
                c.set_incident_open(1.0f);
            }
        } else if(c.get_incident_rate() > 0.0f) {
            const float traffic_rel = c.get_traffic0() > 0.0f ? c.get_traffic() / c.get_traffic0() : 1.0f;
            const float season = 1.0f + c.get_season_amp() * std::cos(2.0f * std::numbers::pi_v<float> * (doy - c.get_season_peak()) / 365.0f);
            const float p = static_cast<float>(P.incident_rate_scale) * c.get_incident_rate() / 365.0f * traffic_rel * std::pow(size_index, c.get_size_elasticity()) * std::max(0.0f, season);
            if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::logistics), tick, ci, 0) < p) {
                const float u1 = std::max(1e-7f, core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::logistics), tick, ci, 1));
                const float u2 = core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::logistics), tick, ci, 2);
                const float z = std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * std::numbers::pi_v<float> * u2);
                const float days = std::max(0.25f, c.get_duration_median() * std::exp(c.get_duration_sigma() * z));
                c.set_blocked_left(days);
                c.set_blocked_elapsed(0.0f);
                c.set_incident_open(1.0f - c.get_incident_cut());
                c.set_incidents(c.get_incidents() + 1.0f);
                const auto d = w.create_decision();
                w.decision_set_tick(d, state.current_tick);
                w.decision_set_lever(d, journal_chokepoint);
                w.decision_set_before(d, static_cast<float>(ci));
                w.decision_set_after(d, days);
                w.decision_set_pragmatic(d, NAN);
                w.decision_set_ideological(d, NAN);
                w.decision_set_prior(d, NAN);
            }
        }
        const float drought = c.get_drought_left() > 0.0f ? 1.0f - c.get_drought_cut() : 1.0f;
        c.set_open(c.get_incident_open() * drought * (1.0f - c.get_conflict()));
    }

    // 2. Expected waits (yesterday's queue and traffic) -> lane costs -> routes.
    for(auto c : w.in_chokepoint) {
        const float cap = c.get_capacity() * c.get_open();
        const float traffic = c.get_traffic();
        float wait = 0.0f;
        if(c.get_blocked_left() > 0.0f && c.get_incident_open() <= 0.05f) {
            // Closed: expected remaining blockage, then the queue (plus ships arriving
            // meanwhile) clears at the spare capacity after reopening.
            const auto remaining = static_cast<float>(lognormal_residual(c.get_duration_median(), c.get_duration_sigma(), c.get_blocked_elapsed()));
            const float cap_after = c.get_capacity() * (c.get_drought_left() > 0.0f ? 1.0f - c.get_drought_cut() : 1.0f) * (1.0f - c.get_conflict());
            const float backlog = c.get_queue() + traffic * remaining;
            wait = remaining + backlog / std::max(1.0f, cap_after - traffic) * 0.5f; // a ship waits for half the backlog on average
        } else {
            wait = c.get_queue() / std::max(1.0f, cap);
        }
        wait += c.get_conflict() * static_cast<float>(P.war_risk_days);
        c.set_wait_days(wait);
    }
    const auto net = shortest_paths(state, false);
    const auto ports = ports_of(state);

    // 3. Flows, chokepoint traffic and queues.
    const auto flows = all_flows(state);
    std::vector<double> usage;
    chokepoint_usage(net, ports, flows, state, usage);
    double stuck_ships = 0.0;
    for(auto c : w.in_chokepoint) {
        const double u0 = c.get_flow0();
        const float traffic = u0 > 0.0 ? static_cast<float>(c.get_traffic0() * usage[c.id.index()] / u0) : c.get_traffic0();
        c.set_traffic(traffic);
        const float cap = c.get_capacity() * c.get_open();
        const float served = std::min(c.get_queue() + traffic, cap);
        const float queue = std::max(0.0f, c.get_queue() + traffic - served);
        c.set_queue(queue);
        c.set_ship_days_lost(c.get_ship_days_lost() + queue);
        stuck_ships += queue;
    }

    // 4. Ports: throughput against capacity -> waiting time (Kingman).
    const float r0 = static_cast<float>(P.port_util0);
    const float g0 = r0 / (1.0f - r0);
    const auto c2 = [&]() -> uint8_t {
        for(auto k : w.in_npi_indicator) {
            if(core::unpack_key(k.get_key()) == "C2") return static_cast<uint8_t>(k.id.index());
        }
        return 0xFF;
    }();
    for(auto n : w.in_nation) {
        double through = 0.0;
        for(auto m : w.in_nation) {
            if(m.id != n.id) through += flows[n.id.index()][m.id.index()] + flows[m.id.index()][n.id.index()];
        }
        const float rel = n.get_port_throughput0() > 0.0f ? static_cast<float>(through / n.get_port_throughput0()) : 1.0f;
        const float absent = working_age_absence(state, n.id);
        n.set_port_absent(absent);
        const float closures = c2 != 0xFF ? npi_fraction(state, n.id, c2) : 0.0f;
        const float avail = std::clamp(1.0f - static_cast<float>(P.port_absence_effect) * absent - static_cast<float>(P.port_npi_effect) * closures, 0.3f, 1.0f);
        const float rho = std::min(static_cast<float>(P.port_util_max), r0 * rel / avail);
        n.set_port_util(rho);
        n.set_port_dwell(n.get_port_dwell0() * (0.5f + 0.5f * (rho / (1.0f - rho)) / g0));
        // Internal freight (trucking, rail): movement restrictions and sickness.
        const float move = D.npi_movement != 0xFF ? npi_fraction(state, n.id, D.npi_movement) : 0.0f;
        n.set_domestic_logistics(std::clamp(1.0f - static_cast<float>(P.domestic_logistics_npi) * move
                                                - static_cast<float>(P.domestic_logistics_absence) * absent, 0.3f, 1.0f));
    }

    // 5. Nation-pair transit times and the demand for ship-days.
    const float smooth = 1.0f / static_cast<float>(P.delay_smoothing_days);
    double demand = 0.0;
    for(auto a : w.in_nation) {
        for(auto b : w.in_nation) {
            if(a.id == b.id) continue;
            const auto kb = static_cast<uint8_t>(b.id.index());
            const float days = sea_days(net, ports[a.id.index()], ports[b.id.index()]) + a.get_port_dwell() + b.get_port_dwell();
            a.set_transit_days(kb, days);
            const float extra = days - (a.get_transit_days0(kb) + 2.0f * static_cast<float>(P.port_time_days));
            const float sea = 1.0f - a.get_land_share(kb);
            const float target = std::exp(-static_cast<float>(P.trade_delay_semi) * extra * sea);
            a.set_pair_cost(kb, a.get_pair_cost(kb) + (target - a.get_pair_cost(kb)) * smooth);
        }
    }
    for(auto a : w.in_nation) {
        for(auto b : w.in_nation) {
            if(a.id.index() >= b.id.index()) continue;
            demand += lane_demand(flows[a.id.index()][b.id.index()], flows[b.id.index()][a.id.index()],
                                  a.get_transit_days(static_cast<uint8_t>(b.id.index())), b.get_transit_days(static_cast<uint8_t>(a.id.index())));
        }
    }
    // Ships stuck in chokepoint queues are capacity taken out of the fleet.
    auto sh = w.shipping_get_demand0(ship);
    const float stuck_share = static_cast<float>(stuck_ships * P.container_ship_share / P.fleet_ships);

    // 6. Freight market.
    {
        const float fleet = w.shipping_get_fleet(ship);
        float deployed = w.shipping_get_deployed(ship);
        const float u0 = static_cast<float>(P.fleet_util0);
        const float d0 = static_cast<float>(1.0 - P.fleet_idle0);
        const float demand_cap = sh > 0.0f ? static_cast<float>(demand / sh) * u0 * d0 + stuck_share : u0 * d0;
        w.shipping_set_demand(ship, static_cast<float>(demand));
        w.shipping_set_world_trade(ship, sh > 0.0f ? static_cast<float>(demand / sh) : 1.0f);
        const float target_deployed = std::clamp(demand_cap / (fleet * static_cast<float>(P.fleet_util_target)), 0.75f, 0.99f);
        deployed += (target_deployed - deployed) / static_cast<float>(P.fleet_deploy_days);
        w.shipping_set_deployed(ship, deployed);
        const float util = demand_cap / std::max(0.1f, fleet * deployed);
        w.shipping_set_utilisation(ship, util);
        const float rate_target = std::pow((1.0f - u0) / std::max(0.015f, 1.0f - std::min(util, 0.985f)), static_cast<float>(P.freight_curve));
        float spot = w.shipping_get_spot_rate(ship);
        spot += (rate_target - spot) * static_cast<float>(P.freight_speed);
        w.shipping_set_spot_rate(ship, spot);
        float contract = w.shipping_get_contract_rate(ship);
        contract += (spot - contract) / static_cast<float>(P.freight_contract_days);
        w.shipping_set_contract_rate(ship, contract);
        float orderbook = w.shipping_get_orderbook(ship);
        orderbook += static_cast<float>(P.newbuild_order_rate) / 365.0f * std::max(0.0f, spot - 1.3f) * fleet;
        const float delivered = orderbook / static_cast<float>(P.newbuild_lag_days);
        w.shipping_set_orderbook(ship, orderbook - delivered);
        w.shipping_set_fleet(ship, fleet + delivered);
        w.shipping_set_acc_rate(ship, w.shipping_get_acc_rate(ship) + spot);
        w.shipping_set_acc_days(ship, w.shipping_get_acc_days(ship) + 1.0);
        w.shipping_set_acc_idle(ship, std::max(w.shipping_get_acc_idle(ship), static_cast<double>(1.0f - deployed)));
    }

    // 7. Nation effects: freight costs, import prices, input arrivals.
    const float spot = w.shipping_get_spot_rate(ship);
    const float rate = static_cast<float>(P.freight_spot_share) * spot + static_cast<float>(1.0 - P.freight_spot_share) * w.shipping_get_contract_rate(ship);
    const float share0 = static_cast<float>(P.freight_cost_share0);
    for(auto n : w.in_nation) {
        n.set_freight_cost(share0 * rate);
        n.set_import_cost_index(1.0f + n.get_sea_import_share() * share0 * (rate - 1.0f));
        n.set_trade_cost_factor(std::exp(-static_cast<float>(P.trade_freight_elasticity) * n.get_sea_import_share() * share0 * (rate - 1.0f)));
        // Import-weighted change in transit days (sea leg + both ports).
        double num = 0.0, den = 0.0;
        const auto kn = static_cast<uint8_t>(n.id.index());
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            const double f = flows[m.id.index()][n.id.index()];
            num += f * (m.get_transit_days(kn) - m.get_transit_days0(kn) - 2.0 * P.port_time_days);
            den += f;
        }
        const float delay = den > 0.0 ? static_cast<float>(num / den) : 0.0f;
        // A longer pipeline delivers nothing new for the added days: arrivals fall by the
        // daily increase of transit time (and surge when it shortens).
        const float arrivals = std::clamp(1.0f - (delay - n.get_transit_prev()), 0.0f, 2.0f);
        n.set_transit_prev(delay);
        n.set_logistics_delay(delay);
        n.set_logistics_delay_slow(n.get_logistics_delay_slow() + (delay - n.get_logistics_delay_slow()) * smooth);
        // Weekly economy reads the mean arrival rate since its last step.
        n.set_input_availability(n.get_input_availability() + (arrivals - n.get_input_availability()) / 7.0f);
    }
}

} // namespace geosim::sim
