// Procedural music (docs/14-audio.md): composition is deterministic, songs
// last 2-3 minutes (intermissions 30-80 s) with a sane score that ends on the
// tonic, and the player's output is finite and below 0 dBFS.

#include "client/audio/composer.hpp"
#include "client/audio/synth.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace geosim::client::audio;

namespace {
int failures = 0;
void check(bool ok, char const* what, uint64_t seed, mood m) {
    if(!ok) {
        std::fprintf(stderr, "FAIL: %s (seed %llu, %s)\n", what, static_cast<unsigned long long>(seed), mood_name(m));
        ++failures;
    }
}
} // namespace

int main() {
    auto p = std::make_unique<player>(48000.0f);
    std::vector<float> buf(2 * 512);
    for(int mi = 0; mi < mood_count; ++mi) {
        const mood m = static_cast<mood>(mi);
        for(uint64_t seed : {1ull, 3ull, 7ull, 9ull, 123456789ull, 6509162505067969530ull, 13761442689889167167ull}) {
            const song a = compose(seed, m), b = compose(seed, m);
            check(a.events.size() == b.events.size() && a.description == b.description, "same seed, same song", seed, m);
            bool same = a.events.size() == b.events.size();
            for(std::size_t i = 0; same && i < a.events.size(); ++i) same = a.events[i].tick == b.events[i].tick && a.events[i].notes[0] == b.events[i].notes[0];
            check(same, "same seed, same events", seed, m);
            check(a.duration_seconds() >= 30.0 && a.duration_seconds() <= 180.0, "duration 30 s - 3 min", seed, m);
            if(a.sections.size() > 5) check(a.duration_seconds() >= 120.0, "full songs last 2-3 min", seed, m);
            if(m == mood::wartime) check(a.bpm >= 132.0f && a.bpm <= 140.0f, "wartime BPM 132-140", seed, m);
            else check(a.bpm >= 110.0f && a.bpm <= 128.0f, "BPM 110-128", seed, m);
            if(m == mood::wartime) check(a.duration_seconds() >= 120.0, "wartime songs are full length", seed, m);
            bool sorted = true, in_range = true;
            for(std::size_t i = 0; i < a.events.size(); ++i) {
                if(i && a.events[i].tick < a.events[i - 1].tick) sorted = false;
                if(a.events[i].tick >= a.total_ticks()) in_range = false;
            }
            check(sorted && in_range, "events sorted and inside the song", seed, m);
            check(!a.sections.empty() && a.sections.front().kind == section_kind::intro && a.sections.back().kind == section_kind::outro, "intro ... outro", seed, m);
            // the song ends on a tonic chord, and the lead's last note is the
            // tonic (or its third), held into the last bar
            const uint32_t last_bar = (a.total_bars - 1) * ticks_per_bar;
            bool tonic_chord = false, lead_end = false;
            for(note_event const& e : a.events) {
                if(e.ch == ch_pad && e.tick == last_bar) tonic_chord = (e.notes[0] - a.key) % 12 == 0 || (e.notes[1] - a.key) % 12 == 0 || (e.notes[2] - a.key) % 12 == 0;
                if(e.ch == ch_lead && e.tick >= last_bar) {
                    const int pc = ((e.notes[0] - a.key) % 12 + 12) % 12;
                    lead_end = (pc == 0 || pc == 3 || pc == 4) && e.tick + e.len == a.total_ticks();
                }
            }
            check(tonic_chord, "final tonic chord", seed, m);
            check(lead_end, "lead ends on a held tonic", seed, m);

            // 40 s of audio at full intensity: finite, below 0 dBFS
            p->set_song(&a);
            p->set_intensity_now(1.0f);
            float peak = 0;
            bool finite = true;
            for(int blk = 0; blk < 40 * 48000 / 512; ++blk) {
                p->render(buf.data(), 512);
                for(float x : buf) {
                    finite = finite && std::isfinite(x);
                    peak = std::max(peak, std::fabs(x));
                }
            }
            p->set_song(nullptr);
            check(finite, "finite output", seed, m);
            check(peak < 1.0f, "peak below 0 dBFS", seed, m);
        }
    }
    if(failures == 0) std::puts("test_music: ok");
    return failures == 0 ? 0 : 1;
}
