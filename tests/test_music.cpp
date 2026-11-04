// Procedural music (docs/14-audio.md): composition is deterministic, songs
// last 2-3.5 minutes (long ones up to 10, intermissions 30-80 s) with a sane score that ends on the
// tonic, rolls never run faster than 16ths (one beat of 32nds at the end of a
// build at most), wartime has a low lead over a hard kick, songs vary in
// style and lead register, no voice is bit-crushed or 8-bit, and the player's
// output is finite and below 0 dBFS.

#include "client/audio/composer.hpp"
#include "client/audio/jukebox.hpp"
#include "client/audio/synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
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
    std::set<std::string> styles;
    int lead_lo = 127, lead_hi = 0; // range of the songs' median lead notes (outside wartime)
    for(int mi = 0; mi < mood_count; ++mi) {
        const mood m = static_cast<mood>(mi);
        for(uint64_t seed : {1ull, 3ull, 7ull, 9ull, 123456789ull, 6509162505067969530ull, 13761442689889167167ull}) {
            const song a = compose(seed, m), b = compose(seed, m);
            check(a.events.size() == b.events.size() && a.description == b.description, "same seed, same song", seed, m);
            bool same = a.events.size() == b.events.size();
            for(std::size_t i = 0; same && i < a.events.size(); ++i) same = a.events[i].tick == b.events[i].tick && a.events[i].notes[0] == b.events[i].notes[0];
            check(same, "same seed, same events", seed, m);
            check(a.duration_seconds() >= 30.0 && a.duration_seconds() <= 600.0, "duration 30 s - 10 min", seed, m);
            if(a.sections.size() > 5) check(a.duration_seconds() >= 120.0, "full songs last 2-3 min", seed, m);
            if(m == mood::wartime) check(a.bpm >= 138.0f && a.bpm <= 148.0f, "wartime BPM 138-148", seed, m);
            else check(a.bpm >= 80.0f && a.bpm <= 176.0f, "BPM 80-176", seed, m);
            if(m == mood::calm) check(a.bpm <= 120.0f, "calm BPM at most 120", seed, m);
            if(m == mood::triumph) check(a.bpm >= 112.0f, "triumph BPM at least 112", seed, m);
            styles.insert(a.style);
            // late-90s / early-2000s synth, not chiptune: no bit-crushed or
            // 8-bit wavetable voices, 24 dB filters on the melodic voices
            bool no_chip = true;
            for(int c = 0; c < num_channels; ++c) {
                patch const& pt = a.patches[c];
                if(pt.kit) continue;
                no_chip = no_chip && pt.crush_rate == 0.0f && pt.w1 != wave::table && pt.w2 != wave::table;
                if(c == ch_lead || c == ch_pad || c == ch_arp || c == ch_counter || c == ch_bass) no_chip = no_chip && pt.poles4;
            }
            check(no_chip, "no 8-bit / bit-crushed voices, 24 dB filters", seed, m);
            if(m == mood::wartime) check(a.style == "hardtrance", "wartime is hardtrance", seed, m);
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
                if(e.ch == ch_pad && e.tick == last_bar) {
                    for(int i = 0; i < e.count; ++i) tonic_chord = tonic_chord || (e.notes[i] - a.final_key) % 12 == 0;
                }
                if(e.ch == ch_lead && e.tick >= last_bar) {
                    const int pc = ((e.notes[0] - a.final_key) % 12 + 12) % 12;
                    lead_end = (pc == 0 || pc == 3 || pc == 4) && e.tick + e.len == a.total_ticks();
                }
            }
            check(tonic_chord, "final tonic chord", seed, m);
            check(lead_end, "lead ends on a held tonic", seed, m);

            // rolls: no run of 3+ snare/clap onsets closer than a 16th,
            // except one beat of 32nds just before a section boundary
            std::vector<uint32_t> on;
            for(note_event const& e : a.events) {
                if(e.ch == ch_snare && (on.empty() || on.back() != e.tick)) on.push_back(e.tick);
            }
            bool rolls_ok = true;
            for(std::size_t i = 1, run = 0, start = 0; i <= on.size(); ++i) {
                if(i < on.size() && on[i] - on[i - 1] < ticks_per_row) {
                    if(run++ == 0) start = on[i - 1];
                    continue;
                }
                if(run >= 3) {
                    const uint32_t end = on[i - 1];
                    bool near = false;
                    for(section const& sec : a.sections) {
                        const uint32_t b = (sec.start_bar + sec.bars) * ticks_per_bar;
                        near = near || (b >= end && b - end <= 2 * 4 * ticks_per_row);
                    }
                    rolls_ok = rolls_ok && near && end - start <= 4 * ticks_per_row;
                }
                run = 0;
            }
            check(rolls_ok, "rolls top out at 16ths (32nds only on a build's last beat)", seed, m);

            // arp solo / choir block: the arp leads the solo (no lead there); the
            // choir sings only inside a choir block
            for(section const& sec : a.sections) {
                if(sec.kind != section_kind::arp_solo && sec.kind != section_kind::choir) continue;
                const uint32_t t0 = sec.start_bar * ticks_per_bar, t1 = (sec.start_bar + sec.bars) * ticks_per_bar;
                int arp = 0, lead = 0, choir = 0;
                for(note_event const& e : a.events) {
                    if(e.tick < t0 || e.tick >= t1) continue;
                    arp += e.ch == ch_arp;
                    lead += e.ch == ch_lead;
                    choir += e.ch == ch_choir;
                }
                if(sec.kind == section_kind::arp_solo) check(arp >= static_cast<int>(sec.bars) * 8 && lead == 0, "arp solo: the arp leads, no lead", seed, m);
                else check(choir > 0 && arp > 0, "choir block: choir and synth", seed, m);
            }
            {
                bool inside = true;
                for(note_event const& e : a.events) {
                    if(e.ch != ch_choir) continue;
                    bool in = false;
                    for(section const& sec : a.sections) in = in || (sec.kind == section_kind::choir && e.tick >= sec.start_bar * ticks_per_bar && e.tick < (sec.start_bar + sec.bars) * ticks_per_bar);
                    inside = inside && in;
                }
                check(inside, "choir only in choir blocks", seed, m);
            }

            // lead register: wartime low (bass-like), the others vary per song
            std::vector<int> lead;
            for(note_event const& e : a.events) {
                if(e.ch == ch_lead && e.layer != layer_min_high) lead.push_back(e.notes[0]);
            }
            std::sort(lead.begin(), lead.end());
            const int median = lead.empty() ? 0 : lead[lead.size() / 2];
            if(m == mood::wartime) {
                check(median < 60, "wartime lead median below C4", seed, m);
                check(a.patches[ch_kick].kick_drive >= 3.0f && a.patches[ch_kick].kick_pitch >= 240.0f, "wartime hardtrance kick", seed, m);
            } else if(!lead.empty()) {
                lead_lo = std::min(lead_lo, median);
                lead_hi = std::max(lead_hi, median);
            }

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
    check(styles.size() >= 4, "at least 4 style families over the tested seeds", 0, mood::calm);
    // the new sections occur, and never in calm (choir) or intermissions
    int solos = 0, choirs = 0, calm_choirs = 0;
    for(uint64_t seed = 1; seed <= 100; ++seed) {
        for(int mi = 0; mi < mood_count; ++mi) {
            const song a = compose(seed, static_cast<mood>(mi));
            for(section const& sec : a.sections) {
                solos += sec.kind == section_kind::arp_solo;
                choirs += sec.kind == section_kind::choir;
                calm_choirs += sec.kind == section_kind::choir && mi == static_cast<int>(mood::calm);
            }
        }
    }
    check(solos >= 50 && choirs >= 30 && calm_choirs == 0, "arp solos and choir blocks occur (choir never in calm)", 0, mood::calm);
    // the euro lead (boomerang reference): common in triumph, never in wartime
    int euro_triumph = 0, euro_war = 0;
    for(uint64_t seed = 1; seed <= 100; ++seed) {
        euro_triumph += compose(seed, mood::triumph).description.find("euro lead") != std::string::npos;
        euro_war += compose(seed, mood::wartime).description.find("euro lead") != std::string::npos;
    }
    check(euro_triumph >= 25 && euro_war == 0, "euro lead in triumph, not in wartime", 0, mood::triumph);
    // the chip family is rare: at most 1 song in 10 over 200 seeds
    int chip = 0;
    for(uint64_t seed = 1; seed <= 200; ++seed) chip += compose(seed, static_cast<mood>(seed % 4)).style == "chip";
    check(chip <= 20, "chip family rare", 0, mood::calm);
    check(lead_hi - lead_lo >= 12, "song lead registers span an octave or more", 0, mood::calm);
    // B (skip): the song fades out within ~1.2 s and the next queued song starts
    {
        struct queue {
            std::vector<std::unique_ptr<song>> songs;
            std::size_t next = 0;
        } q;
        for(uint64_t sd : {11ull, 12ull, 13ull}) q.songs.push_back(std::make_unique<song>(compose(sd, mood::tense)));
        auto take = [](void* u) -> song* {
            auto* qq = static_cast<queue*>(u);
            return qq->next < qq->songs.size() ? qq->songs[qq->next++].get() : nullptr;
        };
        auto retire = [](void*, song*) {};
        auto box = std::make_unique<jukebox>(48000.0f, take, retire, &q);
        box->set_mood(mood::tense, 0.5f);
        bool finite = true;
        auto run = [&](double seconds) {
            for(int blk = 0; blk < static_cast<int>(seconds * 48000 / 512); ++blk) {
                box->render(buf.data(), 512);
                for(float x : buf) finite = finite && std::isfinite(x);
            }
        };
        run(10.0);
        check(box->songs_started() == 1 && box->current() == q.songs[0].get(), "first song playing", 11, mood::tense);
        box->skip();
        run(2.0);
        check(box->songs_started() == 2 && box->current() == q.songs[1].get(), "skip starts the next song within 2 s", 11, mood::tense);
        check(finite, "skip output finite", 11, mood::tense);
    }
    if(failures == 0) std::puts("test_music: ok");
    return failures == 0 ? 0 : 1;
}
