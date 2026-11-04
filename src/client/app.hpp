#pragma once
// The client application: SDL3 window and event loop, frame orchestration
// (input -> HUD layout -> Vulkan frame), and the simulation thread.

#include <string>

namespace geosim::client {

struct app_options {
    std::string assets = "assets";
    std::string scenario = "2020";
    std::string nation = "USA"; // the player's nation (its executive and central bank are the controlled agents)
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
};

// Returns the process exit code.
int run_app(app_options const& opt);

} // namespace geosim::client
