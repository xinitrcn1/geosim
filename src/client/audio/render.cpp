#include "render.hpp"

#include "composer.hpp"
#include "jukebox.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace geosim::client::audio {

namespace {

struct offline_source {
    uint64_t seed = 1;
    mood m = mood::calm;
    int remaining = 1;
    int solo = -1;
    std::vector<std::unique_ptr<song>> owned;
};

song* take_song(void* user) {
    auto* src = static_cast<offline_source*>(user);
    if(src->remaining <= 0) return nullptr;
    --src->remaining;
    const uint64_t seed = src->owned.empty() ? src->seed : next_seed(src->owned.back()->seed);
    src->owned.push_back(std::make_unique<song>(compose(seed, src->m)));
    if(src->solo >= 0) {
        for(int c = 0; c < num_channels; ++c) {
            if(c != src->solo) src->owned.back()->mix.gain[c] = 0.0f;
        }
    }
    return src->owned.back().get();
}
void retire_song(void*, song*) {} // owned by offline_source

void put_u32(std::FILE* f, uint32_t v) {
    const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void put_u16(std::FILE* f, uint16_t v) {
    const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
    std::fwrite(b, 1, 2, f);
}

double db(double x) { return x > 1e-10 ? 20.0 * std::log10(x) : -200.0; }

} // namespace

bool render_music(render_options const& opt, render_report& rep, core::diagnostics& diag) {
    std::FILE* f = std::fopen(opt.path.c_str(), "wb");
    if(!f) {
        diag.fail("cannot write " + opt.path);
        return false;
    }
    const int sr = opt.sample_rate;
    // header, sizes patched at the end
    std::fwrite("RIFF", 1, 4, f);
    put_u32(f, 0);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put_u32(f, 16);
    put_u16(f, 1);
    put_u16(f, 2);
    put_u32(f, static_cast<uint32_t>(sr));
    put_u32(f, static_cast<uint32_t>(sr * 4));
    put_u16(f, 4);
    put_u16(f, 16);
    std::fwrite("data", 1, 4, f);
    put_u32(f, 0);

    offline_source src;
    src.seed = opt.seed;
    src.m = opt.m;
    src.remaining = std::max(1, opt.songs);
    src.solo = opt.solo;
    auto box = std::make_unique<jukebox>(static_cast<float>(sr), &take_song, &retire_song, &src);
    box->gap_seconds = 3.0f;
    box->set_volume(0.8f);
    mood current_mood = opt.m;
    box->set_mood(current_mood, opt.intensity >= 0 ? opt.intensity : default_intensity(current_mood));

    // Low-band (kick/bass) energy at 1 ms hops for the grid analysis.
    const int hop = sr / 1000;
    std::vector<float> env_db;
    float lp1 = 0, lp2 = 0;
    const float a = 1.0f - std::exp(-6.2831853f * 120.0f / static_cast<float>(sr));
    double hop_energy = 0;
    const double smooth = 1.0 - std::exp(-1.0 / (0.004 * sr));
    int hop_n = 0;

    constexpr int block = 512;
    float buf[block * 2];
    int16_t pcm[block * 2];
    uint64_t frames_total = 0;
    double sum_sq = 0;
    uint64_t started_seen = 0, kicks_before = 0;
    double tail = -1; // seconds rendered after the last song ended
    const double max_seconds = 60.0 * 12.0 * static_cast<double>(src.remaining);
    while(true) {
        const double now = static_cast<double>(frames_total) / sr;
        if(opt.switch_at >= 0 && now >= opt.switch_at && current_mood != opt.switch_to) {
            current_mood = opt.switch_to;
            src.m = current_mood;
            box->set_mood(current_mood, default_intensity(current_mood));
        }
        box->render(buf, block);
        if(box->songs_started() != started_seen) {
            started_seen = box->songs_started();
            song_report s;
            s.description = box->current()->description;
            s.start_s = now;
            s.composed_s = box->current()->duration_seconds();
            s.bpm = box->current()->bpm;
            rep.songs.push_back(s);
        }
        if(!rep.songs.empty() && rep.songs.back().end_s == 0 && !box->current() && started_seen > 0) {
            rep.songs.back().end_s = now;
            rep.songs.back().kicks = box->voice_player().kicks_played() - kicks_before;
            kicks_before = box->voice_player().kicks_played();
        }
        for(int i = 0; i < block * 2; ++i) {
            const float x = buf[i];
            if(!std::isfinite(x)) {
                ++rep.non_finite;
                pcm[i] = 0;
                continue;
            }
            const double ax = std::fabs(static_cast<double>(x));
            rep.peak = std::max(rep.peak, ax);
            if(ax >= 1.0) ++rep.clipped;
            sum_sq += static_cast<double>(x) * x;
            pcm[i] = static_cast<int16_t>(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f));
        }
        for(int i = 0; i < block; ++i) {
            const float m = 0.5f * (buf[2 * i] + buf[2 * i + 1]);
            lp1 += a * ((std::isfinite(m) ? m : 0.0f) - lp1);
            lp2 += a * (lp1 - lp2);
            // squared, smoothed over ~4 ms (a 50 Hz kick ripples at 1 ms hops)
            hop_energy += (static_cast<double>(lp2) * lp2 - hop_energy) * smooth;
            if(++hop_n == hop) {
                env_db.push_back(static_cast<float>(10.0 * std::log10(hop_energy + 1e-12)));
                hop_n = 0;
            }
        }
        std::fwrite(pcm, sizeof(int16_t), block * 2, f);
        frames_total += block;
        const bool all_done = src.remaining == 0 && !box->current() && started_seen > 0;
        if(all_done) {
            if(tail < 0) tail = 0;
            tail += static_cast<double>(block) / sr;
            if(tail >= 2.5) break;
        }
        if(now > max_seconds) {
            diag.fail("render did not finish (sequencer stuck?)");
            break;
        }
    }
    // Patch the RIFF sizes.
    const uint32_t data_bytes = static_cast<uint32_t>(frames_total * 4);
    std::fseek(f, 4, SEEK_SET);
    put_u32(f, 36 + data_bytes);
    std::fseek(f, 40, SEEK_SET);
    put_u32(f, data_bytes);
    const bool write_ok = std::ferror(f) == 0;
    std::fclose(f);
    if(!write_ok) diag.fail("write error on " + opt.path);

