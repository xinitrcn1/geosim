#include "app.hpp"

#include "audio/music.hpp"
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
#include <cstring>

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
        sim_runner sim;
        window_metrics metrics = query_metrics(window);

        if(!vk.init(window, validation, diag)) {
            SDL_DestroyWindow(window);
            SDL_Quit();
            return fail("Vulkan initialisation", diag.error());
        }
        SDL_Log("Vulkan device: %s", vk.device_name.c_str());
        if(!text.init(opt.assets + "/fonts", diag) || !vk.create_swapchain(static_cast<uint32_t>(metrics.pixel_w), static_cast<uint32_t>(metrics.pixel_h), diag)
           || !renderer.init(vk, diag)) {
            renderer.shutdown();
            vk.shutdown();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return fail("renderer initialisation", diag.error());
        }
        SDL_Log("swapchain %ux%u, emoji flag ligatures: %zu", vk.swap_extent.width, vk.swap_extent.height, text.flag_ligatures());

        audio::music_system music;
        if(opt.music) music.start(opt.music_seed, opt.music_volume);
        music_mood last_mood{};
        steady::time_point last_mood_check{};

        sim.start(opt.assets, opt.scenario, opt.nation);
        sim.set_speed(opt.speed);
        ui_context ui(text);
        alert_feed alerts;
        hud_state hud;
        alerts.push_local("⏳", "Loading scenario " + opt.scenario, "Building the world from assets/", tone::neutral);
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
                case SDL_EVENT_KEY_DOWN: {
                    if(ev.key.repeat) break;
                    const SDL_Keycode k = ev.key.key;
                    if(k == SDLK_SPACE || k == SDLK_P || k == SDLK_0) {
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
                    } else if(k == SDLK_M && music.running()) {
                        music.set_muted(!music.muted());
                        SDL_Log("music %s", music.muted() ? "muted" : "on");
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
            if(!announced && sim.status() != sim_status::loading) {
                announced = true;
                alerts.dismiss_all();
                if(sim.status() == sim_status::failed) {
                    alerts.push_local("⚠", "Simulation unavailable", sim.error().substr(0, 60), tone::bad);
                } else {
                    alerts.push_local("🌍", "Scenario " + opt.scenario + " ready", "Press Space or pick a speed to start", tone::good);
                }
            }
            alerts.poll(sim);
            if(music.running() && steady::now() - last_mood_check > std::chrono::seconds(2)) {
                last_mood_check = steady::now();
                const music_mood mm = derive_music_mood(sim, alerts);
                if(mm.m != last_mood.m) SDL_Log("music mood: %s (intensity %.2f)", audio::mood_name(mm.m), static_cast<double>(mm.intensity));
                last_mood = mm;
                music.set_mood(mm.m, mm.intensity);
            }

            // --- layout ---------------------------------------------------------
            const float scale = metrics.display_scale * opt.ui_scale;
            const auto fw = static_cast<float>(vk.swap_extent.width), fh = static_cast<float>(vk.swap_extent.height);
            ui.begin(input, scale, fw, fh);
            // The map placeholder fills the window behind everything (docs/07).
            ui.scene.push(material::map_placeholder, {0, 0, fw, fh, 0, 0, 1, 1, 0xFFFFFFFFu, 0, 0, 0});
            build_hud(ui, sim, alerts, hud);
            ui.end();
            if(hud.requested_speed >= 0) {
                if(sim.speed() > 0) last_speed = sim.speed();
                sim.set_speed(hud.requested_speed);
                SDL_Log("time controls: speed %d", hud.requested_speed);
            }

            // --- automated runs ------------------------------------------------
            const double elapsed = std::chrono::duration<double>(steady::now() - start).count();
            const bool sim_settled = sim.status() != sim_status::loading && (sim.status() == sim_status::failed || sim.tick() >= opt.until_day);
            const bool done = opt.frames > 0 && frames >= opt.frames && (sim_settled || elapsed > 120.0);
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
            if(!renderer.record(text, ui.scene, ui.overlay, static_cast<float>(elapsed), scale, capture_next, diag)) {
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
        renderer.shutdown();
        text.shutdown();
        vk.shutdown();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}

} // namespace geosim::client
