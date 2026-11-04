#pragma once
// The client application: SDL3 window and event loop, frame orchestration
// (input -> HUD layout -> Vulkan frame), and the simulation thread.

#include <cstdint>
#include <string>

namespace geosim::client {

struct app_options {
    std::string assets = "assets";
    std::string scenario = "2020";
    std::string nation = "USA"; // the player's nations, ISO3, comma-separated ("USA,CAN"); their executives and central banks are the controlled agents
    int width = 1600, height = 900; // window size in points
    float ui_scale = 1.0f;          // multiplied with the display scale
    int speed = 0;                  // initial speed 0..5
    bool validation = false;        // VK_LAYER_KHRONOS_validation (also GEOSIM_VK_VALIDATION=1)
    // Automated runs (CI / headless checks): stop after `frames` frames (0 = run
    // until closed), but not before the sim reached `until_day` (or 120 s passed).
    int frames = 0;
    int until_day = 0;
    std::string screenshot;         // write the last frame as PPM
    // Scripted input for headless checks: "frame:kind:args;..." with kinds
    // key:NAME (SDL key name), move:X,Y, click:X,Y, wheel:X,Y,DY, resize:W,H
    // (window coordinates). Example: "10:key:3;20:click:220,24".
    std::string inject;
    // Initial map view "lon,lat,width": focus in degrees and the span of the
    // window width in degrees of arc (default: the first --nation, ~75 degrees).
    std::string map_view;
    bool clouds = true;       // cloud layer (C toggles it at run time)
    bool hazard_demo = false; // fixed demo storms and an earthquake (visual checks before the sim feeds them)
    // Procedural music (docs/14-audio.md); M toggles it at run time.
    bool music = true;
    float music_volume = 0.6f;
    uint64_t music_seed = 0; // 0 = random
};

// Returns the process exit code.
int run_app(app_options const& opt);

} // namespace geosim::client