    rep.seconds = static_cast<double>(frames_total) / sr;
    rep.peak_dbfs = db(rep.peak);
    rep.rms_dbfs = db(std::sqrt(sum_sq / std::max<double>(1.0, static_cast<double>(frames_total * 2))));

    // --- tempo-grid analysis, independent of the sequencer -------------------
    // Onset: the smoothed low-band energy rises > 8 dB within 6 ms (a real
    // attack; slow swells such as the sidechain release do not count), above
    // -45 dB, 80 ms refractory; its time is the steepest rise in the window.
    std::vector<double> onsets;
    std::vector<float> onset_level; // peak low-band level in the 30 ms after the onset
    for(std::size_t i = 12; i < env_db.size(); ++i) {
        float lo = env_db[i - 6];
        for(std::size_t j = i - 6; j < i; ++j) lo = std::min(lo, env_db[j]);
        if(env_db[i] > -45.0f && env_db[i] - lo > 8.0f && (onsets.empty() || static_cast<double>(i) * 1e-3 - onsets.back() > 0.08)) {
            std::size_t best = i;
            float best_d = -1e9f;
            for(std::size_t j = i - 11; j <= i; ++j) {
                if(env_db[j] - env_db[j - 1] > best_d) best_d = env_db[j] - env_db[j - 1], best = j;
            }
            float peak_after = env_db[i];
            for(std::size_t j = i; j < std::min(env_db.size(), i + 30); ++j) peak_after = std::max(peak_after, env_db[j]);
            onsets.push_back(static_cast<double>(best) * 1e-3);
            onset_level.push_back(peak_after);
        }
    }
    for(auto& s : rep.songs) {
        const double step = 60.0 / (static_cast<double>(s.bpm) * rows_per_beat);
        const double end = s.end_s > 0 ? s.end_s : rep.seconds;
        // The detector's latency is constant: estimate it as the median
        // offset from the 16th grid, then count onsets within 10 ms of it.
        std::vector<double> offs;
        for(double t : onsets) {
            if(t < s.start_s || t > end) continue;
            const double rel = t - s.start_s;
            offs.push_back(rel - std::round(rel / step) * step);
        }
        std::vector<double> sorted = offs;
        std::sort(sorted.begin(), sorted.end());
        s.latency_ms = sorted.empty() ? 0.0 : 1000.0 * sorted[sorted.size() / 2];
        const double lat = s.latency_ms * 1e-3;
        // "strong" onsets: within 10 dB of the song's loud low-band level
        // (95th percentile of onset levels), i.e. the kicks
        std::vector<float> levels;
        for(std::size_t i = 0; i < onsets.size(); ++i) {
            if(onsets[i] >= s.start_s && onsets[i] <= end) levels.push_back(onset_level[i]);
        }
        std::sort(levels.begin(), levels.end());
        const float loud = levels.empty() ? 0.0f : levels[levels.size() * 95 / 100];
        for(std::size_t i = 0; i < onsets.size(); ++i) {
            const double t = onsets[i];
            if(t < s.start_s || t > end) continue;
            const double rel = t - s.start_s - lat;
            const bool grid = std::fabs(rel - std::round(rel / step) * step) < 0.010;
            ++s.onsets;
            if(grid) ++s.on_grid;
            if(std::fabs(rel - std::round(rel / (4 * step)) * 4 * step) < 0.010) ++s.on_beat;
            if(onset_level[i] >= loud - 10.0f) {
                ++s.strong;
                if(grid) ++s.strong_on_grid;
            }
        }
        // tempo by autocorrelation of the onset-strength envelope (250 fps)
        const std::size_t i0 = static_cast<std::size_t>(s.start_s * 1000.0), i1 = std::min(env_db.size(), static_cast<std::size_t>(end * 1000.0));
        std::vector<float> o;
        for(std::size_t i = i0 + 4; i + 4 <= i1; i += 4) {
            o.push_back(std::max(0.0f, env_db[i] - env_db[i - 4]));
        }
        double mean = 0;
        for(float v : o) mean += v;
        mean /= static_cast<double>(std::max<std::size_t>(1, o.size()));
        for(float& v : o) v -= static_cast<float>(mean);
        double best = -1e300, best_bpm = 0;
        for(int b10 = 750; b10 <= 1800; ++b10) { // 75-180 BPM: every style family
            const double bpm = b10 / 10.0, lag = 250.0 * 60.0 / bpm;
            double acc = 0;
            for(int k : {1, 2, 4}) {
                const double L = lag * k;
                const std::size_t l0 = static_cast<std::size_t>(L);
                const float fr = static_cast<float>(L - static_cast<double>(l0));
                for(std::size_t i = 0; i + l0 + 1 < o.size(); ++i) acc += o[i] * ((1.0f - fr) * o[i + l0] + fr * o[i + l0 + 1]);
            }
            if(acc > best) best = acc, best_bpm = bpm;
        }
        s.estimated_bpm = best_bpm;
    }
    return !diag.failed();
}

