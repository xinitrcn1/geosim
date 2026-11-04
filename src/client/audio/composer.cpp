// Procedural composer. docs/14-audio.md#composer explains the musical rules;
// the short version:
//  - form: intro / verse / chorus / bridge / breakdown / drop / outro, in
//    8- and 16-bar sections, sized so the song lasts 2-3 minutes;
//  - harmony: one key and mode per song, diatonic 4-chord progressions
//    (1 or 2 bars per chord), pad voicings chosen for smooth voice leading;
//  - melody: two 2-bar motifs per song, placed on the harmony (strong beats
//    snapped to chord tones), answered by varied copies (antecedent ends
//    open, consequent resolves) and by the arpeggiator (call and response);
//  - rhythm: one groove per song (kick, backbeat, hats, ghost notes, bass
//    rhythm), repeated, with fills, risers and drops at section boundaries;
//  - intensity layers: optional events tagged with a layer the player
//    enables from the live mood intensity.

#include "composer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace geosim::client::audio {

char const* mood_name(mood m) noexcept {
    switch(m) {
    case mood::calm: return "calm";
    case mood::tense: return "tense";
    case mood::crisis: return "crisis";
    case mood::triumph: return "triumph";
    }
    return "?";
}

bool parse_mood(char const* text, mood& out) noexcept {
    for(int i = 0; i < mood_count; ++i) {
        if(std::strcmp(text, mood_name(static_cast<mood>(i))) == 0) {
            out = static_cast<mood>(i);
            return true;
        }
    }
    return false;
}

char const* channel_name(int ch) noexcept {
    static char const* const names[num_channels] = {"kick", "snare", "hats", "bass", "lead", "arp", "pad", "fx"};
    return ch >= 0 && ch < num_channels ? names[ch] : "?";
}

char const* section_name(section_kind k) noexcept {
    switch(k) {
    case section_kind::intro: return "intro";
    case section_kind::verse: return "verse";
    case section_kind::chorus: return "chorus";
    case section_kind::bridge: return "bridge";
    case section_kind::breakdown: return "breakdown";
    case section_kind::drop: return "drop";
    case section_kind::outro: return "outro";
    }
    return "?";
}

