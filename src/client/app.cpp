#include "app.hpp"

#include "audio/jukebox.hpp"
#include "audio/music.hpp"
#include "client_files.hpp"
#include "hazard_feed.hpp"
#include "map_data.hpp"
#include "map_layer.hpp"
#include "map_renderer.hpp"
#include "map_view.hpp"
#include "sim_runner.hpp"
#include "style.hpp"
#include "text.hpp"
#include "ui_context.hpp"
#include "ui_layout.hpp"
#include "ui_renderer.hpp"
#include "vk_context.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <memory>

namespace geosim::client {

namespace {

using steady = std::chrono::steady_clock;

struct window_metrics {
    int pixel_w = 0, pixel_h = 0;
    float pixel_density = 1; // pixels per window coordinate (mouse events are in window coordinates)
    float display_scale = 1; // UI scale from the display (density x content scale)
};

window_metrics query_metrics(SDL_Window* w) {
    window_metrics m;
    SDL_GetWindowSizeInPixels(w, &m.pixel_w, &m.pixel_h);
    m.pixel_density = SDL_GetWindowPixelDensity(w);
    m.display_scale = SDL_GetWindowDisplayScale(w);
    if(m.pixel_density <= 0) m.pixel_density = 1;
    if(m.display_scale <= 0) m.display_scale = 1;
    return m;
}

// --inject: synthesises SDL events at given frames (see app_options::inject).
void inject_events(SDL_Window* window, std::string const& script, int frame) {
    std::size_t pos = 0;
    while(pos < script.size()) {
        const std::size_t end = std::min(script.find(';', pos), script.size());
        const std::string item = script.substr(pos, end - pos);
        pos = end + 1;
        int f = -1;
        char kind[16] = {}, args[64] = {};
        if(std::sscanf(item.c_str(), "%d:%15[^:]:%63s", &f, kind, args) != 3 || f != frame) continue;
        const SDL_WindowID wid = SDL_GetWindowID(window);
        float x = 0, y = 0, dy = 0;
        SDL_Event ev{};
        if(std::strcmp(kind, "key") == 0) {
            ev.type = SDL_EVENT_KEY_DOWN;
            ev.key.windowID = wid;
            ev.key.key = SDL_GetKeyFromName(args);
            ev.key.down = true;
            SDL_PushEvent(&ev);
        } else if(std::strcmp(kind, "move") == 0 && std::sscanf(args, "%f,%f", &x, &y) == 2) {
            ev.type = SDL_EVENT_MOUSE_MOTION;
            ev.motion.windowID = wid;
            ev.motion.x = x;
            ev.motion.y = y;
            SDL_PushEvent(&ev);
        } else if(std::strcmp(kind, "click") == 0 && std::sscanf(args, "%f,%f", &x, &y) == 2) {
            for(Uint32 type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
                ev = {};
                ev.type = type;
                ev.button.windowID = wid;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                ev.button.x = x;
                ev.button.y = y;
                SDL_PushEvent(&ev);
            }
        } else if(std::strcmp(kind, "wheel") == 0 && std::sscanf(args, "%f,%f,%f", &x, &y, &dy) == 3) {
            ev.type = SDL_EVENT_MOUSE_MOTION;
            ev.motion.windowID = wid;
            ev.motion.x = x;
            ev.motion.y = y;
            SDL_PushEvent(&ev);
            ev = {};
            ev.type = SDL_EVENT_MOUSE_WHEEL;
            ev.wheel.windowID = wid;
            ev.wheel.y = dy;
            SDL_PushEvent(&ev);
        } else if(std::strcmp(kind, "resize") == 0 && std::sscanf(args, "%f,%f", &x, &y) == 2) {
            SDL_SetWindowSize(window, static_cast<int>(x), static_cast<int>(y));
        }
        SDL_Log("inject frame %d: %s", frame, item.c_str());
    }
}

// Music mood from what the HUD already shows (docs/14-audio.md#mood-mapping):
// the share of indicators coloured bad / good for the player and the bad
// alerts of the last ~4 months (a war alert involving the player forces
// "crisis"); any controlled nation at war in the simulation means "wartime".
struct music_mood {
    audio::mood m = audio::mood::calm;
    float intensity = 0.3f;
};
music_mood derive_music_mood(sim_runner& sim, alert_feed const& alerts) {
    music_mood out;
    if(sim.status() != sim_status::running) return out;
    int good = 0, bad = 0, total = 0;
    for(auto const& panel : sim.read_agents()) {
        for(auto const& item : panel.items) {
            ++total;
            good += item.t == tone::good;
            bad += item.t == tone::bad;
        }
    }
    const int32_t now = sim.tick();
    int bad_alerts = 0, peace = 0;
    bool war = false;
    for(auto const& a : alerts.items()) {
        if(a.tick <= 0 || now - a.tick > 120) continue; // local notices have tick 0
        if(a.t == tone::bad) {
            ++bad_alerts;
            war = war || a.icon == "\u2694";
        } else if(a.t == tone::good && a.icon == "\U0001F54A") {
            ++peace;
        }
    }
    const float bad_share = total > 0 ? static_cast<float>(bad) / static_cast<float>(total) : 0.0f;
    const float good_share = total > 0 ? static_cast<float>(good) / static_cast<float>(total) : 0.0f;
    const float stress = bad_share + 0.15f * static_cast<float>(std::min(bad_alerts, 4)) + (war ? 0.4f : 0.0f);
    if(sim.player_at_war()) out.m = audio::mood::wartime; // any controlled nation at war (the sim's at_war state) overrides
    else if(war || stress >= 0.65f) out.m = audio::mood::crisis;
    else if(stress >= 0.3f) out.m = audio::mood::tense;
    else if((good_share >= 0.4f && bad == 0) || (peace > 0 && stress < 0.15f)) out.m = audio::mood::triumph;
    else out.m = audio::mood::calm;
    float intensity = 0.25f + 0.65f * stress;
    if(out.m == audio::mood::triumph) intensity = 0.55f + 0.3f * good_share;
    if(out.m == audio::mood::wartime) intensity = std::max(intensity, 0.75f);
    if(sim.speed() >= 4) intensity += 0.1f; // fast time feels busier
    out.intensity = std::clamp(intensity, 0.15f, 1.0f);
    return out;
}

// --hazard-demo: two tropical cyclones drifting west-northwest and a great
// earthquake, to check the map's weather and event layers without the sim feed.
void demo_hazards(double days, float seconds, std::vector<map_storm>& storms, std::vector<map_quake>& quakes) {
    const auto d = static_cast<float>(std::fmod(days, 12.0));
    storms = {{-62.0f - 1.6f * d, 18.0f + 0.5f * d, 62.0f, 260.0f}, {138.0f - 1.2f * d, 14.0f + 0.7f * d, 45.0f, 320.0f},
              {-150.0f, -15.0f, 25.0f, 180.0f}};
    quakes = {{1, 142.4f, 38.3f, 9.0f, 0.5f, seconds}};
}

// --map-view "lon,lat,span": focus in degrees, window width in degrees of arc.
bool parse_map_view(std::string const& s, double& lon, double& lat, double& span) {
    return std::sscanf(s.c_str(), "%lf,%lf,%lf", &lon, &lat, &span) == 3 && span > 0.0;
}

int fail(char const* stage, std::string const& message) {
    SDL_Log("geosim: %s failed: %s", stage, message.c_str());
    return 1;
}

} // namespace

int run_app(app_options const& opt) {
    if(!SDL_Init(SDL_INIT_VIDEO)) {
        return fail("SDL_Init", SDL_GetError());
    }
    SDL_Log("SDL video driver: %s", SDL_GetCurrentVideoDriver());
    SDL_Window* window = SDL_CreateWindow("geosim", opt.width, opt.height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if(!window) {
        const std::string err = SDL_GetError();
        SDL_Quit();
        return fail("SDL_CreateWindow", err);
    }

    int exit_code = 0;
    {
        core::diagnostics diag;
        const char* env_validation = std::getenv("GEOSIM_VK_VALIDATION");
        const bool validation = opt.validation || (env_validation && std::strcmp(env_validation, "0") != 0);
        vk_context vk;
        text_system text;
        ui_renderer renderer;
        map_renderer map;
        sim_runner sim;
        window_metrics metrics = query_metrics(window);

        if(!vk.init(window, validation, diag)) {
            SDL_DestroyWindow(window);
            SDL_Quit();
            return fail("Vulkan initialisation", diag.error());
        }
        SDL_Log("Vulkan device: %s", vk.device_name.c_str());
        if(!text.init(opt.assets + "/fonts", diag) || !vk.create_swapchain(static_cast<uint32_t>(metrics.pixel_w), static_cast<uint32_t>(metrics.pixel_h), diag)
           || !renderer.init(vk, diag) || !map.init(vk, vk.swap_format, diag)) {
            map.shutdown();
            renderer.shutdown();
            vk.shutdown();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return fail("renderer initialisation", diag.error());
        }
        SDL_Log("swapchain %ux%u, emoji flag ligatures: %zu", vk.swap_extent.width, vk.swap_extent.height, text.flag_ligatures());

        // The world map: decoded on a worker thread, uploaded when ready (docs/07).
        map_loader map_load;
        map_load.start(opt.assets + "/map/world.gsmap");
        bool map_loaded = false;
        map_layer political;
        map_view view;
        view.set_focus(0.0, 20.0 * map_deg);
        view.scale = static_cast<double>(metrics.pixel_w) / (75.0 * map_deg);
        bool dragging = false;
        float press_x = 0, press_y = 0;
        double menu_lon = 0.0;   // the menu camera's longitude (degrees)
        int saves_listed_at = -2;
        steady::time_point hover_since{};
        const std::string politics_iso = opt.politics;
        bool politics_iso_done = false;
        float drag_x = 0, drag_y = 0;
        double key_pan_x = 0, key_pan_y = 0, key_zoom = 1.0;
        double cell_radians = 2.0 * map_pi / 8192.0;
        // Clouds: advected on their own clock, which follows the simulation (frozen when
        // paused) at most at 2 days per second so they stay readable at high speeds.
        bool clouds_on = opt.clouds;
        double cloud_days = 0.0;
        auto last_frame = steady::now();
        std::vector<map_storm> storms;
        std::vector<map_quake> quakes;
        hazard_feed hazards; // cyclones, earthquakes and disaster alerts from the sim
        bool demo_alerts = false;

        // The main menu first (unless an automated / --nation run goes straight in).
        enum class app_phase : uint8_t { menu, game };
        app_phase phase = opt.menu ? app_phase::menu : app_phase::game;
        menu_state menu;
        client_settings settings = load_settings(); // the saved values (the command line only overrides this run)
        std::vector<save_entry> saves;
        if(settings.fullscreen) SDL_SetWindowFullscreen(window, true);
        std::string session_name = opt.scenario;

        audio::music_system music;
        if(phase == app_phase::menu) { // menu music: wartime, led by an arp solo (docs/14-audio.md#main-menu)
            music.set_menu(true);
            music.set_mood(audio::mood::wartime, 0.8f);
        }
        if(opt.music) music.start(opt.music_seed, opt.music_volume);
        music_mood last_mood{};
        int music_override = -1; // N: -1 = the game's mood, else a mood index (docs/14-audio.md#mood-mapping)
        steady::time_point last_mood_check{};

        if(phase == app_phase::game) sim.start(opt.assets, opt.scenario, opt.nation);
        sim.set_speed(opt.speed);
        // The Control window's list ('+'), refreshed every few seconds while open.
        std::vector<agent_info> agent_list;
        std::future<std::vector<agent_info>> pending_agents;
        steady::time_point last_agents{};
        bool text_input = false;
        std::future<std::string> pending_save;
        std::string pending_save_path;
        ui_context ui(text);
        alert_feed alerts;
        hud_state hud;
        if(!opt.politics.empty()) {
            hud.open = hud_action::politics;
            hud.politics_mode = opt.politics_mode;
        }
        {
            static constexpr char const* view_names[] = {"political", "climate", "economy", "trade", "energy", "epidemic", "unrest"};
            static_assert(std::size(view_names) == static_cast<std::size_t>(map_filter::count));
            for(std::size_t i = 0; i < std::size(view_names); ++i) {
                if(opt.view == view_names[i]) hud.filter = static_cast<map_filter>(i);
            }
        }
        if(phase == app_phase::game) alerts.push_local("⏳", "Loading scenario " + opt.scenario, "Building the world from assets/", tone::neutral);
        bool announced = false;
        int last_speed = opt.speed > 0 ? opt.speed : 1;

        ui_input input;
        bool running = true, minimized = false, swapchain_dirty = false;
        const auto start = steady::now();
        int frames = 0;
        bool capture_next = false, captured = false;

        while(running) {
            // --- events -> input ------------------------------------------------
            input.pressed = input.released = false;
            input.wheel = 0;
            if(!opt.inject.empty()) inject_events(window, opt.inject, frames);
            SDL_Event ev;
            while(SDL_PollEvent(&ev)) {
                switch(ev.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running = false; break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_RESIZED: swapchain_dirty = true; break;
                case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                    metrics = query_metrics(window);
                    text.clear_cache(); // glyph sizes change with the scale
                    swapchain_dirty = true;
                    break;
                case SDL_EVENT_WINDOW_MINIMIZED: minimized = true; break;
                case SDL_EVENT_WINDOW_RESTORED:
                case SDL_EVENT_WINDOW_SHOWN: minimized = false; swapchain_dirty = true; break;
                case SDL_EVENT_WINDOW_MOUSE_LEAVE: input.mouse_x = input.mouse_y = -1; break;
                case SDL_EVENT_MOUSE_MOTION:
                    input.mouse_x = ev.motion.x * metrics.pixel_density;
                    input.mouse_y = ev.motion.y * metrics.pixel_density;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if(ev.button.button == SDL_BUTTON_LEFT) {
                        const bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                        input.mouse_x = ev.button.x * metrics.pixel_density;
                        input.mouse_y = ev.button.y * metrics.pixel_density;
                        input.pressed |= down;
                        input.released |= !down;
                        input.mouse_down = down;
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    input.wheel += ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev.wheel.y : ev.wheel.y;
                    break;
                case SDL_EVENT_TEXT_INPUT:
                    if(phase == app_phase::game && hud.open == hud_action::control && hud.search.size() < 64) hud.search += ev.text.text;
                    break;
                case SDL_EVENT_KEY_DOWN: {
                    const SDL_Keycode k = ev.key.key;
                    // Typing into the Control window's search field.
                    if(phase == app_phase::game && hud.open == hud_action::control && !(ev.key.mod & SDL_KMOD_CTRL)) {
                        if(k == SDLK_BACKSPACE && !hud.search.empty()) {
                            do hud.search.pop_back();
                            while(!hud.search.empty() && (static_cast<unsigned char>(hud.search.back()) & 0xC0) == 0x80); // whole UTF-8 characters
                        } else if(k == SDLK_ESCAPE) {
                            hud.open = hud_action::none;
                        }
                        break;
                    }
                    if(ev.key.repeat) break;
                    if(phase == app_phase::menu) {
                        if(k == SDLK_ESCAPE) menu.open = menu_state::window::none;
                        if(k == SDLK_Q && (ev.key.mod & SDL_KMOD_CTRL)) running = false;
                        break;
                    }
                    if(k == SDLK_ESCAPE) {
                        hud.open = hud_action::none;
                    } else if(k == SDLK_S && (ev.key.mod & SDL_KMOD_CTRL) && sim.status() == sim_status::running && !pending_save.valid()) {
                        pending_save_path = new_save_path(sim.scenario());
                        pending_save = sim.request_save(pending_save_path);
                    } else if(k == SDLK_SPACE || k == SDLK_P || k == SDLK_0) {
                        const int s = sim.speed();
                        if(s > 0) last_speed = s;
                        sim.set_speed(s > 0 || k == SDLK_0 ? 0 : last_speed);
                    } else if(k >= SDLK_1 && k <= SDLK_5) {
                        sim.set_speed(static_cast<int>(k - SDLK_0));
                    } else if(k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) {
                        sim.set_speed(std::min(max_speed, sim.speed() + 1));
                    } else if(k == SDLK_MINUS || k == SDLK_KP_MINUS) {
                        sim.set_speed(std::max(0, sim.speed() - 1));
                    } else if(k == SDLK_Q && (ev.key.mod & SDL_KMOD_CTRL)) {
                        running = false;
                    } else if(k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_UP || k == SDLK_DOWN) {
                        const double step = 0.15 * metrics.pixel_w;
                        key_pan_x += k == SDLK_LEFT ? step : k == SDLK_RIGHT ? -step : 0.0;
                        key_pan_y += k == SDLK_UP ? step : k == SDLK_DOWN ? -step : 0.0;
                    } else if(k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
                        key_zoom *= k == SDLK_PAGEUP ? 1.5 : 1.0 / 1.5;
                    } else if(k == SDLK_C && !(ev.key.mod & SDL_KMOD_CTRL)) {
                        clouds_on = !clouds_on;
                        SDL_Log("clouds %s", clouds_on ? "on" : "off");
                    } else if(k == SDLK_M && music.running()) {
                        music.set_muted(!music.muted());
                        SDL_Log("music %s", music.muted() ? "muted" : "on");
                    } else if(k == SDLK_B && music.running()) {
                        music.skip();
                        SDL_Log("music: skip");
                        alerts.push_local("♪", "Music: next song", "", tone::neutral);
                    } else if(k == SDLK_N && music.running()) {
                        // cycle: auto (the game's mood) -> calm -> ... -> wartime -> auto; a change skips to a new song
                        music_override = music_override + 1 >= audio::mood_count ? -1 : music_override + 1;
                        const music_mood mm = music_override < 0 ? derive_music_mood(sim, alerts) : music_mood{static_cast<audio::mood>(music_override), audio::default_intensity(static_cast<audio::mood>(music_override))};
                        music.set_mood(mm.m, mm.intensity);
                        music.skip();
                        last_mood = mm;
                        SDL_Log("music: mood %s%s", music_override < 0 ? "auto: " : "override: ", audio::mood_name(mm.m));
                        alerts.push_local("♪", std::string("Music: ") + (music_override < 0 ? "auto (" + std::string(audio::mood_name(mm.m)) + ")" : audio::mood_name(mm.m)), "N cycles the mood, B skips", tone::neutral);
                    }
                    break;
                }
                default: break;
                }
            }
            if(!running) break;
            if(minimized) {
                SDL_WaitEventTimeout(nullptr, 100);
                continue;
            }
            if(swapchain_dirty) {
                metrics = query_metrics(window);
                if(metrics.pixel_w <= 0 || metrics.pixel_h <= 0) {
                    SDL_WaitEventTimeout(nullptr, 100);
                    continue;
                }
                if(!vk.create_swapchain(static_cast<uint32_t>(metrics.pixel_w), static_cast<uint32_t>(metrics.pixel_h), diag) || !renderer.resize(diag)) {
                    exit_code = fail("swapchain recreation", diag.error());
                    break;
                }
                swapchain_dirty = false;
            }

            // --- simulation status -> alerts -----------------------------------
            if(phase == app_phase::game && !announced && sim.status() != sim_status::loading) {
                announced = true;
                alerts.dismiss_all();
                if(sim.status() == sim_status::failed) {
                    alerts.push_local("⚠", "Simulation unavailable", sim.error().substr(0, 60), tone::bad);
                } else {
                    alerts.push_local("🌍", "Session " + session_name + " ready",
                                      sim.controlled_agents().empty() ? "Press + (top left) to control an agent" : "Press Space or pick a speed to start", tone::good);
                }
            }
            alerts.poll(sim);
            if(!map_loaded && map_load.ready()) {
                map_loaded = true;
                std::unique_ptr<map_data> data = map_load.take();
                if(data && map.upload(*data, diag)) {
                    political.init(*data, opt.assets);
                    map.set_table(political.table());
                    cell_radians = 2.0 * map_pi / static_cast<double>(data->ids[0].width);
                    double lon = 0, lat = 0, span = 75.0;
                    if(parse_map_view(opt.map_view, lon, lat, span)) {
                        view.set_focus(lon * map_deg, lat * map_deg);
                    } else {
                        const std::string first = opt.nation.substr(0, opt.nation.find(','));
                        const int n = data->find_nation(first);
                        if(n >= 0) view.set_focus(data->nations[static_cast<std::size_t>(n)].lon * map_deg, data->nations[static_cast<std::size_t>(n)].lat * map_deg);
                    }
                    view.scale = static_cast<double>(metrics.pixel_w) / (span * map_deg);
                    view.fit_limits(metrics.pixel_w, cell_radians);
                    SDL_Log("map: %zu regions, %zu nations, %u x %u cells", data->regions.size(), data->nations.size(), data->ids[0].width, data->ids[0].height);
                } else {
                    const std::string err = data ? diag.error() : map_load.error();
                    SDL_Log("map unavailable: %s", err.c_str());
                    alerts.push_local("🗺", "World map unavailable", err.substr(0, 60), tone::bad);
                    if(data) diag = core::diagnostics{}; // not fatal: the placeholder stays
                }
            }
            if(map.ready() && political.update(sim, hazards, phase == app_phase::menu ? map_filter::political : hud.filter)) {
                map.set_table(political.table());
            }
            if(pending_save.valid() && pending_save.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const std::string err = pending_save.get();
                if(err.empty()) alerts.push_local("💾", "Session saved", pending_save_path, tone::good);
                else alerts.push_local("⚠", "Save failed", err.substr(0, 60), tone::bad);
            }
            if(phase == app_phase::game && music.running() && music_override < 0 && steady::now() - last_mood_check > std::chrono::seconds(2)) {
                last_mood_check = steady::now();
                const music_mood mm = derive_music_mood(sim, alerts);
                if(mm.m != last_mood.m) SDL_Log("music mood: %s (intensity %.2f)", audio::mood_name(mm.m), static_cast<double>(mm.intensity));
                last_mood = mm;
                music.set_mood(mm.m, mm.intensity);
            }

            const bool want_text = phase == app_phase::game && hud.open == hud_action::control;
            if(want_text != text_input) {
                if(want_text) SDL_StartTextInput(window);
                else SDL_StopTextInput(window);
                text_input = want_text;
            }
            // --- layout ---------------------------------------------------------
            const float scale = metrics.display_scale * opt.ui_scale;
            const auto fw = static_cast<float>(vk.swap_extent.width), fh = static_cast<float>(vk.swap_extent.height);
            ui.begin(input, scale, fw, fh);
            // The map fills the window behind everything (docs/07); a placeholder until it is loaded.
            if(!map.ready()) ui.scene.push(material::map_placeholder, {0, 0, fw, fh, 0, 0, 1, 1, 0xFFFFFFFFu, 0, 0, 0});
            if(phase == app_phase::menu) {
                // The menu over the world: the camera pans east along the equator, zoomed out.
                if(map.ready()) {
                    view.fit_limits(fw, cell_radians);
                    view.scale = std::max(view.min_scale, static_cast<double>(fw) / (200.0 * map_deg));
                    view.set_focus(std::remainder(menu_lon, 360.0) * map_deg, 0.0);
                }
                const menu_choice choice = build_menu(ui, menu, settings, saves);
                if(menu.open == menu_state::window::load && saves_listed_at != frames - 1) saves = list_saves();
                saves_listed_at = frames;
                if(choice.what == menu_choice::kind::quit) running = false;
                if(choice.what == menu_choice::kind::settings_changed) {
                    save_settings(settings);
                    music.set_volume(settings.music_volume);
                    music.set_muted(!settings.music);
                    clouds_on = settings.clouds;
                    SDL_SetWindowFullscreen(window, settings.fullscreen);
                }
                if(choice.what == menu_choice::kind::new_session || choice.what == menu_choice::kind::load) {
                    // Into the paused game, controlling no one ('+' adds agents).
                    session_name = choice.what == menu_choice::kind::load ? save_scenario(std::filesystem::path(choice.path).stem().string()) : std::string("world2020");
                    sim.start(opt.assets, session_name, "", choice.what == menu_choice::kind::load ? choice.path : std::string{});
                    sim.set_speed(0);
                    phase = app_phase::game;
                    announced = false;
                    alerts.dismiss_all();
                    alerts.push_local("⏳", choice.what == menu_choice::kind::load ? "Loading session" : "Loading scenario " + session_name,
                                      "Building the world from assets/", tone::neutral);
                    view.scale = static_cast<double>(fw) / (75.0 * map_deg);
                    music.set_menu(false);
                    if(music.running()) music.skip(); // back to the game's mood logic
                }
            } else {
                map_tab_info tab_info;
                if(map.ready() && hud.filter != map_filter::political) tab_info = political.tab(hud.filter, sim, hazards);
                // Politics tab (🗳): the electoral tile map of one nation.
                electoral_view_info politics_info;
                const bool politics_open = map.ready() && hud.open == hud_action::politics && sim.status() == sim_status::running;
                if(politics_open) {
                    if(!politics_iso_done && !politics_iso.empty() && politics_iso != "-") { // --politics ISO3
                        const int n = political.nation_index(politics_iso);
                        if(n >= 0) {
                            hud.politics_nation = n;
                            politics_iso_done = true;
                        }
                    }
                    hud.politics_nation = political.resolve_nation(hud.politics_nation, 0, sim);
                    political.update_politics(sim, hud.politics_nation);
                    politics_info = political.electoral(hud.politics_nation, hud.politics_mode, sim);
                }
                if(hud.open == hud_action::control && sim.status() == sim_status::running) {
                    if(pending_agents.valid() && pending_agents.wait_for(std::chrono::seconds(0)) == std::future_status::ready) agent_list = pending_agents.get();
                    if(!pending_agents.valid() && (agent_list.empty() || steady::now() - last_agents > std::chrono::seconds(3))) {
                        pending_agents = sim.request_agents();
                        last_agents = steady::now();
                    }
                }
                build_hud(ui, sim, alerts, hud, map.ready() ? &tab_info : nullptr, politics_open ? &politics_info : nullptr,
                          agent_list.empty() ? nullptr : &agent_list);
                if(politics_open && hud.politics_step != 0) {
                    hud.politics_nation = political.resolve_nation(hud.politics_nation, hud.politics_step, sim);
                    hud.politics_mode = 0;
                }
                const bool on_tooltip = hud.tooltip_rect.contains(input.mouse_x, input.mouse_y);
                const bool on_ui = ui.wants_mouse() || on_tooltip;
                // Map navigation: drag pans (started outside the panels), the wheel zooms
                // at the pointer, arrows / Page Up / Page Down from the keyboard.
                if(map.ready()) {
                    view.fit_limits(fw, cell_radians);
                    if(input.pressed && !on_ui && input.mouse_x >= 0) {
                        dragging = true;
                        drag_x = input.mouse_x;
                        drag_y = input.mouse_y;
                        press_x = input.mouse_x;
                        press_y = input.mouse_y;
                    }
                    // A click (no drag) on the map while the Politics tab is open shows that nation.
                    if(input.released && dragging && politics_open && std::fabs(input.mouse_x - press_x) < 4 && std::fabs(input.mouse_y - press_y) < 4) {
                        double lon = 0, lat = 0;
                        view.to_geo(input.mouse_x - fw * 0.5, input.mouse_y - fh * 0.5, lon, lat);
                        const int n = political.nation_at(political.pick(lon / map_deg, lat / map_deg));
                        if(n >= 0 && n != hud.politics_nation) {
                            hud.politics_nation = n;
                            hud.politics_mode = 0;
                        }
                    }
                    if(dragging && (input.mouse_x != drag_x || input.mouse_y != drag_y) && input.mouse_x >= 0) {
                        view.pan(input.mouse_x - drag_x, input.mouse_y - drag_y);
                        drag_x = input.mouse_x;
                        drag_y = input.mouse_y;
                    }
                    if(!input.mouse_down) dragging = false;
                    if(!on_ui) {
                        const float steps = ui.take_wheel({0, 0, fw, fh});
                        if(steps != 0) view.zoom_at(std::pow(1.25, static_cast<double>(steps)), input.mouse_x - fw * 0.5, input.mouse_y - fh * 0.5);
                    }
                    if(key_pan_x != 0 || key_pan_y != 0) view.pan(key_pan_x, key_pan_y);
                    if(key_zoom != 1.0) view.zoom_at(key_zoom, 0, 0);
                    // Hover: the region (or electoral tile) under the pointer gets a compact
                    // tooltip; keeping the pointer still (within a few pixels) for 0.75 s of real
                    // time expands it into the detail panel, which stays open while the pointer
                    // is on it. Another region collapses it and restarts the timer.
                    int candidate = -1;
                    bool candidate_tile = false;
                    if(hud.politics_tile >= 0) {
                        candidate = hud.politics_tile;
                        candidate_tile = true;
                    } else if(!on_ui && !dragging && input.mouse_x >= 0) {
                        double lon = 0, lat = 0;
                        view.to_geo(input.mouse_x - fw * 0.5, input.mouse_y - fh * 0.5, lon, lat);
                        candidate = political.pick(lon / map_deg, lat / map_deg);
                    }
                    const auto hover_now = steady::now();
                    if(on_tooltip && hud.tooltip_region >= 0) {
                        hud.tooltip_expanded = true; // reading the panel
                        hud.tooltip_progress = 1.0f;
                    } else if(candidate >= 0) {
                        const bool same = candidate == hud.tooltip_region && candidate_tile == hud.tooltip_tile;
                        const bool still = std::fabs(input.mouse_x - hud.tooltip_x) < 4.0f && std::fabs(input.mouse_y - hud.tooltip_y) < 4.0f;
                        if(!same || (!still && !hud.tooltip_expanded) || (hud.tooltip_expanded && !same)) {
                            hud.tooltip_region = candidate;
                            hud.tooltip_tile = candidate_tile;
                            hud.tooltip_x = input.mouse_x;
                            hud.tooltip_y = input.mouse_y;
                            hud.tooltip_expanded = false;
                            hover_since = hover_now;
                        }
                        if(!hud.tooltip_expanded) {
                            hud.tooltip_progress = static_cast<float>(std::chrono::duration<double>(hover_now - hover_since).count() / 0.75); // expands after 0.75 s
                            if(hud.tooltip_progress >= 1.0f) hud.tooltip_expanded = true;
                        }
                    } else {
                        hud.tooltip_region = -1;
                        hud.tooltip_expanded = false;
                    }
                    if(hud.tooltip_region >= 0) {
                        draw_map_tooltip(ui, hud.tooltip_tile ? political.describe_tile(hud.tooltip_region, hud.politics_mode, sim)
                                                              : political.describe(hud.tooltip_region, hud.filter, sim),
                                         hud);
                    } else {
                        hud.tooltip_rect = {};
                    }
                }
                // Agent bars and the Control window.
                if(hud.toggle_ai >= 0) {
                    const auto agent = static_cast<uint32_t>(hud.toggle_ai);
                    sim.set_ai(agent, !sim.ai_on(agent));
                }
                if(hud.remove_agent >= 0) sim.remove_control(static_cast<uint32_t>(hud.remove_agent));
                if(hud.add_agent >= 0 && static_cast<std::size_t>(hud.add_agent) < agent_list.size()) sim.add_control(agent_list[static_cast<std::size_t>(hud.add_agent)]);
                if(sim.status() == sim_status::loading && sim.started()) draw_notice(ui, "Loading", "Building the world… (a few seconds)");
            }
            key_pan_x = key_pan_y = 0;
            key_zoom = 1.0;
            ui.end();
            if(hud.requested_speed >= 0) {
                if(sim.speed() > 0) last_speed = sim.speed();
                sim.set_speed(hud.requested_speed);
                SDL_Log("time controls: speed %d", hud.requested_speed);
            }

            // --- automated runs ------------------------------------------------
            const double elapsed = std::chrono::duration<double>(steady::now() - start).count();
            const bool sim_settled = phase == app_phase::menu
                                     || (sim.status() != sim_status::loading && (sim.status() == sim_status::failed || sim.tick() >= opt.until_day));
            const bool done = opt.frames > 0 && frames >= opt.frames && ((sim_settled && map_loaded) || elapsed > 120.0);
            if(done && !opt.screenshot.empty() && !captured && !capture_next) {
                capture_next = true; // capture this frame, exit after it
            }

            // --- render ---------------------------------------------------------
            const auto acq = vk.begin_frame(diag);
            if(acq == acquire_status::out_of_date) {
                swapchain_dirty = true;
                continue;
            }
            if(acq == acquire_status::failed) {
                exit_code = fail("frame acquire", diag.error());
                break;
            }
            const auto frame_now = steady::now();
            const double frame_dt = std::min(0.25, std::chrono::duration<double>(frame_now - last_frame).count());
            last_frame = frame_now;
            if(sim.status() == sim_status::running && sim.speed() > 0) {
                const double rate = sim.speed() < max_speed ? days_per_second[sim.speed()] : sim.measured_days_per_second();
                cloud_days += frame_dt * std::min(rate, 2.0);
            }
            hazards.poll(sim, alerts, static_cast<float>(elapsed));
            if(opt.hazard_demo && !demo_alerts && sim.status() == sim_status::running) {
                demo_alerts = true;
                disaster_note warn;
                warn.key = 0xDE30;
                warn.kind = 0;
                warn.title = "Hurricane Laura";
                warn.place = "Louisiana, United States";
                warn.date = format_long_date(sim.date());
                warn.deaths = 40;
                warn.affected = 1.2e6f;
                warn.damage_usd = 1.9e10;
                warn.gdp_loss_pct = 0.09f;
                warn.potential = true;
                alerts.upsert(hazard_feed::make_alert(warn, true));
                disaster_note quake = warn;
                quake.key = 0xDE31;
                quake.kind = 3;
                quake.title = "Magnitude 9.0 earthquake";
                quake.place = "Miyagi, Japan";
                quake.deaths = 18500;
                quake.affected = 4.5e5f;
                quake.damage_usd = 2.35e11;
                quake.gdp_loss_pct = 4.1f;
                quake.potential = false;
                alerts.upsert(hazard_feed::make_alert(quake, false));
            }
            if(opt.hazard_demo) {
                demo_hazards(cloud_days, static_cast<float>(elapsed), storms, quakes);
                map.set_hazards(storms, quakes);
            } else {
                map.set_hazards(hazards.storms(), hazards.quakes());
            }
            map_anim anim;
            anim.cloud_days = static_cast<float>(std::fmod(cloud_days, 3000.0));
            anim.seconds = static_cast<float>(elapsed);
            // Clouds and cyclones: the climate view only (and behind the main menu).
            anim.cloud_strength = clouds_on && (phase == app_phase::menu || hud.filter == map_filter::climate) ? 0.5f : 0.0f;
            if(phase == app_phase::menu) {
                menu_lon += frame_dt * 4.0; // degrees per second
                cloud_days += frame_dt * 0.5;
            }
            if(sim.status() == sim_status::running) {
                const core::civil_date today = sim.date();
                anim.day_of_year = static_cast<float>(core::days_from_civil(today) - core::days_from_civil({today.year, 1, 1}) + 1);
            }
            const map_pass map_draw{&map, &view, true, anim}; // every view fills regions (map_layer)
            if(!renderer.record(text, ui.scene, ui.overlay, static_cast<float>(elapsed), scale, capture_next, diag, map_draw)) {
                exit_code = fail("frame recording", diag.error());
                vk.end_frame(diag);
                break;
            }
            const auto pres = vk.end_frame(diag);
            if(pres == acquire_status::failed) {
                exit_code = fail("present", diag.error());
                break;
            }
            if(pres == acquire_status::out_of_date) swapchain_dirty = true;
            ++frames;
            if(capture_next) {
                capture_next = false;
                captured = true;
                if(renderer.save_capture(opt.screenshot, diag)) {
                    SDL_Log("wrote %s", opt.screenshot.c_str());
                } else {
                    SDL_Log("screenshot failed: %s", diag.error().c_str());
                }
            }
            if(done && (opt.screenshot.empty() || captured)) running = false;
        }

        const double secs = std::chrono::duration<double>(steady::now() - start).count();
        SDL_Log("frames: %d in %.2f s (%.1f fps); sim tick %d (%s); validation messages: %u (errors %u)", frames, secs,
                secs > 0 ? frames / secs : 0.0, sim.tick(), sim.status() == sim_status::running ? "running" : sim.status() == sim_status::failed ? "failed" : "loading",
                vk.validation_messages, vk.validation_errors);
        if(exit_code == 0 && vk.validation_errors > 0 && opt.frames > 0) exit_code = 3;
        music.stop();
        sim.stop();
        map.shutdown();
        renderer.shutdown();
        text.shutdown();
        vk.shutdown();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}

} // namespace geosim::client