void print_report(render_report const& r) {
    std::printf("rendered %.1f s, peak %.4f (%.2f dBFS), RMS %.1f dBFS, non-finite samples %llu, clipped samples %llu\n", r.seconds, r.peak, r.peak_dbfs,
                r.rms_dbfs, static_cast<unsigned long long>(r.non_finite), static_cast<unsigned long long>(r.clipped));
    for(std::size_t i = 0; i < r.songs.size(); ++i) {
        auto const& s = r.songs[i];
        std::printf("song %zu: %s\n  plays %.1f-%.1f s (%.1f s; score %.1f s), BPM %.1f, estimated from audio %.1f\n"
                    "  low-band onsets %d: %.1f%% within 10 ms of the 16th grid, %.1f%% of the beat grid (detector latency %.1f ms)\n"
                    "  strong (kick-level) onsets %d: %.1f%% on the 16th grid; kicks played %llu\n",
                    i + 1, s.description.c_str(), s.start_s, s.end_s, s.end_s - s.start_s, s.composed_s, static_cast<double>(s.bpm), s.estimated_bpm,
                    s.onsets, s.onsets ? 100.0 * s.on_grid / s.onsets : 0.0, s.onsets ? 100.0 * s.on_beat / s.onsets : 0.0,
                    s.latency_ms, s.strong, s.strong ? 100.0 * s.strong_on_grid / s.strong : 0.0, static_cast<unsigned long long>(s.kicks));
    }
}

} // namespace geosim::client::audio