uint64_t next_seed(uint64_t seed) noexcept {
    uint64_t z = seed + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

namespace {

// --- deterministic random numbers -------------------------------------------

class rng {
public:
    explicit rng(uint64_t seed) : s_(seed ^ 0x6A09E667F3BCC909ull) {}
    uint64_t next() noexcept {
        s_ = next_seed(s_);
        return s_;
    }
    float uniform() noexcept { return static_cast<float>(next() >> 40) / 16777216.0f; } // [0,1)
    float range(float lo, float hi) noexcept { return lo + (hi - lo) * uniform(); }
    int range(int lo, int hi) noexcept { return lo + static_cast<int>(next() % static_cast<uint64_t>(hi - lo + 1)); } // inclusive
    bool chance(float p) noexcept { return uniform() < p; }
    template<typename T, std::size_t N>
    T const& pick(T const (&a)[N]) noexcept {
        return a[next() % N];
    }

private:
    uint64_t s_;
};

uint8_t u8(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }

// --- scales and harmony -----------------------------------------------------

struct mode_def {
    char const* name;
    int steps[7];
    bool minor;
};
constexpr mode_def modes[] = {
    {"ionian", {0, 2, 4, 5, 7, 9, 11}, false},      {"dorian", {0, 2, 3, 5, 7, 9, 10}, true},
    {"phrygian", {0, 1, 3, 5, 7, 8, 10}, true},     {"lydian", {0, 2, 4, 6, 7, 9, 11}, false},
    {"mixolydian", {0, 2, 4, 5, 7, 9, 10}, false},  {"aeolian", {0, 2, 3, 5, 7, 8, 10}, true},
    {"harmonic minor", {0, 2, 3, 5, 7, 8, 11}, true},
};
enum mode_id { m_ionian, m_dorian, m_phrygian, m_lydian, m_mixolydian, m_aeolian, m_harmonic };
constexpr char const* note_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Four-chord progressions as scale degrees (0 = tonic). Every chord is a
// diatonic triad of its mode that is major or minor (no diminished chords).
struct progression {
    int mode_mask; // bit per mode_id it fits
    int deg[4];
};
constexpr int bit(int m) { return 1 << m; }
constexpr int majorish = bit(m_ionian) | bit(m_mixolydian) | bit(m_lydian);
constexpr int minorish = bit(m_aeolian) | bit(m_dorian) | bit(m_phrygian) | bit(m_harmonic);
constexpr progression progressions[] = {
    {bit(m_ionian), {0, 4, 5, 3}},                  // I V vi IV
    {bit(m_ionian), {0, 3, 5, 4}},                  // I IV vi V
    {bit(m_ionian), {5, 3, 0, 4}},                  // vi IV I V
    {bit(m_ionian), {0, 5, 3, 4}},                  // I vi IV V
    {bit(m_ionian) | bit(m_mixolydian), {0, 3, 0, 4}}, // I IV I V (V = v in mixolydian)
    {bit(m_mixolydian), {0, 6, 3, 0}},              // I bVII IV I
    {bit(m_mixolydian), {0, 6, 0, 3}},              // I bVII I IV
    {bit(m_lydian), {0, 1, 0, 1}},                  // I II I II
    {bit(m_lydian), {0, 1, 4, 0}},                  // I II V I
    {bit(m_aeolian), {0, 5, 2, 6}},                 // i VI III VII
    {bit(m_aeolian), {0, 0, 6, 5}},                 // i i VII VI (the reference module's bass cycle)
    {bit(m_aeolian), {0, 6, 5, 6}},                 // i VII VI VII
    {bit(m_aeolian) | bit(m_harmonic), {0, 5, 3, 4}}, // i VI iv v / V
    {bit(m_aeolian) | bit(m_dorian), {0, 3, 6, 2}},  // i iv VII III (IV in dorian)
    {bit(m_dorian), {0, 3, 0, 3}},                  // i IV i IV
    {bit(m_dorian), {0, 6, 3, 0}},                  // i VII IV i
    {bit(m_phrygian), {0, 1, 0, 6}},                // i bII i bVII
    {bit(m_phrygian), {0, 1, 5, 1}},                // i bII bVI bII
    {bit(m_harmonic), {0, 5, 4, 4}},                // i VI V V
    {bit(m_harmonic), {0, 3, 4, 0}},                // i iv V i
};
static_assert((majorish & minorish) == 0);

struct motif_note {
    int row;  // within the 2-bar cell, 0..31
    int len;  // rows
    int deg;  // scale degree relative to the cell's first chord root
};
using motif = std::vector<motif_note>;

enum class bass_style : uint8_t { offbeat, rolling, house, octaves };

// --- the composer -----------------------------------------------------------

class composer {
public:
    composer(uint64_t seed, mood m) : r_(seed), s_() {
        s_.seed = seed;
        s_.m = m;
    }

    song run() {
        choose_basics();
        choose_instruments();
        choose_groove();
        choose_form();
        for(std::size_t i = 0; i < plan_.size(); ++i) render_section(i);
        std::stable_sort(s_.events.begin(), s_.events.end(), [](note_event const& a, note_event const& b) { return a.tick < b.tick; });
        describe();
        return std::move(s_);
    }

private:
    // A section as planned: its kind plus the harmony it uses.
    struct plan_item {
        section sec;
        int const* prog = nullptr;
        int bars_per_chord = 1;
        int variant = 0; // 0 first occurrence, 1 repeat (varied)
    };

    rng r_;
    song s_;
    int key_ = 57;
    mode_def const* mode_ = &modes[0];
    int const* prog_a_ = nullptr;
    int const* prog_b_ = nullptr;
    int const* prog_c_ = nullptr;
    int bpc_a_ = 2, bpc_b_ = 1;
    int swing_ = 0;          // ticks added to odd rows
    bool seventh_ = false;   // pads use 7th chords
    bool broken_kick_ = false;
    bool kick_pickup_ = false;
    uint8_t backbeat_ = dr_clap;
    std::vector<int> ghosts_;
    uint16_t hat_closed_ = 0, hat_open_ = 0, hat_extra_ = 0;
    bass_style bass_ = bass_style::offbeat;
    uint16_t bass_mask_ = 0;
    int bass_role_[16]{};    // 0 root, 1 octave, 2 fifth, 3 seventh/below
    std::vector<int> arp_order_;
    int arp_rate_ = 1;
    uint16_t stab_mask_ = 0;
    bool chip_chords_ = false;
    bool staccato_ = false;
    motif motif_a_, motif_b_;
    std::vector<plan_item> plan_;
    int last_voicing_[4] = {0, 0, 0, 0};
    int voicing_n_ = 0;

    // --- pitch helpers ------------------------------------------------------
    [[nodiscard]] int pitch(int base, int deg) const {
        const int oct = deg >= 0 ? deg / 7 : -((6 - deg) / 7);
        return base + 12 * oct + mode_->steps[deg - 7 * oct];
    }
    // Degrees (relative to the tonic) of the chord built on `root`.
    [[nodiscard]] bool is_chord_tone(int deg, int root) const {
        const int rel = ((deg - root) % 7 + 7) % 7;
        return rel == 0 || rel == 2 || rel == 4;
    }
    [[nodiscard]] int nearest_chord_tone(int deg, int root, int avoid_root = 0) const {
        for(int d = 0; d <= 3; ++d) {
            for(int sgn : {1, -1}) {
                const int c = deg + sgn * d;
                if(is_chord_tone(c, root) && !(avoid_root && ((c - root) % 7 + 7) % 7 == 0)) return c;
            }
        }
        return deg;
    }
    [[nodiscard]] int chord_at(plan_item const& p, int bar) const { return p.prog[(bar / p.bars_per_chord) % 4]; }

    // --- emission -------------------------------------------------------------
    void add(uint32_t bar, int row, uint8_t ch, std::initializer_list<int> notes, int len_rows, int vel, uint8_t lay = layer_always,
             note_fx fx = note_fx::none, int len_ticks_extra = 0) {
        note_event e;
        int tick_in_bar = row * ticks_per_row;
        if((row & 1) && ch != ch_kick) tick_in_bar += swing_;
        e.tick = bar * ticks_per_bar + static_cast<uint32_t>(tick_in_bar);
        e.len = static_cast<uint16_t>(std::max(1, len_rows * ticks_per_row + len_ticks_extra - ((row & 1) ? swing_ : 0)));
        e.ch = ch;
        e.vel = u8(std::clamp(vel + r_.range(-5, 5), 1, 127));
        e.layer = lay;
        e.fx = fx;
        e.count = 0;
        for(int n : notes) {
            if(e.count < 4) e.notes[e.count++] = u8(n);
        }
        s_.events.push_back(e);
    }
    void add_ticks(uint32_t tick, uint8_t ch, int note, int len_ticks, int vel, uint8_t lay = layer_always) {
        note_event e;
        e.tick = tick;
        e.len = static_cast<uint16_t>(std::max(1, len_ticks));
        e.ch = ch;
        e.vel = u8(std::clamp(vel, 1, 127));
        e.layer = lay;
        e.count = 1;
        e.notes[0] = u8(note);
        s_.events.push_back(e);
    }

    // --- song-level choices -------------------------------------------------
    void choose_basics() {
        const mood m = s_.m;
        // Tempo: upbeat but not hurried (the reference sits at ~146 BPM; this
        // game wants 110-128).
        float lo = 112, hi = 118;
        if(m == mood::tense) lo = 116, hi = 122;
        if(m == mood::crisis) lo = 122, hi = 128;
        if(m == mood::triumph) lo = 120, hi = 126;
        s_.bpm = std::round(r_.range(lo, hi + 0.99f));
        key_ = 45 + r_.range(0, 11); // tonic between A2 and G#3
        s_.key = key_;
        static constexpr int calm_modes[] = {m_ionian, m_lydian, m_mixolydian, m_dorian, m_ionian};
        static constexpr int tense_modes[] = {m_dorian, m_aeolian, m_aeolian, m_dorian};
        static constexpr int crisis_modes[] = {m_aeolian, m_phrygian, m_harmonic, m_aeolian};
        static constexpr int triumph_modes[] = {m_ionian, m_mixolydian, m_ionian, m_lydian};
        int mode = m_ionian;
        switch(m) {
        case mood::calm: mode = r_.pick(calm_modes); break;
        case mood::tense: mode = r_.pick(tense_modes); break;
        case mood::crisis: mode = r_.pick(crisis_modes); break;
        case mood::triumph: mode = r_.pick(triumph_modes); break;
        }
        s_.mode = mode;
        mode_ = &modes[mode];
        std::vector<int const*> fit;
        for(auto const& p : progressions) {
            if(p.mode_mask & bit(mode)) fit.push_back(p.deg);
        }
        prog_a_ = fit[r_.next() % fit.size()];
        prog_b_ = fit[r_.next() % fit.size()];
        if(fit.size() > 1) {
            for(int tries = 0; tries < 4 && prog_b_ == prog_a_; ++tries) prog_b_ = fit[r_.next() % fit.size()];
        }
        prog_c_ = fit[r_.next() % fit.size()];
        bpc_a_ = r_.chance(0.6f) ? 2 : 1;
        bpc_b_ = r_.chance(0.6f) ? 1 : 2;
        const float swing_p = m == mood::calm ? 0.6f : m == mood::crisis ? 0.1f : 0.35f;
        swing_ = r_.chance(swing_p) ? 1 : 0;
        seventh_ = (m == mood::calm || m == mood::triumph) ? r_.chance(0.6f) : r_.chance(0.25f);
    }

    void choose_instruments() {
        const mood m = s_.m;
        auto& P = s_.patches;
        auto& X = s_.mix;
        // drums
        P[ch_kick].kit = true;
        P[ch_kick].kick_pitch = r_.range(130.0f, 190.0f);
        P[ch_kick].kick_end = r_.range(44.0f, 54.0f);
        P[ch_kick].kick_decay = m == mood::calm ? r_.range(0.25f, 0.35f) : r_.range(0.3f, 0.45f);
        P[ch_kick].kick_click = r_.range(0.2f, 0.7f);
        P[ch_snare].kit = true;
        P[ch_snare].snare_tone = r_.range(170.0f, 230.0f);
        P[ch_snare].snare_decay = r_.range(0.12f, 0.2f);
        P[ch_hats].kit = true;
        P[ch_hats].hat_decay = r_.range(0.03f, 0.06f);
        P[ch_hats].hat_tone = r_.range(0.8f, 1.25f);
        P[ch_fx].kit = true;
        P[ch_fx].mono = false;

        // bass
        patch& b = P[ch_bass];
        const int bass_kind = m == mood::calm ? r_.range(0, 2) : m == mood::crisis ? r_.range(2, 3) : r_.range(0, 3);
        b.glide = 0.0f;
        switch(bass_kind) {
        case 0: // warm triangle + sine sub
            b.w1 = wave::triangle, b.w2 = wave::saw, b.mix2 = 0.25f, b.sub = 0.6f;
            b.cutoff = 900, b.resonance = 0.1f, b.fenv = 1.5f, b.fdecay = 0.12f;
            break;
        case 1: // chip pulse bass
            b.w1 = wave::pulse, b.duty = 0.25f, b.w2 = wave::pulse, b.mix2 = 0.0f, b.sub = 0.5f;
            b.cutoff = 1200, b.resonance = 0.15f, b.fenv = 1.0f, b.fdecay = 0.1f;
            break;
        case 2: // "sampled" 8-bit bass
            b.w1 = wave::table, b.table1 = 1, b.w2 = wave::sine, b.mix2 = 0.5f, b.sub = 0.3f;
            b.cutoff = 1500, b.resonance = 0.1f, b.fenv = 1.2f, b.fdecay = 0.15f, b.crush_rate = 16726.0f;
            break;
        default: // resonant saw (acid-lite)
            b.w1 = wave::saw, b.w2 = wave::saw, b.mix2 = 0.5f, b.detune = 7, b.sub = 0.4f;
            b.cutoff = 500, b.resonance = r_.range(0.45f, 0.65f), b.fenv = 3.0f, b.fdecay = 0.14f, b.drive = 0.4f;
            break;
        }
        b.attack = 0.003f, b.decay = 0.25f, b.sustain = 0.6f, b.release = 0.05f, b.key_track = 0.3f;

        // lead
        patch& l = P[ch_lead];
        const int lead_kind = r_.range(0, 3);
        switch(lead_kind) {
        case 0: // PWM pulse with vibrato
            l.w1 = wave::pulse, l.w2 = wave::pulse, l.mix2 = 0.4f, l.detune = 9, l.duty = 0.35f, l.pwm_depth = 0.12f, l.pwm_rate = 0.7f;
            l.cutoff = 3200, l.fenv = 1.0f;
            break;
        case 1: // detuned saws
            l.w1 = wave::saw, l.w2 = wave::saw, l.mix2 = 0.7f, l.detune = 12;
            l.cutoff = 2600, l.fenv = 1.4f, l.resonance = 0.25f;
            break;
        case 2: // chip "sample" lead
            l.w1 = wave::table, l.table1 = r_.chance(0.5f) ? 0 : 2, l.w2 = wave::pulse, l.duty = 0.5f, l.mix2 = 0.3f, l.semi2 = 12;
            l.cutoff = 4500, l.fenv = 0.5f, l.crush_rate = 20000.0f;
            break;
        default: // square + fifth organ-ish
            l.w1 = wave::pulse, l.duty = 0.5f, l.w2 = wave::triangle, l.mix2 = 0.5f, l.semi2 = 7;
            l.cutoff = 3000, l.fenv = 0.8f;
            break;
        }
        l.attack = 0.01f, l.decay = 0.3f, l.sustain = 0.65f, l.release = 0.12f;
        l.vib_depth = r_.range(0.08f, 0.2f), l.vib_rate = r_.range(4.5f, 6.0f), l.vib_delay = 0.18f;
        l.glide = r_.chance(0.5f) ? 0.04f : 0.0f;
        l.fdecay = 0.25f;
        staccato_ = r_.chance(m == mood::crisis ? 0.6f : 0.3f);

        // arp: short plucks
        patch& a = P[ch_arp];
        const int arp_kind = r_.range(0, 2);
        a.w1 = arp_kind == 0 ? wave::pulse : arp_kind == 1 ? wave::saw : wave::table;
        a.duty = r_.chance(0.5f) ? 0.125f : 0.25f;
        a.table1 = 3;
        a.w2 = wave::pulse, a.mix2 = 0.0f;
        a.attack = 0.002f, a.decay = r_.range(0.09f, 0.16f), a.sustain = 0.0f, a.release = 0.06f;
        a.cutoff = 1800, a.fenv = 2.0f, a.fdecay = 0.08f, a.resonance = 0.3f;
        if(arp_kind == 2) a.crush_rate = 16726.0f;

        // pad / chords
        patch& p = P[ch_pad];
        chip_chords_ = r_.chance(m == mood::calm ? 0.25f : 0.4f);
        if(chip_chords_) { // tracker arpeggio chord on a pulse
            p.w1 = wave::pulse, p.duty = 0.25f, p.w2 = wave::pulse, p.mix2 = 0.0f;
            p.cutoff = 2400, p.resonance = 0.1f;
            p.attack = 0.005f, p.decay = 0.3f, p.sustain = 0.5f, p.release = 0.25f;
        } else {
            p.w1 = wave::saw, p.w2 = wave::saw, p.mix2 = 0.9f, p.detune = r_.range(8.0f, 14.0f);
            p.cutoff = m == mood::crisis ? 900.0f : 1400.0f, p.resonance = 0.15f, p.fenv = 0.6f, p.fdecay = 0.6f;
            p.attack = 0.08f, p.decay = 0.6f, p.sustain = 0.8f, p.release = 0.6f;
            p.pwm_depth = 0.0f;
        }
        p.mono = true;
        p.key_track = 0.2f;

        // mix (levels tuned by ear on offline renders, see docs/14-audio.md)
        const float g[num_channels] = {0.95f, 0.5f, 0.24f, 0.55f, 0.3f, 0.2f, chip_chords_ ? 0.16f : 0.2f, 0.32f};
        const float pan[num_channels] = {0.0f, 0.05f, 0.3f, 0.0f, 0.15f, -0.4f, -0.05f, 0.0f};
        const float dly[num_channels] = {0.0f, 0.04f, 0.06f, 0.0f, 0.28f, 0.32f, 0.12f, 0.1f};
        const float rev[num_channels] = {0.02f, 0.18f, 0.06f, 0.0f, 0.2f, 0.15f, 0.35f, 0.35f};
        const float duck[num_channels] = {0.0f, 0.0f, 0.15f, 0.55f, 0.15f, 0.3f, 0.6f, 0.0f};
        for(int c = 0; c < num_channels; ++c) {
            X.gain[c] = g[c];
            X.pan[c] = c == ch_arp && r_.chance(0.5f) ? -pan[c] : pan[c];
            X.delay[c] = dly[c];
            X.reverb[c] = rev[c] * (m == mood::calm ? 1.3f : 1.0f);
            X.duck[c] = duck[c];
        }
        if(X.pan[ch_arp] > 0) X.pan[ch_lead] = -X.pan[ch_lead];
        X.bright[ch_lead] = X.bright[ch_arp] = X.bright[ch_pad] = true;
        X.delay_rows = r_.chance(0.7f) ? 3.0f : 2.0f;
        X.delay_feedback = r_.range(0.3f, 0.45f);
        X.reverb_size = m == mood::calm ? 0.85f : 0.75f;
    }

    void choose_groove() {
        const mood m = s_.m;
        broken_kick_ = m == mood::calm && r_.chance(0.3f);
        kick_pickup_ = r_.chance(0.5f);
        backbeat_ = r_.chance(m == mood::crisis ? 0.5f : 0.7f) ? dr_clap : dr_snare;
        static constexpr int ghost_sets[][3] = {{7, -1, -1}, {15, -1, -1}, {10, 15, -1}, {7, 14, -1}, {-1, -1, -1}, {3, 11, 15}};
        for(int g : ghost_sets[r_.next() % 6]) {
            if(g >= 0) ghosts_.push_back(g);
        }
        // hats: closed on 8ths or 16ths, open on the off-beats
        hat_open_ = 0x4444; // rows 2, 6, 10, 14
        hat_closed_ = r_.chance(0.5f) ? 0x5555 : 0x1111; // 8ths (rows 0,2,..) or quarters
        if(hat_closed_ == 0x5555) hat_closed_ &= static_cast<uint16_t>(~hat_open_);
        hat_extra_ = static_cast<uint16_t>(0xFFFF & ~hat_closed_ & ~hat_open_); // remaining 16ths: intensity layer

        // bass rhythm
        if(m == mood::crisis) bass_ = r_.chance(0.6f) ? bass_style::rolling : bass_style::offbeat;
        else if(m == mood::calm) bass_ = r_.chance(0.5f) ? bass_style::house : bass_style::octaves;
        else {
            static constexpr bass_style all[] = {bass_style::offbeat, bass_style::rolling, bass_style::house, bass_style::octaves};
            bass_ = r_.pick(all);
        }
        switch(bass_) {
        case bass_style::offbeat: bass_mask_ = 0x4444; break;
        case bass_style::rolling: bass_mask_ = 0xEEEE; break;
        case bass_style::octaves: bass_mask_ = 0x5555; break;
        case bass_style::house: {
            // 4-6 hits, always one near the downbeat, never more than two in a row
            bass_mask_ = r_.chance(0.5f) ? 0x0001 : 0x0004;
            int hits = r_.range(4, 6);
            while(__builtin_popcount(bass_mask_) < hits) {
                const int row = r_.range(1, 15);
                bass_mask_ = static_cast<uint16_t>(bass_mask_ | (1u << row));
            }
            break;
        }
        }
        for(int i = 0; i < 16; ++i) {
            if(bass_ == bass_style::octaves) bass_role_[i] = (i / 2) % 2;
            else if(bass_ == bass_style::house) {
                static constexpr int roles[] = {0, 0, 1, 2, 0, 3};
                bass_role_[i] = i < 2 ? 0 : r_.pick(roles);
            } else if(bass_ == bass_style::rolling) bass_role_[i] = (i % 4 == 3 && r_.chance(0.3f)) ? 1 : 0;
            else bass_role_[i] = 0;
        }

        // arpeggio order over chord tones (0 root, 1 third, 2 fifth, 3 octave, 4 octave+third)
        static constexpr int orders[][8] = {{0, 1, 2, 3, -1}, {3, 2, 1, 0, -1}, {0, 1, 2, 3, 2, 1, -1}, {0, 2, 1, 3, -1},
                                            {0, 2, 3, 1, 2, 3, 0, 2}, {0, 3, 1, 3, 2, 3, -1}, {0, 2, 4, 2, -1}, {2, 0, 3, 0, 4, 0, -1}};
        auto const& o = orders[r_.next() % 8];
        for(int v : o) {
            if(v < 0) break;
            arp_order_.push_back(v);
        }
        arp_rate_ = m == mood::calm && r_.chance(0.5f) ? 2 : 1;

        // chord stabs
        static constexpr uint16_t stabs[] = {0x5494 /* 2,4,7,10,12,14 (reference) */, 0x4444, 0x1249, 0x4924, 0x2492};
        stab_mask_ = r_.pick(stabs);

        // motifs
        const float density = m == mood::calm ? 0.8f : m == mood::crisis ? 1.3f : 1.0f;
        motif_a_ = make_motif(density, prog_a_[0], prog_a_[bpc_a_ == 1 ? 1 : 0]);
        motif_b_ = make_motif(density * 1.2f, prog_b_[0], prog_b_[bpc_b_ == 1 ? 1 : 0]);
    }

    // A 2-bar motif: weighted rhythm (strong beats likelier), stepwise
    // contour with leap recovery, strong beats on chord tones; the last
    // 4 rows stay empty so the answer can breathe.
    motif make_motif(float density, int root0, int root1) {
        motif out;
        const int target = std::clamp(static_cast<int>(std::round(r_.range(4.0f, 6.5f) * density)), 3, 10);
        auto weight = [&](int row) {
            if(row % 8 == 0) return 5.0f;
            if(row % 4 == 0) return 3.0f;
            if(row % 2 == 0) return 2.2f;
            return 0.6f * density;
        };
        bool used[32] = {};
        const bool repeat_rhythm = r_.chance(0.45f);
        const int span = repeat_rhythm ? 16 : 28;
        const int first = r_.chance(0.7f) ? 0 : 2;
        used[first] = true;
        int count = 1;
        const int want = repeat_rhythm ? std::max(2, target / 2 + 1) : target;
        for(int guard = 0; count < want && guard < 200; ++guard) {
            float total = 0;
            for(int i = 0; i < span; ++i) total += used[i] ? 0.0f : weight(i);
            float x = r_.uniform() * total;
            for(int i = 0; i < span; ++i) {
                if(used[i]) continue;
                x -= weight(i);
                if(x <= 0) {
                    used[i] = true;
                    ++count;
                    break;
                }
            }
        }
        if(repeat_rhythm) { // bar 2 echoes bar 1's rhythm, minus its tail
            int last = 0;
            for(int i = 0; i < 16; ++i) {
                if(used[i]) last = i;
            }
            for(int i = 0; i < 16; ++i) {
                if(used[i] && !(i == last && last >= 8) && i + 16 < 28) used[i + 16] = true;
            }
        }
        std::vector<int> rows;
        for(int i = 0; i < 32; ++i) {
            if(used[i]) rows.push_back(i);
        }
        int deg = nearest_chord_tone(r_.range(0, 4), root0) - root0; // relative to root0
        int prev_step = 0;
        for(std::size_t i = 0; i < rows.size(); ++i) {
            const int row = rows[i];
            if(i > 0) {
                int step;
                if(std::abs(prev_step) >= 3) step = prev_step > 0 ? -1 : 1; // leap recovery
                else {
                    const float x = r_.uniform();
                    const int mag = x < 0.55f ? 1 : x < 0.8f ? 2 : x < 0.9f ? 0 : r_.range(3, 4);
                    step = r_.chance(0.5f) ? mag : -mag;
                }
                deg += step;
                if(deg > 7) deg -= 2 * std::abs(step);
                if(deg < -3) deg += 2 * std::abs(step);
                prev_step = step;
            }
            const int root = row < 16 ? root0 : root1;
            if(row % 4 == 0) deg = nearest_chord_tone(deg + root0, root) - root0;
            const int next_row = i + 1 < rows.size() ? rows[i + 1] : 32;
            int len = std::min(next_row - row, 6);
            if(staccato_) len = std::min(len, 2);
            out.push_back({row, std::max(1, len), deg});
        }
        return out;
    }

    // --- form ---------------------------------------------------------------
    void choose_form() {
        using K = section_kind;
        struct item {
            K k;
            int bars;
        };
        static const std::vector<item> forms[] = {
            {{K::intro, 8}, {K::verse, 16}, {K::chorus, 16}, {K::bridge, 8}, {K::breakdown, 8}, {K::drop, 16}, {K::outro, 8}},
            {{K::intro, 8}, {K::verse, 16}, {K::chorus, 16}, {K::breakdown, 16}, {K::drop, 16}, {K::outro, 8}},
            {{K::intro, 16}, {K::verse, 16}, {K::chorus, 16}, {K::breakdown, 8}, {K::verse, 8}, {K::drop, 16}, {K::outro, 8}},
            {{K::intro, 8}, {K::verse, 16}, {K::chorus, 8}, {K::verse, 8}, {K::chorus, 16}, {K::breakdown, 8}, {K::drop, 16}, {K::outro, 8}},
        };
        std::vector<item> form = forms[r_.next() % 4];
        const double bar_s = 240.0 / static_cast<double>(s_.bpm);
        const double target = r_.range(140.0f, 165.0f);
        auto total = [&] {
            int b = 0;
            for(auto const& it : form) b += it.bars;
            return b;
        };
        // Fit 2-3 minutes: shorten long sections, or extend the drop.
        for(int guard = 0; guard < 8 && total() * bar_s > target + 10; ++guard) {
            bool done = false;
            for(auto& it : form) {
                if(it.bars == 16 && (it.k == K::intro || it.k == K::breakdown)) {
                    it.bars = 8;
                    done = true;
                    break;
                }
            }
            if(!done) {
                for(std::size_t i = 0; i < form.size(); ++i) {
                    if(form[i].k == K::bridge || (form[i].k == K::verse && i > 2)) {
                        form.erase(form.begin() + static_cast<std::ptrdiff_t>(i));
                        done = true;
                        break;
                    }
                }
            }
            if(!done) break;
        }
        for(int guard = 0; guard < 4 && total() * bar_s < target - 15; ++guard) {
            for(std::size_t i = 0; i < form.size(); ++i) {
                if(form[i].k == K::drop) {
                    form.insert(form.begin() + static_cast<std::ptrdiff_t>(i) + 1, item{K::chorus, 8});
                    break;
                }
            }
        }
        uint32_t bar = 0;
        int verses = 0, choruses = 0;
        for(auto const& it : form) {
            plan_item p;
            p.sec = {it.k, bar, static_cast<uint32_t>(it.bars)};
            switch(it.k) {
            case K::intro:
            case K::verse:
            case K::outro: p.prog = prog_a_, p.bars_per_chord = bpc_a_; break;
            case K::chorus:
            case K::drop: p.prog = prog_b_, p.bars_per_chord = bpc_b_; break;
            case K::bridge: p.prog = prog_c_, p.bars_per_chord = 2; break;
            case K::breakdown: p.prog = r_.chance(0.5f) ? prog_a_ : prog_b_, p.bars_per_chord = 2; break;
            }
            if(it.k == K::verse) p.variant = verses++ > 0 ? 1 : 0;
            if(it.k == K::chorus || it.k == K::drop) p.variant = choruses++ > 0 ? 1 : 0;
            plan_.push_back(p);
            s_.sections.push_back(p.sec);
            bar += static_cast<uint32_t>(it.bars);
        }
        s_.total_bars = bar;
    }

    // --- section rendering --------------------------------------------------
    void render_section(std::size_t index) {
        using K = section_kind;
        plan_item const& p = plan_[index];
        const K k = p.sec.kind;
        const K next = index + 1 < plan_.size() ? plan_[index + 1].sec.kind : K::outro;
        const bool last_section = index + 1 == plan_.size();
        const int bars = static_cast<int>(p.sec.bars);
        const uint32_t b0 = p.sec.start_bar;
        const bool into_big = next == K::chorus || next == K::drop;

        for(int bi = 0; bi < bars; ++bi) {
            const uint32_t bar = b0 + static_cast<uint32_t>(bi);
            const bool last_bar = bi == bars - 1;
            const bool phrase_end = (bi % 8) == 7;
            const int root = chord_at(p, bi);
            const int next_root = last_bar ? (last_section ? 0 : chord_at(plan_[index + 1], 0)) : chord_at(p, bi + 1);
            const bool chord_change = next_root != root;
            const bool fill = last_bar && !last_section && k != K::breakdown;
            const bool drop_gap = k == K::breakdown && last_bar; // silence on the last beat before the drop

            // kick
            bool kick = k != K::breakdown;
            if(k == K::intro && s_.m == mood::calm && bi < bars / 2) kick = false;
            if(k == K::outro && bi >= bars - 1) kick = false;
            if(k == K::breakdown && bi >= bars - 1 && bars >= 8) kick = false;
            if(kick) {
                if(broken_kick_ && k != K::drop && k != K::chorus) {
                    for(int row : {0, 7, 10}) add(bar, row, ch_kick, {dr_kick}, 2, row == 0 ? 120 : 105);
                } else {
                    for(int row = 0; row < 16; row += 4) {
                        if(fill && row == 12 && into_big) continue; // leave room for the fill
                        add(bar, row, ch_kick, {dr_kick}, 2, 120);
                    }
                }
                if(phrase_end && kick_pickup_ && !fill) add(bar, 14, ch_kick, {dr_kick}, 1, 90);
            }
            if(last_section && last_bar) add(bar, 0, ch_kick, {dr_kick}, 2, 120);

            // backbeat, ghosts, fills
            const bool backbeat = k != K::intro || bi >= bars / 2;
            if(backbeat && k != K::breakdown && !(last_section && last_bar)) {
                for(int row : {4, 12}) {
                    if(fill && row == 12) continue;
                    add(bar, row, ch_snare, {backbeat_}, 2, 112);
                    if(k == K::drop || k == K::chorus) {
                        // layer the other backbeat sound at high intensity
                        add(bar, row, ch_snare, {backbeat_ == dr_clap ? dr_snare : dr_clap}, 2, 80, layer_min_high);
                    }
                }
                for(int g : ghosts_) {
                    if(!(fill && g >= 8)) add(bar, g, ch_snare, {dr_snare}, 1, 42, layer_min_mid);
                }
            }
            if(fill) {
                if(into_big || r_.chance(0.5f)) {
                    for(int row = 8; row < 16; ++row) {
                        if(row < 12 && (row & 1)) continue;
                        add(bar, row, ch_snare, {dr_snare}, 1, 60 + (row - 8) * 8);
                    }
                } else {
                    const int toms[4] = {dr_tom_high, dr_tom_high, dr_tom_mid, dr_tom_low};
                    for(int i = 0; i < 4; ++i) add(bar, 12 + i, ch_fx, {toms[i]}, 2, 85 + i * 6);
                }
            }
            if(k == K::breakdown && bi >= bars - 2) { // snare roll: 8ths, then 16ths, then the drop gap
                const bool final = bi == bars - 1;
                for(int row = 0; row < (final ? 12 : 16); row += final ? 1 : 2) {
                    const int pos = (final ? 16 : 0) + row;
                    add(bar, row, ch_snare, {dr_snare}, 1, 50 + pos * 2);
                }
            }

            // hats
            const bool hats = k != K::breakdown && !(k == K::outro && bi >= bars - 2) && !(last_section && last_bar);
            if(hats) {
                const bool open = k == K::chorus || k == K::drop || k == K::bridge || (k == K::verse && p.variant == 1);
                for(int row = 0; row < 16; ++row) {
                    const uint16_t bitm = static_cast<uint16_t>(1u << row);
                    if(fill && row >= 12) continue;
                    if(hat_open_ & bitm) {
                        if(open || k == K::verse) add(bar, row, ch_hats, {dr_hat_open}, 2, 92, open ? layer_always : layer_min_mid);
                        if(!open) add(bar, row, ch_hats, {dr_hat_closed}, 1, 85, k == K::verse ? layer_max_mid : layer_always);
                    } else if(hat_closed_ & bitm) {
                        add(bar, row, ch_hats, {dr_hat_closed}, 1, row % 4 == 0 ? 70 : 82);
                    } else if(hat_extra_ & bitm) {
                        add(bar, row, ch_hats, {dr_hat_closed}, 1, 52, k == K::intro ? layer_min_high : layer_min_mid);
                    }
                }
                if(k == K::drop || k == K::chorus) {
                    for(int row = 0; row < 16; ++row) add(bar, row, ch_fx, {row % 4 == 2 ? dr_rim : dr_shaker}, 1, row % 4 == 2 ? 55 : 40, layer_min_high);
                }
            }
            if(k == K::breakdown && bi < bars - 2 && s_.m != mood::calm) {
                for(int row = 2; row < 16; row += 4) add(bar, row, ch_hats, {dr_hat_closed}, 1, 45, layer_min_mid);
            }

            // bass
            bool bass = k != K::intro || (bars >= 16 && bi >= bars - 4);
            if(k == K::breakdown || (k == K::outro && bi >= bars - 3)) bass = false;
            if(bass) {
                for(int row = 0; row < 16; ++row) {
                    if(!(bass_mask_ & (1u << row))) continue;
                    if(fill && row >= 12 && into_big) continue;
                    int next_hit = row + 1;
                    while(next_hit < 16 && !(bass_mask_ & (1u << next_hit))) ++next_hit;
                    const int len = std::max(1, std::min(next_hit - row, 4) * 3 / 4);
                    int deg = root;
                    switch(bass_role_[row]) {
                    case 1: deg = root + 7; break;
                    case 2: deg = root + 4; break;
                    case 3: deg = root - 1; break;
                    default: break;
                    }
                    if(chord_change && next_hit >= 16 && row >= 10) deg = next_root + (next_root > root ? -1 : 1); // approach tone
                    const int n = pitch(key_ - 12, deg);
                    // at high intensity root notes gain an octave (an alternative event, not an extra one)
                    const bool root_hit = bass_role_[row] == 0;
                    add(bar, row, ch_bass, {n}, len, row % 4 == 0 ? 112 : 100, root_hit ? layer_max_high : layer_always);
                    if(root_hit) add(bar, row, ch_bass, {n, n + 12}, len, 104, layer_min_high);
                }
            }
            if(k == K::breakdown && bi < bars - 1 && (bi % p.bars_per_chord) == 0) {
                // a soft sub note under the breakdown pads
                add(bar, 0, ch_bass, {pitch(key_ - 12, root)}, p.bars_per_chord * 16 - 2, 70, layer_min_mid);
            }
            if(last_section && last_bar) add(bar, 0, ch_bass, {pitch(key_ - 12, 0)}, 12, 110);

            // pads
            const bool pad_sustain = k == K::intro || k == K::breakdown || k == K::bridge || k == K::outro || k == K::verse;
            if(!drop_gap || bi < bars - 1) {
                if(pad_sustain && (bi % p.bars_per_chord) == 0) {
                    const int len = p.bars_per_chord * 16 - 1;
                    const int vel = k == K::verse ? 62 : k == K::intro ? 70 : 85;
                    const uint8_t lay = k == K::verse ? layer_max_high : layer_always;
                    emit_chord(bar, 0, root, len, vel, lay, drop_gap ? 12 : 0);
                }
                if(k == K::chorus || k == K::drop || (k == K::verse && p.variant == 1)) {
                    for(int row = 0; row < 16; ++row) {
                        if(!(stab_mask_ & (1u << row))) continue;
                        if(fill && row >= 12) continue;
                        emit_chord(bar, row, root, 1, 92, k == K::verse ? layer_min_high : layer_always, 0, true);
                    }
                    if(k == K::verse) {
                        // keep the sustained pad at high intensity too, quieter
                    } else if(bi % p.bars_per_chord == 0) {
                        emit_chord(bar, 0, root, p.bars_per_chord * 16 - 1, 50, layer_max_mid);
                    }
                }
            }
            if(last_section && last_bar) emit_chord(bar, 0, 0, 15, 90, layer_always);

            // fx: crashes, impacts, risers
            if(bi == 0 && (k == K::chorus || k == K::drop || (k == K::verse && index > 1) || k == K::outro)) add(bar, 0, ch_fx, {dr_crash}, 16, 100);
            if(bi == 0 && k == K::drop) add(bar, 0, ch_fx, {dr_impact}, 16, 110);
            if(k == K::breakdown && bi == bars - 4 && bars >= 4) add(bar, 0, ch_fx, {dr_riser}, 64, 100);
            if(into_big && k != K::breakdown && bi == bars - 2) add(bar, 0, ch_fx, {dr_riser}, 32, 80, layer_min_mid);
            if(k == K::intro && last_bar) add(bar, 0, ch_fx, {dr_swell}, 16, 90);
            if(last_section && last_bar) add(bar, 0, ch_fx, {dr_crash}, 16, 105);
        }

        render_melody(index);
    }

    // Chord voicing with smooth voice leading: of all inversions/octaves
    // inside the pad register, take the one closest to the previous voicing.
    void emit_chord(uint32_t bar, int row, int root, int len, int vel, uint8_t lay, int cut_rows = 0, bool stab = false) {
        const int tones = seventh_ && !stab ? 4 : 3;
        int pcs[4];
        for(int i = 0; i < tones; ++i) pcs[i] = pitch(key_, root + 2 * i);
        int best[4] = {}, best_cost = 1 << 30;
        const int lo = key_ + 7, hi = key_ + 26;
        for(int inv = 0; inv < tones; ++inv) {
            for(int oct = -1; oct <= 1; ++oct) {
                int v[4];
                for(int i = 0; i < tones; ++i) {
                    int n = pcs[(i + inv) % tones] + 12 * oct + ((i + inv) >= tones ? 12 : 0);
                    v[i] = n;
                }
                bool ok = true;
                for(int i = 0; i < tones; ++i) ok = ok && v[i] >= lo && v[i] <= hi;
                if(!ok) continue;
                int cost = 0;
                if(voicing_n_ == tones) {
                    for(int i = 0; i < tones; ++i) cost += std::abs(v[i] - last_voicing_[i]);
                } else {
                    cost = std::abs(v[0] - (key_ + 12));
                }
                if(cost < best_cost) {
                    best_cost = cost;
                    std::copy(v, v + tones, best);
                }
            }
        }
        if(best_cost == (1 << 30)) {
            for(int i = 0; i < tones; ++i) best[i] = pcs[i] + 12;
        }
        if(!stab) {
            std::copy(best, best + tones, last_voicing_);
            voicing_n_ = tones;
        }
        const int l = cut_rows > 0 ? std::min(len, cut_rows) : len;
        const note_fx fx = chip_chords_ ? note_fx::arp : note_fx::none;
        if(tones == 4) add(bar, row, ch_pad, {best[0], best[1], best[2], best[3]}, l, vel, lay, fx);
        else add(bar, row, ch_pad, {best[0], best[1], best[2]}, l, vel, lay, fx);
    }

    // Place a motif on the harmony from `bar`: degrees shift with the
    // chord root (diatonic sequence, wrapped to +-3 steps), strong-beat
    // notes snap to chord tones, the ending is chosen by phrase position.
    enum class ending : uint8_t { keep, open, close };
    void place_motif(motif const& mt, int motif_root, plan_item const& p, int bi, int base, uint8_t ch, int vel, ending end, int stretch = 1,
                     int octave_layer = -1, bool vary = false) {
        const uint32_t bar0 = p.sec.start_bar + static_cast<uint32_t>(bi);
        const int root = chord_at(p, bi);
        int shift = ((root - motif_root) % 7 + 7) % 7;
        if(shift > 3) shift -= 7;
        const std::size_t n = mt.size();
        for(std::size_t i = 0; i < n; ++i) {
            motif_note const& mn = mt[i];
            if(vary && i == n / 2 && n > 3) continue; // rhythmic variation: drop an inner note
            const int row_abs = mn.row * stretch;
            const int bar_off = row_abs / 16;
            if(bi + bar_off >= static_cast<int>(p.sec.bars)) break;
            const int row = row_abs % 16;
            const int croot = chord_at(p, bi + bar_off);
            int deg = mn.deg + motif_root + shift;
            if(row % 4 == 0) deg = nearest_chord_tone(deg, croot);
            if(i + 1 == n && end != ending::keep) {
                if(end == ending::close) {
                    // resolve: the tonic if it is a chord tone, else the chord tone nearest it
                    const int tonic = 7 * static_cast<int>(std::floor(deg / 7.0 + 0.5));
                    deg = is_chord_tone(tonic, croot) ? tonic : nearest_chord_tone(tonic, croot);
                } else {
                    deg = nearest_chord_tone(deg, croot, 1); // a non-root chord tone: leaves the phrase open
                }
            }
            const int note = pitch(base, deg);
            const int len = mn.len * stretch;
            if(octave_layer >= 0) {
                add(bar0 + static_cast<uint32_t>(bar_off), row, ch, {note}, len, vel, layer_max_high);
                add(bar0 + static_cast<uint32_t>(bar_off), row, ch, {note, note + 12}, len, vel, static_cast<uint8_t>(octave_layer));
            } else {
                add(bar0 + static_cast<uint32_t>(bar_off), row, ch, {note}, len, vel);
            }
        }
    }

    // Arpeggio over the chord tones, `bars` bars from `bi`, starting from
    // `start_row`.
    void place_arp(plan_item const& p, int bi, int bars, int start_row, int vel, uint8_t lay, int base_shift = 0) {
        int idx = 0;
        for(int b = bi; b < bi + bars && b < static_cast<int>(p.sec.bars); ++b) {
            const int root = chord_at(p, b);
            const uint32_t bar = p.sec.start_bar + static_cast<uint32_t>(b);
            for(int row = (b == bi ? start_row : 0); row < 16; row += arp_rate_) {
                const int tone = arp_order_[static_cast<std::size_t>(idx++) % arp_order_.size()];
                const int deg = root + (tone == 3 ? 7 : tone == 4 ? 9 : tone * 2);
                const int n = pitch(key_ + 24 + base_shift, deg);
                const int accent = row % 4 == 0 ? 12 : row % 2 == 0 ? 4 : 0;
                add(bar, row, ch_arp, {n}, 1, vel + accent, lay);
            }
        }
    }

    void render_melody(std::size_t index) {
        using K = section_kind;
        plan_item const& p = plan_[index];
        const int bars = static_cast<int>(p.sec.bars);
        const int lead_base = key_ + 12;
        const int root_a = prog_a_[0], root_b = prog_b_[0];
        switch(p.sec.kind) {
        case K::intro:
            if(bars >= 8) place_arp(p, bars - 4, 4, 0, 58, layer_min_mid);
            break;
        case K::verse:
            // call (lead, bars 1-2) and response (arp, bars 3-4)
            for(int ph = 0; ph < bars; ph += 4) {
                const bool second = (ph / 4) % 2 == 1;
                place_motif(motif_a_, root_a, p, ph, lead_base, ch_lead, 96, ph + 4 >= bars || second ? ending::close : ending::open, 1, -1,
                            p.variant == 1 && second);
                place_arp(p, ph + 2, 2, 0, 66, layer_always);
            }
            break;
        case K::chorus:
        case K::drop:
            // full 8-bar melody: motif, open answer, motif, closed answer
            for(int ph = 0; ph < bars; ph += 8) {
                const bool high = p.sec.kind == K::drop && ph >= 8;
                const int base = lead_base + (high ? 12 : 0);
                const int oct_layer = high ? -1 : layer_min_high;
                place_motif(motif_b_, root_b, p, ph, base, ch_lead, 100, ending::keep, 1, oct_layer);
                place_motif(motif_b_, root_b, p, ph + 2, base, ch_lead, 94, ending::open, 1, oct_layer, true);
                place_motif(motif_b_, root_b, p, ph + 4, base, ch_lead, 100, ending::keep, 1, oct_layer, p.variant == 1);
                place_motif(motif_b_, root_b, p, ph + 6, base, ch_lead, 94, ending::close, 1, oct_layer, true);
                place_arp(p, ph, std::min(8, bars - ph), 0, 50, layer_min_mid, -12);
            }
            break;
        case K::bridge:
            // the arp takes the call with motif A, the lead answers
            for(int ph = 0; ph < bars; ph += 4) {
                place_arp(p, ph, 2, 0, 70, layer_always);
                place_motif(motif_a_, root_a, p, ph + 2, lead_base, ch_lead, 88, ph + 4 >= bars ? ending::close : ending::open, 1, -1, true);
            }
            break;
        case K::breakdown:
            // motif A in augmentation over the pads
            for(int ph = (bars >= 16 ? 8 : 0); ph + 4 <= bars; ph += 4) {
                place_motif(motif_a_, root_a, p, ph, lead_base, ch_lead, 80, ph + 8 > bars ? ending::close : ending::open, 2);
            }
            if(bars >= 16) place_arp(p, 0, 8, 0, 54, layer_min_mid);
            break;
        case K::outro:
            place_arp(p, 0, std::max(0, bars - 4), 0, 56, layer_min_mid);
            break;
        }
    }

    void describe() {
        char buf[256];
        const int pc = ((key_ % 12) + 12) % 12;
        int deg_a[4], deg_b[4];
        std::copy(prog_a_, prog_a_ + 4, deg_a);
        std::copy(prog_b_, prog_b_ + 4, deg_b);
        std::string form;
        for(auto const& sec : s_.sections) {
            if(!form.empty()) form += " ";
            form += section_name(sec.kind);
            form += std::to_string(sec.bars);
        }
        static char const* const bass_names[] = {"offbeat", "rolling", "house", "octaves"};
        std::snprintf(buf, sizeof buf, "seed %llu, %s, %.0f BPM, %s %s, prog %d%d%d%d/%d%d%d%d, bass %s%s%s, %u bars (%.0f s): ",
                      static_cast<unsigned long long>(s_.seed), mood_name(s_.m), static_cast<double>(s_.bpm), note_names[pc], mode_->name, deg_a[0] + 1,
                      deg_a[1] + 1, deg_a[2] + 1, deg_a[3] + 1, deg_b[0] + 1, deg_b[1] + 1, deg_b[2] + 1, deg_b[3] + 1,
                      bass_names[static_cast<int>(bass_)], swing_ ? ", swing" : "", chip_chords_ ? ", chip chords" : "", s_.total_bars,
                      s_.duration_seconds());
        s_.description = buf + form;
    }
};

} // namespace

song compose(uint64_t seed, mood m) { return composer(seed, m).run(); }

} // namespace geosim::client::audio
