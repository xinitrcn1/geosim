// geosim: the game client (SDL3 + Vulkan 1.3). Built with -DGEOSIM_BUILD_CLIENT=ON.
// docs/08-ui.md (UI), docs/09-engine-architecture.md (architecture).
//
//   geosim [--assets DIR] [--scenario 2020] [--nation USA[,CAN...]] [--speed 0..5]
//          [--width W --height H] [--ui-scale S] [--validation 1]
//          [--frames N [--until-day D] [--screenshot OUT.ppm]] [--inject SCRIPT]
//          [--music 0|1] [--music-volume 0..1] [--music-seed N]
//   geosim --render-music OUT.wav [--seed N] [--mood calm|tense|crisis|triumph|wartime]
//          [--intensity 0..1] [--songs N] [--switch SECONDS:MOOD] [--solo CHANNEL]
//            renders procedural music offline, no window or audio device
//            (docs/14-audio.md)
//
// Headless check (no display): SDL_VIDEODRIVER=offscreen geosim --frames 120 --screenshot out.ppm

#include "app.hpp"
#include "audio/render.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int usage() {
    std::fputs("usage: geosim [--assets DIR] [--scenario NAME] [--nation ISO3[,ISO3...]] [--speed 0..5] [--width W] [--height H]\n"
               "              [--ui-scale S] [--validation 1] [--frames N] [--until-day D] [--screenshot OUT.ppm]\n"
               "              [--inject \"FRAME:key:NAME;FRAME:click:X,Y;FRAME:move:X,Y;FRAME:wheel:X,Y,DY;FRAME:resize:W,H\"]\n"
               "              [--music 0|1] [--music-volume 0..1] [--music-seed N]\n"
               "       geosim --render-music OUT.wav [--seed N] [--mood calm|tense|crisis|triumph|wartime] [--intensity 0..1]\n"
               "              [--songs N] [--switch SECONDS:MOOD] [--solo kick|snare|hats|bass|lead|arp|pad|fx]\n",
               stderr);
    return 2;
}

bool has_assets(std::string const& dir) {
    std::FILE* f = std::fopen((dir + "/fonts/SourceSans3-Variable.ttf").c_str(), "rb");
    if(f) std::fclose(f);
    return f != nullptr;
}

// Without --assets: ./assets, else the repository's assets/ found from the
// executable's directory (build/<preset>/src/geosim -> ../../../assets).
std::string find_assets() {
    if(has_assets("assets")) return "assets";
    char const* base = SDL_GetBasePath();
    if(base) {
        std::string dir = base;
        for(int up = 0; up < 5; ++up) {
            if(has_assets(dir + "assets")) return dir + "assets";
            dir += "../";
        }
    }
    return "assets";
}

template<typename T>
bool parse(char const* text, T& out) {
    auto const* end = text + std::strlen(text);
    auto r = std::from_chars(text, end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

} // namespace

int main(int argc, char** argv) {
    geosim::client::app_options opt;
    geosim::client::audio::render_options render;
    bool assets_given = false;
    for(int i = 1; i < argc; i += 2) {
        if(i + 1 >= argc || std::strncmp(argv[i], "--", 2) != 0) return usage();
        const std::string key = argv[i] + 2;
        char const* v = argv[i + 1];
        bool ok = true;
        if(key == "assets") {
            opt.assets = v;
            assets_given = true;
        }
        else if(key == "scenario") opt.scenario = v;
        else if(key == "nation") opt.nation = v;
        else if(key == "screenshot") opt.screenshot = v;
        else if(key == "inject") opt.inject = v;
        else if(key == "speed") ok = parse(v, opt.speed) && opt.speed >= 0 && opt.speed <= 5;
        else if(key == "width") ok = parse(v, opt.width) && opt.width > 0;
        else if(key == "height") ok = parse(v, opt.height) && opt.height > 0;
        else if(key == "ui-scale") ok = parse(v, opt.ui_scale) && opt.ui_scale > 0.25f && opt.ui_scale < 8.0f;
        else if(key == "frames") ok = parse(v, opt.frames) && opt.frames >= 0;
        else if(key == "until-day") ok = parse(v, opt.until_day) && opt.until_day >= 0;
        else if(key == "validation") opt.validation = std::strcmp(v, "0") != 0;
        else if(key == "music") opt.music = std::strcmp(v, "0") != 0;
        else if(key == "music-volume") ok = parse(v, opt.music_volume) && opt.music_volume >= 0.0f && opt.music_volume <= 1.0f;
        else if(key == "music-seed") ok = parse(v, opt.music_seed);
        else if(key == "render-music") render.path = v;
        else if(key == "seed") ok = parse(v, render.seed);
        else if(key == "mood") ok = geosim::client::audio::parse_mood(v, render.m);
        else if(key == "intensity") ok = parse(v, render.intensity) && render.intensity >= 0.0f && render.intensity <= 1.0f;
        else if(key == "solo") {
            render.solo = -1;
            for(int c = 0; c < geosim::client::audio::num_channels; ++c) {
                if(std::strcmp(v, geosim::client::audio::channel_name(c)) == 0) render.solo = c;
            }
            ok = render.solo >= 0;
        }
        else if(key == "songs") ok = parse(v, render.songs) && render.songs >= 1 && render.songs <= 20;
        else if(key == "switch") {
            char mood_text[16] = {};
            ok = std::sscanf(v, "%lf:%15s", &render.switch_at, mood_text) == 2 && geosim::client::audio::parse_mood(mood_text, render.switch_to);
        }
        else ok = false;
        if(!ok) return usage();
    }
    if(!render.path.empty()) {
        geosim::core::diagnostics diag;
        geosim::client::audio::render_report report;
        const bool ok = geosim::client::audio::render_music(render, report, diag);
        if(!ok) {
            std::fprintf(stderr, "render-music: %s\n", diag.error().c_str());
            return 1;
        }
        geosim::client::audio::print_report(report);
        std::printf("wrote %s\n", render.path.c_str());
        return report.non_finite == 0 && report.clipped == 0 ? 0 : 1;
    }
    if(!assets_given) opt.assets = find_assets();
    return geosim::client::run_app(opt);
}
