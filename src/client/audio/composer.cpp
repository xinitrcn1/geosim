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
    composer(uint64_t seed, mood m) : r_(seed), tr_(seed ^ 0xA54FF53A5F1D36F1ull), s_() {
        s_.seed = seed;
        s_.m = m;
    }

    song run() {
        choose_basics();
        choose_instruments();
        choose_groove();
        choose_form();
        choose_arrangement();
        for(std::size_t i = 0; i < plan_.size(); ++i) render_section(i);
        apply_mutes();
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
        bool cadence = false; // last section: its last 2 bars are a cadence chord, then the tonic
        bool end_tonic = false; // the last bar's chord shares no tone with the tonic triad: it becomes the tonic
        // How this section hands over to the next one (docs/14-audio.md#transitions).
        uint8_t out = 0;      // transition kind (tr_*)
        bool reverse = false; // reverse cymbal into the next downbeat
        bool downlift = false; // downlifter at the start of the next section
        bool impact = false;  // impact boom at the start of the next section
    };

    rng r_;
    rng tr_; // arrangement choices (transitions, development); separate so the basics stay the same per seed
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
    // registers (MIDI notes): pads below the lead, the lead above the pads
    int pad_lo_ = 52, pad_hi_ = 69, lead_lo_ = 64, lead_hi_ = 83, lead_base_ = 69;
    int cad_deg_ = 4;         // the final cadence chord (V, bVII or bII by mode)
    // arrangement (tr_)
    int seq_step_ = 1;        // diatonic sequence step of the second cell
    int dev_steps_ = -2;      // development of the third cell in a repeated phrase
    bool dev_invert_ = false;
    int cad_steps_ = 2;       // the cadence cell's sequence step
    int risers_left_ = 0;
    bool hard_drop_ = false;  // the drop switches to an off-beat bass (hardtrance)
    bool beat_intro_ = false; // the intro starts with kick, hats and bass
    uint16_t gate_mask_ = 0;  // trance gate on breakdown pads (0 = none)
    struct mute {
        uint32_t from, to; // ticks
        uint32_t channels; // bit per channel
    };
    std::vector<mute> mutes_;
    std::string transitions_;
    // the lead's notes for the current phrase, fitted to the register before emission
    struct lead_note {
        uint32_t bar;
        int row, len, note, vel;
        bool dbl;
        int cell; // notes of one cell move together
    };
    std::vector<lead_note> phrase_;
    int cell_id_ = 0;
    int last_lead_ = -1; // the last lead note emitted (for smooth octave choices)

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
    [[nodiscard]] int chord_at(plan_item const& p, int bar) const {
        const int bars = static_cast<int>(p.sec.bars);
        if(p.cadence && bar == bars - 2) return cad_deg_;
        if((p.cadence || p.end_tonic) && bar >= bars - 1) return 0;
        return p.prog[(bar / p.bars_per_chord) % 4];
    }

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
        s_.bpm = std::min(hi, std::round(r_.range(lo, hi + 0.99f))); // (rounding could give hi + 1)
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
        // Registers (no randomness, so the seed's choices stay the same).
        // Pads sit in about an octave and a half from D3-C#4; the lead's
        // window starts a fifth above the pad floor and never below D4, so
        // it sings over the chords instead of muddying bass and pads.
        pad_lo_ = key_ + 7 <= 61 ? key_ + 7 : key_ - 5;
        pad_hi_ = pad_lo_ + 17;
        lead_lo_ = std::max(62, pad_lo_ + 7);
        lead_hi_ = lead_lo_ + 19;
        lead_base_ = key_ + 12;
        while(lead_base_ < lead_lo_ - 1) lead_base_ += 12;
        switch(mode) {
        case m_mixolydian:
        case m_dorian:
        case m_aeolian: cad_deg_ = 6; break; // bVII - i
        case m_phrygian: cad_deg_ = 1; break; // bII - i
        default: cad_deg_ = 4; break;        // V - I
        }
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
            l.gain = 1.0f;
            break;
        case 1: // detuned saws
            l.w1 = wave::saw, l.w2 = wave::saw, l.mix2 = 0.7f, l.detune = 12;
            l.cutoff = 2600, l.fenv = 1.4f, l.resonance = 0.25f;
            l.gain = 1.95f; // the detuned pair partly cancels: measured ~6 dB under the PWM lead
            break;
        case 2: // chip "sample" lead
            l.w1 = wave::table, l.table1 = r_.chance(0.5f) ? 0 : 2, l.w2 = wave::pulse, l.duty = 0.5f, l.mix2 = 0.3f, l.semi2 = 12;
            l.cutoff = 4500, l.fenv = 0.5f, l.crush_rate = 20000.0f;
            l.gain = 1.5f;
            break;
        default: // square + fifth organ-ish
            l.w1 = wave::pulse, l.duty = 0.5f, l.w2 = wave::triangle, l.mix2 = 0.5f, l.semi2 = 7;
            l.cutoff = 3000, l.fenv = 0.8f;
            l.gain = 1.0f;
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
        const float g[num_channels] = {0.72f, 0.8f, 0.5f, 0.62f, 0.36f, 0.45f, chip_chords_ ? 0.32f : 0.45f, 0.4f};
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
        // About one song in four is a short intermission (30-80 s). Its own
        // generator, so a full-length song's basics do not depend on it.
        rng ir(s_.seed ^ 0x3C6EF372FE94F82Bull);
        if(ir.chance(0.25f)) {
            static const std::vector<item> short_forms[] = {
                {{K::intro, 4}, {K::verse, 8}, {K::outro, 4}},
                {{K::intro, 4}, {K::verse, 8}, {K::chorus, 8}, {K::outro, 4}},
                {{K::intro, 8}, {K::chorus, 8}, {K::breakdown, 8}, {K::drop, 8}, {K::outro, 4}},
            };
            form = short_forms[ir.next() % 3];
        }
        const bool intermission = form.size() <= 5 && form.back().bars == 4;
        auto total = [&] {
            int b = 0;
            for(auto const& it : form) b += it.bars;
            return b;
        };
        // Fit 2-3 minutes: shorten long sections, or extend the drop.
        for(int guard = 0; guard < 8 && !intermission && total() * bar_s > target + 10; ++guard) {
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
        for(int guard = 0; guard < 4 && !intermission && total() * bar_s < target - 15; ++guard) {
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

    // --- arrangement: transitions and melodic development (tr_) -----------
    // Section boundaries get one of several hand-overs, picked per seed and
    // per context, so a riser is one option among many (docs/14-audio.md#transitions).
    enum tr_kind : uint8_t {
        tr_plain,        // straight cut, crash on the next downbeat
        tr_fill,         // snare fill on the last 2 beats
        tr_toms,         // tom fill on the last beat
        tr_roll,         // snare roll build-up (8ths, 16ths, then 32nds)
        tr_riser,        // noise riser (2 bars, 4 out of a breakdown)
        tr_short_riser,  // one-bar sweep up
        tr_filter_close, // bus low-pass closes over the last 2 bars
        tr_filter_open,  // the section starts dark and the low-pass opens across it
        tr_kick_out,     // the kick drops out for the last bar
        tr_stop,         // everything but the lead stops for the last beats
        tr_kick_return,  // breakdown: the kick comes back for its last bars, with a roll
        tr_drum_break,   // the melody, arp and chords drop out for the last 4 bars: drums and bass only
    };
    static char const* tr_name(uint8_t k) {
        static char const* const names[] = {"cut", "fill", "toms", "roll", "riser", "sweep", "filter-close", "filter-open", "kick-out", "stop", "kick-return", "drum-break"};
        return k < sizeof names / sizeof names[0] ? names[k] : "?";
    }

    void choose_arrangement() {
        using K = section_kind;
        seq_step_ = tr_.chance(0.45f) ? 1 : tr_.chance(0.5f) ? 2 : -1;
        dev_invert_ = tr_.chance(0.5f);
        dev_steps_ = tr_.chance(0.5f) ? -2 : 3;
        risers_left_ = tr_.chance(0.4f) ? 1 : 0; // most songs have no long riser at all
        int sweeps_left = 1;
        hard_drop_ = bass_ != bass_style::offbeat && tr_.chance(s_.m == mood::calm ? 0.35f : 0.55f);
        // trance gates over a bar of 16ths (bit r = row r)
        static constexpr uint16_t gates[] = {0xB6DB /* x.xx.xx.xx.x.xx.x */, 0x6DB6, 0xDDDD /* x.xxx.xx... */, 0x5555, 0xF6F6};
        gate_mask_ = tr_.chance(0.4f) ? tr_.pick(gates) : 0;
        beat_intro_ = tr_.chance(0.3f); // 90s style: the intro opens on drums and bass, the chords come in halfway
        plan_.back().cadence = true;
        cad_steps_ = tr_.chance(0.4f) ? 2 : tr_.chance(0.5f) ? -1 : 1;
        // Sections end on a chord that holds a tonic-triad tone, so the
        // closing phrase can land on a stable note: II (lydian), bVII, bII,
        // ii... at the very end become a modal cadence onto I for one bar.
        for(plan_item& p : plan_) {
            if(p.sec.kind == K::intro || p.cadence) continue;
            const int last = p.prog[((static_cast<int>(p.sec.bars) - 1) / p.bars_per_chord) % 4];
            p.end_tonic = !is_chord_tone(0, last) && !is_chord_tone(4, last) && !is_chord_tone(2, last);
        }
        uint8_t prev = 255;
        for(std::size_t i = 0; i + 1 < plan_.size(); ++i) {
            plan_item& p = plan_[i];
            const K k = p.sec.kind, next = plan_[i + 1].sec.kind;
            struct opt {
                uint8_t k;
                float w;
            };
            std::vector<opt> o;
            const float R = risers_left_ > 0 ? 1.0f : 0.0f, S = sweeps_left > 0 ? 1.0f : 0.0f;
            if(k == K::breakdown) {
                o = {{tr_roll, 1.2f}, {tr_riser, 1.8f * R}, {tr_filter_open, 1.2f}, {tr_kick_return, 1.0f}, {tr_short_riser, 0.7f * S}, {tr_stop, 0.9f}};
            } else if(next == K::chorus || next == K::drop) {
                o = {{tr_roll, 1.3f}, {tr_fill, 1.2f},      {tr_toms, 0.5f}, {tr_riser, 1.6f * R}, {tr_drum_break, k != K::intro && p.sec.bars >= 16 ? 0.7f : 0.0f},
                     {tr_short_riser, 0.6f * S}, {tr_kick_out, 0.9f}, {tr_stop, 0.6f}, {tr_plain, 0.4f}};
            } else if(next == K::breakdown) {
                o = {{tr_filter_close, 1.6f}, {tr_fill, 0.7f}, {tr_toms, 0.5f}, {tr_stop, 0.6f}, {tr_plain, 0.6f}};
            } else if(next == K::outro) {
                o = {{tr_fill, 1.0f}, {tr_toms, 0.7f}, {tr_plain, 0.8f}, {tr_filter_close, 0.6f}, {tr_kick_out, 0.6f}};
            } else if(k == K::intro) {
                o = {{tr_filter_open, 1.0f}, {tr_fill, 1.0f}, {tr_toms, 0.6f}, {tr_plain, 0.8f}, {tr_kick_out, 0.4f}};
            } else {
                o = {{tr_fill, 1.2f}, {tr_toms, 0.8f}, {tr_plain, 0.9f}, {tr_kick_out, 0.6f}, {tr_stop, 0.4f}, {tr_drum_break, p.sec.bars >= 16 ? 0.6f : 0.0f}};
            }
            auto pick = [&] {
                float total = 0;
                for(auto const& x : o) total += x.w;
                float x = tr_.uniform() * total;
                for(auto const& c : o) {
                    if(c.w <= 0) continue;
                    x -= c.w;
                    if(x <= 0) return c.k;
                }
                return o.front().k;
            };
            uint8_t t = pick();
            if(t == prev) t = pick(); // vary consecutive hand-overs
            if(t == tr_riser) --risers_left_;
            if(t == tr_short_riser) --sweeps_left;
            p.out = t;
            prev = t;
            const bool up = next == K::chorus || next == K::drop;
            p.reverse = t != tr_riser && tr_.chance(t == tr_stop ? 0.6f : up ? 0.3f : 0.15f);
            p.downlift = !up && next != K::verse && tr_.chance(0.45f);
            p.impact = next == K::drop && (t == tr_stop || tr_.chance(0.6f));
            if(!transitions_.empty()) transitions_ += ", ";
            transitions_ += tr_name(t);
            if(p.reverse) transitions_ += "+reverse";
            if(p.downlift) transitions_ += "+downlifter";
        }
    }

    // Removes or shortens the events under a stop / kick drop-out.
    void apply_mutes() {
        if(mutes_.empty()) return;
        std::vector<note_event> kept;
        kept.reserve(s_.events.size());
        for(note_event e : s_.events) {
            bool drop = false;
            for(mute const& m : mutes_) {
                if(!((m.channels >> e.ch) & 1u)) continue;
                if(e.tick >= m.from && e.tick < m.to) drop = true;
                else if(e.tick < m.from && e.tick + e.len > m.from && !s_.patches[e.ch].kit) e.len = static_cast<uint16_t>(m.from - e.tick);
            }
            if(!drop) kept.push_back(e);
        }
        s_.events.swap(kept);
    }

    // A snare roll from `bar0` over `bars` bars, ending `gap_rows` before
    // the boundary: 8ths, then 16ths, the last beat in 32nds, crescendo.
    void snare_roll(uint32_t bar0, int bars, int gap_rows) {
        const int total = bars * 16 - gap_rows;
        for(int r = 0; r < total; ++r) {
            const bool second_half = r >= total / 2;
            const bool last_beat = r >= total - 4 && bars >= 2;
            if(!second_half && (r & 1)) continue;
            const int vel = 46 + 70 * r / std::max(1, total);
            const uint32_t tick = (bar0 + static_cast<uint32_t>(r / 16)) * ticks_per_bar + static_cast<uint32_t>((r % 16) * ticks_per_row);
            add_ticks(tick, ch_snare, dr_snare, 2, vel);
            if(last_beat) add_ticks(tick + 3, ch_snare, dr_snare, 2, vel - 6);
        }
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
        const uint8_t T = last_section ? static_cast<uint8_t>(tr_plain) : p.out;
        plan_item const* prev = index > 0 ? &plan_[index - 1] : nullptr;
        const uint32_t end_tick = (b0 + static_cast<uint32_t>(bars)) * ticks_per_bar;
        constexpr uint32_t all_but_lead_fx = (1u << ch_kick) | (1u << ch_snare) | (1u << ch_hats) | (1u << ch_bass) | (1u << ch_arp) | (1u << ch_pad);

        // the hand-over at the end of this section
        int gap_rows = 0; // silence (but the lead and fx tails) before the next downbeat
        int roll_bars = 0;
        if(!last_section) {
            switch(T) {
            case tr_roll: roll_bars = k == K::breakdown ? std::min(4, bars / 2) : bars >= 8 ? 2 : 1; break;
            case tr_riser: roll_bars = k == K::breakdown ? 2 : 0; break;
            case tr_filter_open:
            case tr_kick_return: roll_bars = k == K::breakdown ? std::min(2, bars / 2) : 0; break;
            default: break;
            }
            if(k == K::breakdown && (T == tr_roll || T == tr_riser)) gap_rows = 4;
            if(T == tr_stop) gap_rows = bars >= 16 || k == K::breakdown ? 16 : 8;
            if(gap_rows > 0) mutes_.push_back({end_tick - static_cast<uint32_t>(gap_rows * ticks_per_row), end_tick, all_but_lead_fx});
            if(T == tr_drum_break) mutes_.push_back({end_tick - 4 * ticks_per_bar, end_tick, (1u << ch_lead) | (1u << ch_arp) | (1u << ch_pad)});
            if(T == tr_kick_out) mutes_.push_back({end_tick - ticks_per_bar, end_tick, (1u << ch_kick) | (bars >= 16 ? (1u << ch_bass) : 0u)});
        }
        const int kick_return_from = T == tr_kick_return ? bars - std::min(4, bars / 2) : bars;

        for(int bi = 0; bi < bars; ++bi) {
            const uint32_t bar = b0 + static_cast<uint32_t>(bi);
            const bool last_bar = bi == bars - 1;
            const bool final_bar = last_section && last_bar;
            const bool phrase_end = (bi % 8) == 7;
            const int root = chord_at(p, bi);
            const int next_root = last_bar ? (last_section ? 0 : chord_at(plan_[index + 1], 0)) : chord_at(p, bi + 1);
            const bool chord_change = next_root != root;
            const bool fill = last_bar && !last_section && (T == tr_fill || T == tr_toms) && k != K::breakdown;
            const bool rolling = k != K::breakdown && roll_bars > 0 && bi >= bars - roll_bars;

            // kick
            bool kick = k != K::breakdown || bi >= kick_return_from;
            if(k == K::intro && s_.m == mood::calm && bi < bars / 2 && !beat_intro_) kick = false;
            if(k == K::outro && bi >= bars - 1) kick = false;
            if(kick) {
                if(broken_kick_ && k != K::drop && k != K::chorus && k != K::breakdown) {
                    for(int row : {0, 7, 10}) add(bar, row, ch_kick, {dr_kick}, 2, row == 0 ? 120 : 105);
                } else {
                    for(int row = 0; row < 16; row += 4) {
                        if(fill && row == 12 && into_big) continue; // leave room for the fill
                        const int vel = k == K::breakdown ? 96 + 6 * (bi - kick_return_from) : 120;
                        add(bar, row, ch_kick, {dr_kick}, 2, vel);
                    }
                }
                if(phrase_end && kick_pickup_ && !fill && !last_bar) add(bar, 14, ch_kick, {dr_kick}, 1, 90);
            }
            if(final_bar) add(bar, 0, ch_kick, {dr_kick}, 2, 120);

            // backbeat, ghosts, fills
            const bool backbeat = (k != K::intro || bi >= bars / 2) && !rolling;
            if(backbeat && k != K::breakdown && !final_bar) {
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
                if(T == tr_fill) {
                    for(int row = 8; row < 16; ++row) {
                        if(row < 12 && (row & 1)) continue;
                        add(bar, row, ch_snare, {dr_snare}, 1, 60 + (row - 8) * 8);
                    }
                } else {
                    const int toms[4] = {dr_tom_high, dr_tom_high, dr_tom_mid, dr_tom_low};
                    for(int i = 0; i < 4; ++i) add(bar, 12 + i, ch_fx, {toms[i]}, 2, 85 + i * 6);
                }
            }
            if(T == tr_kick_out && last_bar && !last_section) {
                for(int row = 12; row < 16; ++row) add(bar, row, ch_snare, {dr_snare}, 1, 70 + (row - 12) * 10);
            }
            if(roll_bars > 0 && bi == bars - roll_bars) snare_roll(bar, roll_bars, gap_rows);

            // hats
            const bool hats = k != K::breakdown && !(k == K::outro && bi >= bars - 2) && !final_bar;
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
            if(k == K::breakdown && (bi < bars - 2 || bi >= kick_return_from) && s_.m != mood::calm) {
                for(int row = 2; row < 16; row += 4) add(bar, row, ch_hats, {dr_hat_closed}, 1, 45, layer_min_mid);
            }

            // bass (the hardtrance drop: off-beat roots, octave on top at high intensity)
            const bool offbeat_drop = k == K::drop && hard_drop_;
            const uint16_t bmask = offbeat_drop ? 0x4444 : bass_mask_;
            bool bass = k != K::intro || (bars >= 16 && bi >= bars - 4) || beat_intro_;
            if(k == K::breakdown || (k == K::outro && bi >= bars - 3)) bass = false;
            if(bass) {
                for(int row = 0; row < 16; ++row) {
                    if(!(bmask & (1u << row))) continue;
                    if(fill && row >= 12 && into_big) continue;
                    int next_hit = row + 1;
                    while(next_hit < 16 && !(bmask & (1u << next_hit))) ++next_hit;
                    const int len = std::max(1, std::min(next_hit - row, 4) * 3 / 4);
                    const int role = offbeat_drop ? 0 : bass_role_[row];
                    int deg = root;
                    switch(role) {
                    case 1: deg = root + 7; break;
                    case 2: deg = root + 4; break;
                    case 3: deg = root - 1; break;
                    default: break;
                    }
                    if(chord_change && next_hit >= 16 && row >= 10 && !offbeat_drop) deg = next_root + (next_root > root ? -1 : 1); // approach tone
                    const int n = pitch(key_ - 12, deg);
                    // at high intensity root notes gain an octave (an alternative event, not an extra one)
                    const bool root_hit = role == 0;
                    add(bar, row, ch_bass, {n}, len, row % 4 == 0 ? 112 : 104, root_hit ? layer_max_high : layer_always);
                    if(root_hit) add(bar, row, ch_bass, {n, n + 12}, len, 104, layer_min_high);
                }
            }
            if(k == K::breakdown && bi < bars - 1 && (bi % p.bars_per_chord) == 0) {
                // a soft sub note under the breakdown pads
                add(bar, 0, ch_bass, {pitch(key_ - 12, root)}, p.bars_per_chord * 16 - 2, 70, layer_min_mid);
            }
            if(k == K::outro && bi == bars - 2) add(bar, 0, ch_bass, {pitch(key_ - 12, root)}, 14, 96); // the cadence chord's root
            if(final_bar) add(bar, 0, ch_bass, {pitch(key_ - 12, 0)}, 16, 110);

            // pads (a new chord at each chord change; gated in the breakdown when the song has a gate)
            const bool pad_sustain = (k == K::intro && !(beat_intro_ && bi < bars / 2)) || k == K::breakdown || k == K::bridge || k == K::outro || k == K::verse;
            const bool new_chord = bi % p.bars_per_chord == 0 || chord_at(p, bi - 1) != root;
            int chord_rows = 0;
            for(int b = bi; b < bars && (b == bi || (b % p.bars_per_chord != 0 && chord_at(p, b) == root)); ++b) chord_rows += 16;
            if(final_bar) {
                emit_chord(bar, 0, 0, 16, 92, layer_always);
            } else if(new_chord && pad_sustain) {
                const int vel = k == K::verse ? 62 : k == K::intro ? 70 : 85;
                const uint8_t lay = k == K::verse ? layer_max_high : layer_always;
                const uint16_t gate = k == K::breakdown ? gate_mask_ : 0;
                emit_chord(bar, 0, root, chord_rows - 1, vel, lay, 0, false, gate);
            }
            if(!final_bar && (k == K::chorus || k == K::drop || (k == K::verse && p.variant == 1))) {
                for(int row = 0; row < 16; ++row) {
                    if(!(stab_mask_ & (1u << row))) continue;
                    if(fill && row >= 12) continue;
                    emit_chord(bar, row, root, 1, 92, k == K::verse ? layer_min_high : layer_always, 0, true);
                }
                if(k != K::verse && new_chord) emit_chord(bar, 0, root, chord_rows - 1, 50, layer_max_mid, 0, false, offbeat_drop ? gate_mask_ : 0);
            }

            // fx: crashes, impacts, risers, sweeps
            const bool entry = bi == 0 && (k == K::chorus || k == K::drop || (k == K::verse && index > 1) || k == K::outro || (prev && prev->downlift));
            if(entry) add(bar, 0, ch_fx, {dr_crash}, 16, 100);
            if(bi == 0 && prev && prev->impact) add(bar, 0, ch_fx, {dr_impact}, 16, 110);
            if(bi == 0 && prev && prev->downlift) add(bar, 0, ch_fx, {dr_downlifter}, 32, 96);
            if(bi == 0 && T == tr_filter_open && !last_section) add(bar, 0, ch_fx, {dr_lp_open}, bars * 16 - 1, 100);
            if(!last_section && last_bar) {
                if(T == tr_short_riser) add(bar, 0, ch_fx, {dr_riser}, 16, 92);
                if(p.reverse) add(bar, 16 - std::max(8, gap_rows), ch_fx, {dr_swell}, std::max(8, gap_rows), 96);
            }
            if(T == tr_riser && !last_section) {
                const int len_bars = k == K::breakdown ? std::min(4, bars) : 2;
                if(bi == bars - len_bars) add(bar, 0, ch_fx, {dr_riser}, len_bars * 16, k == K::breakdown ? 100 : 88);
            }
            if(T == tr_filter_close && !last_section) {
                const int len_bars = bars >= 8 ? 2 : 1;
                if(bi == bars - len_bars) add(bar, 0, ch_fx, {dr_lp_close}, len_bars * 16, 100);
            }
            if(final_bar) add(bar, 0, ch_fx, {dr_crash}, 16, 105);
        }

        render_melody(index);
    }

    // Chord voicing with smooth voice leading: of all inversions/octaves
    // inside the pad register, take the one closest to the previous voicing.
    void emit_chord(uint32_t bar, int row, int root, int len, int vel, uint8_t lay, int cut_rows = 0, bool stab = false, uint16_t gate = 0) {
        const int tones = seventh_ && !stab ? 4 : 3;
        int pcs[4];
        for(int i = 0; i < tones; ++i) pcs[i] = pitch(key_, root + 2 * i);
        int best[4] = {}, best_cost = 1 << 30;
        const int lo = pad_lo_, hi = pad_hi_;
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
        const note_fx fx = chip_chords_ ? note_fx::arp : stab ? note_fx::stab : note_fx::none;
        if(tones == 4) add(bar, row, ch_pad, {best[0], best[1], best[2], best[3]}, l, vel, lay, fx);
        else add(bar, row, ch_pad, {best[0], best[1], best[2]}, l, vel, lay, fx);
        s_.events.back().gate = gate;
    }

    // --- melody -------------------------------------------------------------
    // A 2-bar cell of a motif, placed on the harmony from bar `bi`: degrees
    // shift with the chord root (diatonic sequence, wrapped to +-3 steps),
    // plus `steps` (melodic sequence); strong-beat notes snap to chord tones;
    // the ending is chosen by phrase position. Notes go to phrase_, which
    // flush_phrase() fits into the lead's register.
    enum class ending : uint8_t { keep, open, close };
    enum class cell_op : uint8_t {
        keep,       // as written
        invert,     // contour mirrored around the first note
        retro_tail, // bar 2 sings bar 2's pitches backwards
        cadence,    // bar 1 of the motif, then a long resolving note
    };
    struct cell {
        int steps = 0;
        cell_op op = cell_op::keep;
        ending end = ending::keep;
        bool vary = false; // drop an inner note
    };

    // A stable resolution near `deg` that belongs to the chord on `root`:
    // the tonic, else the key's fifth, else its third (else any chord tone).
    [[nodiscard]] int resolve(int deg, int root, bool final = false) const {
        int best = nearest_chord_tone(deg, root), best_v = -100;
        for(int d = -4; d <= 4; ++d) {
            const int c = deg + d;
            if(!is_chord_tone(c, root)) continue;
            const int pc = ((c % 7) + 7) % 7;
            const int stable = pc == 0 ? (final ? 5 : 3) : pc == 4 ? 2 : pc == 2 ? 1 : 0;
            if(!stable) continue;
            const int v = stable * 2 - std::abs(d);
            if(v > best_v) best_v = v, best = c;
        }
        return best;
    }

    void place_motif(motif const& mt, int motif_root, plan_item const& p, int bi, int vel, cell c, int stretch = 1, bool dbl = false) {
        const int bars = static_cast<int>(p.sec.bars);
        if(bi >= bars || mt.empty()) return;
        const int root = chord_at(p, bi);
        int shift = ((root - motif_root) % 7 + 7) % 7;
        if(shift > 3) shift -= 7;
        struct pn {
            int row, len, deg; // row from the cell start (stretched)
        };
        std::vector<pn> notes;
        std::vector<int> tail;
        for(auto const& mn : mt) {
            if(mn.row >= 16) tail.push_back(mn.deg);
        }
        std::size_t tail_i = tail.size();
        int last_row = -1;
        for(std::size_t i = 0; i < mt.size(); ++i) {
            motif_note const& mn = mt[i];
            if(c.vary && i == mt.size() / 2 && mt.size() > 3) continue;
            if(c.op == cell_op::cadence && mn.row >= 16) break;
            int d = mn.deg;
            if(c.op == cell_op::invert && i > 0) d = 2 * mt[0].deg - d;
            if(c.op == cell_op::retro_tail && mn.row >= 16 && tail_i > 0) d = tail[--tail_i];
            const int row = mn.row * stretch;
            if(bi + row / 16 >= bars) break; // the section ends: the last placed note takes the ending
            notes.push_back({row, mn.len * stretch, d + motif_root + shift + c.steps});
            last_row = mn.row;
        }
        // A transformed cell stays in the register of the plain one: an
        // inversion or a sequence never drifts by more than half an octave.
        if(!notes.empty()) {
            float mean = 0, ref = 0;
            for(pn const& n : notes) mean += static_cast<float>(n.deg);
            for(motif_note const& mn : mt) ref += static_cast<float>(mn.deg + motif_root + shift);
            mean /= static_cast<float>(notes.size());
            ref /= static_cast<float>(mt.size());
            const int oct = mean - ref > 3.5f ? -7 : mean - ref < -3.5f ? 7 : 0;
            for(pn& n : notes) n.deg += oct;
        }
        if(c.op == cell_op::cadence) {
            const int r_end = last_row <= 13 ? 16 : 20;
            if(bi + (r_end * stretch) / 16 < bars) {
                const int prev = notes.empty() ? motif_root + shift + c.steps : notes.back().deg;
                if(!notes.empty()) notes.back().len = std::min(notes.back().len, r_end * stretch - notes.back().row);
                notes.push_back({r_end * stretch, (30 - r_end) * stretch, prev});
            }
        }
        for(std::size_t i = 0; i < notes.size(); ++i) {
            pn& n = notes[i];
            const int croot = chord_at(p, bi + n.row / 16);
            const bool strong = (n.row % 16) % 4 == 0;
            if(strong) n.deg = nearest_chord_tone(n.deg, croot);
            // augmented cells: no repeated pitch, move to a neighbour / another chord tone
            if(stretch > 1 && i > 0 && n.deg == notes[i - 1].deg) {
                const int dir = i + 1 < notes.size() && notes[i + 1].deg < n.deg ? -1 : 1;
                n.deg = strong ? nearest_chord_tone(n.deg + 2 * dir, croot) : n.deg + dir;
            }
            if(i + 1 == notes.size() && c.end != ending::keep) {
                const bool section_end = bi + (n.row + n.len) / 16 >= bars - 1;
                if(c.end == ending::close) {
                    n.deg = resolve(n.deg, croot, section_end);
                    n.len = std::max(n.len, 30 * stretch - n.row); // hold the resolution to the cell's end
                } else {
                    // a non-root chord tone leaves the phrase open; not the note just sung
                    const int prev = i > 0 ? notes[i - 1].deg : 1000;
                    for(int d = 0; d <= 3; ++d) {
                        const int a = n.deg + d, b = n.deg - d;
                        auto ok = [&](int x) { return is_chord_tone(x, croot) && ((x - croot) % 7 + 7) % 7 != 0 && x != prev; };
                        if(ok(a)) {
                            n.deg = a;
                            break;
                        }
                        if(ok(b)) {
                            n.deg = b;
                            break;
                        }
                    }
                    n.len = std::max(n.len, std::min(8 * stretch, 30 * stretch - n.row));
                }
            }
            const int room = (bars - bi) * 16 - n.row; // never past the section's end
            n.len = std::clamp(n.len, 1, std::max(1, room));
        }
        ++cell_id_;
        for(pn const& n : notes) {
            phrase_.push_back({p.sec.start_bar + static_cast<uint32_t>(bi + n.row / 16), n.row % 16, n.len, pitch(lead_base_, n.deg), vel, dbl, cell_id_});
        }
    }

    // Emits phrase_ on the lead: the whole phrase moves by octaves into the
    // lead's window (its contour stays), single stray notes fold in by an
    // octave. `high` asks for the window an octave up (second half of a
    // drop); a phrase that would go above C7 there stays in the normal window.
    void flush_phrase(bool high = false) {
        if(phrase_.empty()) return;
        // Each cell takes the octave that keeps its notes in the window and
        // joins the previous note smoothly; a phrase never jumps registers
        // in the middle of a cell.
        const int lo = lead_lo_, hi = lead_hi_;
        std::size_t i = 0;
        int mx = 0;
        while(i < phrase_.size()) {
            std::size_t j = i;
            while(j < phrase_.size() && phrase_[j].cell == phrase_[i].cell) ++j;
            int best = 0, best_cost = 1 << 30;
            for(int sh : {0, 12, -12, 24, -24}) {
                int cost = std::abs(sh) / 12;
                for(std::size_t k = i; k < j; ++k) cost += 100 * (phrase_[k].note + sh < lo - 2 || phrase_[k].note + sh > hi + 3);
                if(last_lead_ >= 0) cost += 3 * std::max(0, std::abs(phrase_[i].note + sh - last_lead_) - 7);
                if(cost < best_cost) best_cost = cost, best = sh;
            }
            for(std::size_t k = i; k < j; ++k) {
                int& note = phrase_[k].note;
                note += best;
                while(note < lo - 2) note += 12; // a stray note folds in
                while(note > hi + 3) note -= 12;
                mx = std::max(mx, note);
            }
            last_lead_ = phrase_[j - 1].note;
            i = j;
        }
        const int up = high && mx + 12 <= 96 ? 12 : 0; // the drop's second half, an octave up
        for(lead_note const& n : phrase_) {
            const int note = n.note + up;
            if(n.dbl && note + 12 <= 96) {
                add(n.bar, n.row, ch_lead, {note}, n.len, n.vel, layer_max_high);
                add(n.bar, n.row, ch_lead, {note, note + 12}, n.len, n.vel, layer_min_high);
            } else {
                add(n.bar, n.row, ch_lead, {note}, n.len, n.vel);
            }
        }
        if(up) last_lead_ = -1;
        phrase_.clear();
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

    // Phrase plans. A 4-cell (8-bar) phrase is statement, sequence,
    // restatement or development, cadence; the last cell of a section always
    // resolves on a held stable tone.
    void render_melody(std::size_t index) {
        using K = section_kind;
        plan_item const& p = plan_[index];
        // before a drum break the melody ends (with its cadence) 4 bars early
        const int bars = static_cast<int>(p.sec.bars) - (index + 1 < plan_.size() && p.out == tr_drum_break ? 4 : 0);
        const int root_a = prog_a_[0], root_b = prog_b_[0];
        const cell develop = dev_invert_ ? cell{0, cell_op::invert, ending::keep, false} : cell{dev_steps_, cell_op::retro_tail, ending::keep, false};
        const cell cadence{cad_steps_, cell_op::cadence, ending::close, false};
        switch(p.sec.kind) {
        case K::intro:
            if(bars >= 8) place_arp(p, bars - 4, 4, 0, 58, layer_min_mid);
            break;
        case K::verse:
            // call (lead, bars 1-2) and response (arp, bars 3-4)
            for(int ph = 0; ph < bars; ph += 4) {
                const int c = (ph / 4) % 4;
                const bool last = ph + 4 >= bars;
                cell cl;
                switch(c) {
                case 0: cl = {0, cell_op::keep, ending::open, false}; break;
                case 1: cl = {0, cell_op::keep, ending::close, p.variant == 1}; break;
                case 2: cl = {seq_step_, cell_op::keep, ending::open, false}; break;
                default: cl = cadence; break;
                }
                if(last) cl = cadence;
                place_motif(motif_a_, root_a, p, ph, 96, cl);
                place_arp(p, ph + 2, 2, 0, 66, layer_always);
                if(c % 2 == 1 || last) flush_phrase();
            }
            break;
        case K::chorus:
        case K::drop:
            // 8-bar melody: motif, its sequence (open), motif or its development, cadence
            for(int ph = 0; ph < bars; ph += 8) {
                const bool high = p.sec.kind == K::drop && ph >= 8;
                const bool second = ph >= 8 || p.variant == 1;
                cell cells[4] = {{0, cell_op::keep, ending::keep, false}, {seq_step_, cell_op::keep, ending::open, true},
                                 second ? develop : cell{0, cell_op::keep, ending::keep, false}, cadence};
                const int n = std::clamp((bars - ph) / 2, 0, 4); // fewer cells before a drum break
                if(n > 0 && n < 4) cells[n - 1] = cadence;          // ... and the last one still resolves
                for(int c = 0; c < n; ++c) place_motif(motif_b_, root_b, p, ph + 2 * c, c % 2 ? 94 : 100, cells[c], 1, true);
                flush_phrase(high);
                place_arp(p, ph, std::min(8, bars - ph), 0, 50, layer_min_mid, -12);
            }
            break;
        case K::bridge:
            // the arp takes the call, the lead answers with sequences of motif A
            for(int ph = 0; ph < bars; ph += 4) {
                place_arp(p, ph, 2, 0, 70, layer_always);
                const bool last = ph + 4 >= bars;
                const int j = ph / 4;
                const cell cl = last ? cadence : j % 2 == 0 ? cell{seq_step_, cell_op::keep, ending::open, true} : cell{dev_steps_, cell_op::invert, ending::open, false};
                place_motif(motif_a_, root_a, p, ph + 2, 88, cl);
                if(j % 2 == 1 || last) flush_phrase();
            }
            break;
        case K::breakdown: {
            // the theme of the harmony it borrows, augmented: a statement,
            // then a sequence a third higher that resolves
            const bool b_theme = p.prog == prog_b_;
            motif const& mt = b_theme ? motif_b_ : motif_a_;
            const int mroot = b_theme ? root_b : root_a;
            const int start = bars >= 16 ? 8 : 0;
            for(int ph = start; ph + 4 <= bars; ph += 4) {
                const int j = (ph - start) / 4;
                const bool last = ph + 8 > bars;
                const cell cl = last ? cell{j == 0 ? 0 : 2, cell_op::cadence, ending::close, false} : cell{j == 0 ? 0 : seq_step_, cell_op::keep, ending::open, false};
                place_motif(mt, mroot, p, ph, 82, cl, 2);
            }
            flush_phrase();
            if(bars >= 16) place_arp(p, 0, 8, 0, 54, layer_min_mid);
            break;
        }
        case K::outro: {
            place_arp(p, 0, std::max(0, bars - 4), 0, 56, layer_min_mid);
            if(bars < 4) break;
            // a last statement, then the cadence: a step onto the tonic, held
            place_motif(motif_a_, root_a, p, bars - 4, 80, {0, cell_op::keep, ending::open, false});
            const int prev_note = phrase_.empty() ? pitch(lead_base_, 0) : phrase_.back().note;
            int approach = cad_deg_ + 2; // the cadence chord's tone nearest the tonic by step
            for(int d : {1, -1, 3, 4}) {
                if(is_chord_tone(d, cad_deg_) && ((pitch(lead_base_, d) - prev_note) % 12 + 12) % 12 != 0) {
                    approach = d;
                    break;
                }
            }
            const int final_deg = approach == 3 || approach == 4 ? 2 : 0;
            int final_note = pitch(lead_base_, final_deg);
            while(final_note < prev_note - 6) final_note += 12;
            while(final_note > prev_note + 6) final_note -= 12;
            int approach_note = pitch(lead_base_, approach) + 12 * ((final_note - pitch(lead_base_, final_deg)) / 12);
            if(approach_note - final_note > 6) approach_note -= 12;
            if(final_note - approach_note > 6) approach_note += 12;
            const uint32_t b = p.sec.start_bar + static_cast<uint32_t>(bars);
            ++cell_id_;
            phrase_.push_back({b - 2, 0, 14, approach_note, 84, false, cell_id_});
            phrase_.push_back({b - 1, 0, 16, final_note, 88, false, cell_id_});
            flush_phrase();
            break;
        }
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
        s_.description = buf + form + "; transitions: " + (transitions_.empty() ? std::string("none") : transitions_);
    }
};

} // namespace

song compose(uint64_t seed, mood m) { return composer(seed, m).run(); }

} // namespace geosim::client::audio
