#pragma once
// Offline rendering (docs/14-audio.md#offline-rendering): composes and
// plays songs through the same jukebox/player as the game, faster than real
// time, without an audio device, and writes a 16-bit stereo WAV. Also
// measures the result: duration, peak, RMS, NaNs, and an independent check
// of the tempo grid (low-band onset detection vs the 16th-note grid, and an
// autocorrelation tempo estimate).
//
//   geosim --render-music out.wav [--seed 7] [--mood calm] [--intensity 0.5]
//          [--songs 1] [--switch SECONDS:MOOD]

#include "core/diagnostics.hpp"
#include "song.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geosim::client::audio {

struct render_options {
    std::string path;
    uint64_t seed = 1;
    mood m = mood::calm;
    float intensity = -1.0f;  // < 0: the mood's default
    int songs = 1;            // consecutive songs (gap between them, seeds chained)
    double switch_at = -1.0;  // seconds; >= 0: change the mood then (tests the wrap-up)
    mood switch_to = mood::crisis;
    int sample_rate = 48000;
};

struct song_report {
    std::string description;
    double start_s = 0, end_s = 0; // in the rendered file
    double composed_s = 0;         // nominal length from the score
    float bpm = 0;
    double estimated_bpm = 0;      // from the audio
    int onsets = 0;
    int on_grid = 0;               // onsets within 10 ms of a 16th-note grid line
    int on_beat = 0;               // ... of a quarter-note grid line
    double latency_ms = 0;         // median onset offset (detector latency)
    uint64_t kicks = 0;
};

struct render_report {
    double seconds = 0;
    double peak = 0, peak_dbfs = -200, rms_dbfs = -200;
    uint64_t non_finite = 0, clipped = 0; // samples; clipped = |x| >= 1
    std::vector<song_report> songs;
};

bool render_music(render_options const& opt, render_report& report, core::diagnostics& diag);
void print_report(render_report const& report);

} // namespace geosim::client::audio
