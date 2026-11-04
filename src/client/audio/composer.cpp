// Procedural composer. docs/14-audio.md#composer explains the musical rules;
// the short version:
//  - style: a family per song (techno, synthwave, amiga punk, chip, breaks,
//    eurodance, lo-fi, hardtrance), weighted by mood; it sets the tempo,
//    the groove templates, bass patterns and sounds;
//  - form: intro / verse / chorus / bridge / breakdown / drop / outro, in
//    8- and 16-bar sections, sized so the song lasts 2-3 minutes, sometimes
//    with a key change for the last chorus;
//  - harmony: one key and mode per song, 4-chord progressions (1 or 2 bars
//    per chord) that may borrow chords (bVI, iv, V major, mediants...),
//    realised by snapping notes into the chord's local scale; pad voicings
//    chosen for smooth voice leading, suspensions;
//  - melody: two 2-bar motifs per song (riff, legato, syncopated or anthem
//    rhythm), placed on the harmony (strong beats snapped to chord tones),
//    answered by varied copies (antecedent ends open, consequent resolves),
//    by evolving arpeggios and by a counter voice; the lead's register is
//    low, mid or high per song (wartime: low, almost a bass);
//  - rhythm: groove templates per section group (verse / chorus), fills,
//    rolls (16ths at most, one beat of 32nds) and drops at section boundaries;
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
    case mood::wartime: return "wartime";
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
    static char const* const names[num_channels] = {"kick", "snare", "hats", "bass", "lead", "arp", "pad", "fx", "counter", "choir"};
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
    case section_kind::groove: return "groove";
    case section_kind::arp_solo: return "arp-solo";
    case section_kind::choir: return "choir";
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

// Chord colours: a chord outside the mode is written as the diatonic chord
// on the same root degree plus a colour, a local scale (tonic offset, mode,
// and the local degree of the chord, -1 = the same degree). Every tonal note
// sounding in that chord's bars is snapped into the local scale, chord tones
// first (docs/14-audio.md#harmony), so the melody logic stays diatonic while
// the harmony borrows: bVI / bVII / iv from the parallel minor, V major from
// harmonic minor, IV major from dorian, bII from phrygian, I major in minor
// (picardy), and the chromatic mediants III and VI major in a major key.
enum color_id : int8_t { c_home, c_minor, c_major, c_harm, c_dorian, c_phryg, c_med3, c_med6, c_count };
struct color_def {
    int offset; // local tonic, semitones above the key
    int mode;   // mode_id (-1: the song's mode)
    int deg;    // the chord's degree in the local scale (-1: its home degree)
};
constexpr color_def colors[c_count] = {{0, -1, -1}, {0, m_aeolian, -1}, {0, m_ionian, -1}, {0, m_harmonic, -1},
                                       {0, m_dorian, -1}, {0, m_phrygian, -1}, {4, m_ionian, 0}, {9, m_mixolydian, 0}};
constexpr char const* color_names[c_count] = {"", "b", "M", "M", "M", "b", "III", "VI"};

// Four-chord progressions as scale degrees (0 = tonic). Uncoloured chords
// are diatonic triads of the modes in the mask that are major or minor (no
// diminished chords); coloured ones are major or minor in their local scale.
struct progression {
    int mode_mask; // bit per mode_id it fits
    int deg[4];
    int8_t col[4] = {c_home, c_home, c_home, c_home};
    bool pivot = false; // centred on the relative key: bridges and breakdowns
    [[nodiscard]] bool colored() const { return col[0] | col[1] | col[2] | col[3]; }
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
    {bit(m_harmonic), {0, 5, 3, 4}},                // i VI iv V
    {bit(m_aeolian) | bit(m_dorian), {0, 3, 6, 2}},  // i iv VII III (IV in dorian)
    {bit(m_dorian), {0, 3, 0, 3}},                  // i IV i IV
    {bit(m_dorian), {0, 6, 3, 0}},                  // i VII IV i
    {bit(m_phrygian), {0, 1, 0, 6}},                // i bII i bVII
    {bit(m_phrygian), {0, 1, 5, 1}},                // i bII bVI bII
    {bit(m_harmonic), {0, 5, 4, 4}},                // i VI V V
    {bit(m_harmonic), {0, 3, 4, 0}},                // i iv V i
    // borrowed and chromatic chords
    {bit(m_ionian) | bit(m_mixolydian) | bit(m_lydian), {0, 5, 6, 0}, {c_home, c_minor, c_minor, c_home}}, // I bVI bVII I ("epic")
    {bit(m_ionian) | bit(m_mixolydian), {0, 3, 0, 4}, {c_home, c_minor, c_home, c_home}},                // I iv I V
    {bit(m_ionian) | bit(m_mixolydian), {3, 3, 0, 0}, {c_home, c_minor, c_home, c_home}},                // IV iv I I
    {bit(m_ionian), {0, 2, 5, 3}, {c_home, c_med3, c_home, c_home}},                                     // I III vi IV (mediant)
    {bit(m_ionian) | bit(m_mixolydian), {0, 5, 1, 4}, {c_home, c_med6, c_home, c_home}},                 // I VI ii V (secondary dominant)
    {bit(m_ionian), {5, 3, 0, 6}, {c_home, c_home, c_home, c_minor}},                                    // vi IV I bVII
    {bit(m_ionian) | bit(m_lydian), {0, 4, 5, 2}, {c_home, c_home, c_home, c_med3}},                     // I V vi III
    {bit(m_ionian), {0, 6, 3, 0}, {c_home, c_minor, c_home, c_home}},                                    // I bVII IV I
    {bit(m_aeolian), {0, 6, 5, 4}, {c_home, c_home, c_home, c_harm}},                                    // i VII VI V (andalusian)
    {bit(m_aeolian) | bit(m_dorian), {0, 3, 4, 0}, {c_home, c_home, c_harm, c_home}},                    // i iv V i
    {bit(m_aeolian), {0, 5, 3, 4}, {c_home, c_home, c_home, c_harm}},                                    // i VI iv V
    {bit(m_aeolian), {0, 2, 6, 3}, {c_home, c_home, c_home, c_dorian}},                                  // i III VII IV
    {bit(m_aeolian), {5, 6, 0, 0}},                                                                      // VI VII i i
    {bit(m_aeolian), {0, 5, 6, 0}, {c_home, c_home, c_home, c_major}},                                   // i VI VII I (picardy)
    {bit(m_aeolian) | bit(m_harmonic), {0, 5, 0, 1}, {c_home, c_home, c_home, c_phryg}},                 // i VI i bII (neapolitan)
    {bit(m_aeolian) | bit(m_phrygian), {0, 3, 5, 4}, {c_home, c_home, c_home, c_harm}},                  // i iv VI V
    // relative-key centred (bridges and breakdowns)
    {bit(m_ionian), {5, 2, 3, 0}, {}, true},                                                             // vi iii IV I
    {bit(m_ionian) | bit(m_mixolydian), {5, 1, 3, 4}, {}, true},                                         // vi ii IV V
    {bit(m_ionian), {5, 3, 2, 2}, {c_home, c_home, c_home, c_med3}, true},                               // vi IV III III
    {bit(m_aeolian) | bit(m_dorian), {2, 6, 3, 2}, {}, true},                                            // III VII iv III
    {bit(m_aeolian), {2, 5, 6, 2}, {}, true},                                                            // III VI VII III
    {bit(m_aeolian) | bit(m_harmonic), {5, 2, 3, 4}, {c_home, c_home, c_home, c_harm}, true},             // VI III iv V
};
static_assert((majorish & minorish) == 0);


struct motif_note {
    int row;  // within the 2-bar cell, 0..31
    int len;  // rows
    int deg;  // scale degree relative to the cell's first chord root
};
using motif = std::vector<motif_note>;

// --- styles, grooves, bass patterns -------------------------------------------
// A song's style family sets its tempo, grooves, bass patterns and sounds;
// the mood picks the family (docs/14-audio.md#styles).

enum style_id : uint8_t { st_techno, st_synthwave, st_punk, st_chip, st_breaks, st_eurodance, st_lofi, st_hardtrance, st_count };
enum groove_id : uint8_t { g_four, g_broken, g_electro, g_breakbeat, g_amen, g_halftime, g_shuffle, g_synthwave, g_punk, g_dbeat, g_chip, g_hardtrance, g_trance, g_count };
enum bass_id : uint8_t { b_offbeat, b_rolling, b_gallop, b_octaves, b_house, b_walking, b_syncop, b_long, b_pedal16, b_bounce, b_pulse8, b_octave16, b_count };
constexpr char const* bass_names[b_count] = {"offbeat", "rolling", "gallop", "octaves", "house", "walking", "syncopated", "long", "pedal", "bounce", "eighths", "octave 16ths"};
enum class mstyle : uint8_t { riff, legato, syncop, anthem };
constexpr char const* mstyle_names[] = {"riff", "legato", "syncopated", "anthem"};

// One drum pattern over a bar of 16ths (bit r = row r).
struct groove_def {
    char const* name;
    uint16_t kick, kick_alt; // kick_alt: the last bar of an 8-bar phrase
    uint16_t snare, ghost;   // backbeat; ghost notes (an intensity layer)
    uint16_t hat_closed, hat_open, hat_extra; // hat_extra: an intensity layer
    int swing;               // ticks on odd 16ths (-1: the song's own)
};
constexpr groove_def grooves[g_count] = {
    {"four", 0x1111, 0x5111, 0x1010, 0x0000, 0x1111, 0x4444, 0xAAAA, -1},      // four-on-the-floor
    {"broken", 0x1041, 0x1441, 0x1010, 0x8000, 0x1111, 0x4444, 0xAAAA, -1},    // 3-3-2 kick
    {"electro", 0x0441, 0x0449, 0x1010, 0x0200, 0x5555, 0x0000, 0xAAAA, 0},    // kick 1, 2a, 3+
    {"breakbeat", 0x0401, 0x0481, 0x1010, 0x8200, 0x1555, 0x4000, 0xAAAA, -1}, // kick 1, 3+
    {"amen", 0x0C05, 0x0405, 0x1010, 0x8280, 0x5555, 0x0000, 0xAAAA, 0},       // the break: kick 1, 1+, 3+, 3a
    {"halftime", 0x0801, 0x0841, 0x0100, 0x4008, 0x5555, 0x0000, 0xAAAA, -1},  // snare on 3
    {"shuffle", 0x0901, 0x0941, 0x1010, 0x8080, 0xFFFF, 0x0000, 0x0000, 2},    // swung 16th hats
    {"synthwave", 0x0101, 0x0501, 0x1010, 0x0000, 0x5555, 0x0000, 0xAAAA, 0},  // kick 1 and 3, big snare
    {"punk", 0x0101, 0x4101, 0x1010, 0x0000, 0x5555, 0x0000, 0x0000, 0},       // kick-snare on the beats, 8th hats
    {"d-beat", 0x0141, 0x4141, 0x1010, 0x0000, 0x5555, 0x0000, 0x0000, 0},     // kick 1, 2+, 3
    {"chip", 0x0181, 0x0581, 0x1010, 0x8000, 0x5555, 0x0000, 0xAAAA, -1},      // kick 1, 2e+, 3
    {"hardtrance", 0x1111, 0x9111, 0x1010, 0x0000, 0xBBBB, 0x4444, 0x0000, 0}, // 16th hats, open off-beats, last-16th kick
    {"trance", 0x1111, 0x5111, 0x1010, 0x0000, 0x0000, 0x4444, 0xBBBB, 0},     // open off-beats only
};

struct style_def {
    char const* name;
    float bpm_lo, bpm_hi;
    uint8_t groove_a[3]; // intro / verse / bridge / outro candidates (the first one twice as likely)
    uint8_t groove_b[3]; // chorus / drop
    uint8_t bass[4];
    float swing_p;
};
constexpr style_def styles[st_count] = {
    {"techno", 116, 128, {g_four, g_broken, g_electro}, {g_four, g_electro, g_broken}, {b_offbeat, b_rolling, b_house, b_octaves}, 0.3f},
    {"synthwave", 88, 108, {g_synthwave, g_halftime, g_four}, {g_synthwave, g_four, g_synthwave}, {b_pulse8, b_pedal16, b_octaves, b_pulse8}, 0.0f},
    {"amiga punk", 152, 172, {g_punk, g_dbeat, g_punk}, {g_dbeat, g_punk, g_punk}, {b_pulse8, b_walking, b_pulse8, b_gallop}, 0.0f},
    {"chip", 124, 146, {g_chip, g_breakbeat, g_four}, {g_chip, g_four, g_chip}, {b_octaves, b_bounce, b_walking, b_pulse8}, 0.25f},
    {"breaks", 136, 156, {g_breakbeat, g_amen, g_halftime}, {g_amen, g_breakbeat, g_amen}, {b_long, b_syncop, b_house, b_long}, 0.2f},
    {"eurodance", 124, 136, {g_four, g_chip, g_four}, {g_four, g_four, g_four}, {b_offbeat, b_octave16, b_rolling, b_octaves}, 0.0f},
    {"lo-fi", 82, 100, {g_halftime, g_shuffle, g_breakbeat}, {g_shuffle, g_halftime, g_breakbeat}, {b_long, b_walking, b_bounce, b_long}, 0.7f},
    {"hardtrance", 138, 148, {g_hardtrance, g_trance, g_hardtrance}, {g_hardtrance, g_hardtrance, g_hardtrance}, {b_offbeat, b_gallop, b_rolling, b_offbeat}, 0.0f},
};

// A bass rhythm over a bar: hits and the role of each hit.
enum bass_role : int8_t { br_root, br_octave, br_fifth, br_below, br_third, br_approach };
struct bass_pat {
    uint16_t mask = 0;
    int8_t role[16]{};
    bool legato = false; // notes last until the next hit
};

// --- the composer -----------------------------------------------------------

class composer {
public:
    composer(uint64_t seed, mood m) : r_(seed), tr_(seed ^ 0xA54FF53A5F1D36F1ull), sr_(seed ^ 0x510E527FADE682D1ull), s_() {
        s_.seed = seed;
        s_.m = m;
    }

    song run() {
        choose_style();
        choose_basics();
        choose_instruments();
        choose_groove();
        choose_form();
        choose_arrangement();
        insert_arp_solo();
        insert_choir();
        plan_bars();
        for(std::size_t i = 0; i < plan_.size(); ++i) {
            if(plan_[i].sec.kind == section_kind::arp_solo) render_arp_solo(i);
            else if(plan_[i].sec.kind == section_kind::choir) render_choir(i);
            else render_section(i);
        }
        apply_mutes();
        s_.events.insert(s_.events.end(), post_.begin(), post_.end());
        std::stable_sort(s_.events.begin(), s_.events.end(), [](note_event const& a, note_event const& b) { return a.tick < b.tick; });
        describe();
        return std::move(s_);
    }

private:
    // A section as planned: its kind plus the harmony it uses.
    struct plan_item {
        section sec;
        progression const* prog = nullptr;
        int bars_per_chord = 1;
        int variant = 0; // 0 first occurrence, 1 repeat (varied)
        bool cadence = false; // last section: its last 2 bars are a cadence chord, then the tonic
        bool end_tonic = false; // the last bar's chord shares no tone with the tonic triad: it becomes the tonic
        int shift = 0;          // semitones above the key (key change for the last chorus)
        int pivot_deg = -1;     // >= 0: the last bar is the next key's dominant (degree and colour)
        int8_t pivot_col = c_home;
        // How this section hands over to the next one (docs/14-audio.md#transitions).
        uint8_t out = 0;      // transition kind (tr_*)
        bool reverse = false; // reverse cymbal into the next downbeat
        bool downlift = false; // downlifter at the start of the next section
        bool impact = false;  // impact boom at the start of the next section
        uint8_t forced_out = 255; // the epic form's fixed hand-overs (255: choose)
        bool quiet = false;     // intro of pads and arps only (no drums or bass)
        bool build_up = false;  // drums and bass enter, layers build (kick + bass, hats, backbeat, melody)
        uint8_t orig_next = 255; // the next section's kind before an arp solo was inserted (renders as before)
    };
    // The harmony of one bar of the song, for snapping coloured chords.
    struct bar_info {
        int root = 0, col = c_home, shift = 0;
        uint16_t scale = 0, chord = 0; // pitch-class masks of the local scale and chord
    };

    rng r_;
    rng tr_; // arrangement choices (transitions, development); separate so the basics stay the same per seed
    // arp devices (the arp solo, the wide arp shapes): their own generator, so
    // a seed keeps every other choice it made before they existed
    rng sr_;
    rng* jit_ = &r_; // the generator of add()'s velocity jitter (the solo's own while it renders)
    song s_;
    bool war_ = false;
    style_id style_ = st_techno;
    int key_ = 57;
    mode_def const* mode_ = &modes[0];
    progression const* prog_a_ = nullptr;
    progression const* prog_b_ = nullptr;
    progression const* prog_c_ = nullptr;
    int bpc_a_ = 2, bpc_b_ = 1;
    int swing_ = 0;          // ticks added to odd rows (the current section's groove)
    int song_swing_ = 0;
    bool seventh_ = false;   // pads use 7th chords
    bool power_ = false;     // pads are power chords (root, fifth, octave): punk
    uint8_t backbeat_ = dr_clap;
    uint16_t ghost_extra_ = 0, kick_extra_ = 0;
    groove_id groove_a_ = g_four, groove_b_ = g_four;
    bass_id bass_a_ = b_offbeat, bass_b_ = b_offbeat;
    bass_pat bpat_a_, bpat_b_;
    bool bass_octave_layer_ = true;
    std::vector<int> arp_order_;
    int arp_rate_ = 1, arp_len_ = 1;
    bool arp_cont_ = false;  // verses: a continuous evolving arp instead of 2-bar answers
    uint16_t stab_mask_ = 0;
    bool chip_chords_ = false;
    bool staccato_ = false;
    mstyle style_a_ = mstyle::riff, style_b_ = mstyle::riff;
    motif motif_a_, motif_b_;
    std::vector<plan_item> plan_;
    std::vector<bar_info> bars_;
    int last_voicing_[4] = {0, 0, 0, 0};
    int voicing_n_ = 0;
    // registers (MIDI notes)
    int lead_class_ = 1;      // 0 low, 1 mid, 2 high (wartime: low, bass-like)
    int pad_lo_ = 52, pad_hi_ = 69, lead_lo_ = 64, lead_hi_ = 83, lead_base_ = 69;
    int arp_base_ = 81, ctr_lo_ = 52, ctr_hi_ = 67;
    int cad_deg_ = 4;         // the final cadence chord (V, bVII or bII by mode)
    int8_t cad_col_ = c_home;
    // the counter voice: 0 none, 1 guide tones, 2 guide tones + fills, 3 gated supersaw chords (wartime)
    int counter_mode_ = 0;
    bool counter_pulse_ = false;
    int last_ctr_ = -1;
    // key change
    bool arp_wide_ = false;   // after an arp solo, the arp keeps its wide cells, pedals and octave jumps
    uint32_t solo_bar_ = ~0u; // the arp solo's first bar (none: ~0)
    bool no_jitter_ = false;  // add() without velocity jitter (events that must not draw random numbers)
    bool epic_ = false;       // the long form after the reference module
    int mod_shift_ = 0;
    uint32_t mod_bar_ = 0;
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
    std::vector<note_event> post_; // device events added after the mutes
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
        if(p.pivot_deg >= 0 && bar == bars - 1) return p.pivot_deg;
        if(p.cadence && bar == bars - 2) return cad_deg_;
        if((p.cadence || p.end_tonic) && bar >= bars - 1) return 0;
        return p.prog->deg[(bar / p.bars_per_chord) % 4];
    }
    [[nodiscard]] int color_at(plan_item const& p, int bar) const {
        const int bars = static_cast<int>(p.sec.bars);
        if(p.pivot_deg >= 0 && bar == bars - 1) return p.pivot_col;
        if(p.cadence && bar == bars - 2) return cad_col_;
        if((p.cadence || p.end_tonic) && bar >= bars - 1) return c_home;
        return p.prog->col[(bar / p.bars_per_chord) % 4];
    }
    // Chord identity (root and colour): IV then iv are two chords.
    [[nodiscard]] int chord_id(plan_item const& p, int bar) const { return chord_at(p, bar) * 16 + color_at(p, bar); }

    // Fills bars_ from the plan: the root, colour and key shift of every bar,
    // and the pitch-class masks used to snap notes in coloured bars.
    void plan_bars() {
        bars_.assign(s_.total_bars, bar_info{});
        for(plan_item const& p : plan_) {
            for(int b = 0; b < static_cast<int>(p.sec.bars); ++b) {
                bar_info& bi = bars_[p.sec.start_bar + static_cast<uint32_t>(b)];
                bi.root = chord_at(p, b);
                bi.col = color_at(p, b);
                bi.shift = p.shift;
                color_def const& c = colors[bi.col];
                mode_def const& md = c.mode < 0 ? *mode_ : modes[c.mode];
                const int tonic = key_ + c.offset;
                const int ldeg = c.deg >= 0 ? c.deg : bi.root;
                for(int i = 0; i < 7; ++i) bi.scale = static_cast<uint16_t>(bi.scale | (1u << ((tonic + md.steps[i]) % 12)));
                for(int i = 0; i < (seventh_ ? 4 : 3); ++i) {
                    const int d = ((ldeg + 2 * i) % 7 + 7) % 7;
                    bi.chord = static_cast<uint16_t>(bi.chord | (1u << ((tonic + md.steps[d]) % 12)));
                }
            }
        }
    }
    // A note of the home scale moved into the bar's local scale (coloured
    // chords): kept if it belongs, else a semitone to a chord tone, else down.
    [[nodiscard]] int snap(int note, uint32_t bar) const {
        if(bars_.empty()) return note;
        bar_info const& b = bars_[std::min<std::size_t>(bar, bars_.size() - 1)];
        if(b.col == c_home) return note;
        auto in = [](uint16_t mask, int n) { return ((mask >> (((n % 12) + 12) % 12)) & 1u) != 0; };
        if(in(b.scale, note)) return note;
        if(in(b.chord, note - 1)) return note - 1;
        if(in(b.chord, note + 1)) return note + 1;
        if(in(b.scale, note - 1)) return note - 1;
        if(in(b.scale, note + 1)) return note + 1;
        return note;
    }
    [[nodiscard]] int shift_at(uint32_t bar) const { return bars_.empty() ? 0 : bars_[std::min<std::size_t>(bar, bars_.size() - 1)].shift; }

    // --- emission -------------------------------------------------------------
    // Tonal notes are written in the home key and realised here: snapped into
    // a coloured chord's scale and moved by the key change. A held lead or
    // counter note that crosses into a bar where it would sound different is
    // split there and re-articulated in the new harmony.
    void realize(note_event& e, uint32_t bar, int const* orig) const {
        for(int i = 0; i < e.count; ++i) e.notes[i] = u8(snap(orig[i], bar) + shift_at(bar));
    }
    void push_tonal(note_event e, int const* orig) {
        const uint32_t bar = e.tick / ticks_per_bar;
        note_event out = e;
        realize(out, bar, orig);
        if((e.ch == ch_lead || e.ch == ch_counter) && e.fx == note_fx::none) {
            for(uint32_t b = bar + 1; b * ticks_per_bar < e.tick + e.len; ++b) {
                note_event t = e;
                realize(t, b, orig);
                if(std::equal(t.notes, t.notes + t.count, out.notes)) continue;
                const uint32_t cut = b * ticks_per_bar;
                out.len = static_cast<uint16_t>(cut - e.tick);
                s_.events.push_back(out);
                e.len = static_cast<uint16_t>(e.tick + e.len - cut);
                e.tick = cut;
                push_tonal(e, orig);
                return;
            }
        }
        s_.events.push_back(out);
    }
    void add(uint32_t bar, int row, uint8_t ch, std::initializer_list<int> notes, int len_rows, int vel, uint8_t lay = layer_always,
             note_fx fx = note_fx::none, int len_ticks_extra = 0) {
        note_event e;
        int tick_in_bar = row * ticks_per_row;
        if((row & 1) && ch != ch_kick) tick_in_bar += swing_;
        e.tick = bar * ticks_per_bar + static_cast<uint32_t>(tick_in_bar);
        e.len = static_cast<uint16_t>(std::max(1, len_rows * ticks_per_row + len_ticks_extra - ((row & 1) ? swing_ : 0)));
        e.ch = ch;
        e.vel = u8(std::clamp(vel + (no_jitter_ ? 0 : jit_->range(-5, 5)), 1, 127));
        e.layer = lay;
        e.fx = fx;
        e.count = 0;
        int orig[4] = {0, 0, 0, 0};
        for(int n : notes) {
            if(e.count < 4) {
                orig[e.count] = n;
                e.notes[e.count++] = u8(n);
            }
        }
        if(s_.patches[ch].kit) s_.events.push_back(e);
        else push_tonal(e, orig);
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

    // weighted pick over n options
    template<typename R>
    static int pick_w(R& g, float const* w, int n) {
        float total = 0;
        for(int i = 0; i < n; ++i) total += w[i];
        float x = g.uniform() * total;
        for(int i = 0; i < n; ++i) {
            if(w[i] <= 0) continue;
            x -= w[i];
            if(x <= 0) return i;
        }
        for(int i = n - 1; i >= 0; --i) {
            if(w[i] > 0) return i;
        }
        return 0;
    }

    // --- song-level choices -------------------------------------------------
    void choose_style() {
        war_ = s_.m == mood::wartime;
        // weights per style (techno, synthwave, punk, chip, breaks, eurodance, lo-fi, hardtrance)
        static constexpr float w[mood_count][st_count] = {
            {1.0f, 1.2f, 0.0f, 0.15f, 0.0f, 0.0f, 2.0f, 0.0f}, // calm: mundane, relaxed (chip rare)
            {1.2f, 1.4f, 0.6f, 0.15f, 1.0f, 0.0f, 0.0f, 0.0f}, // tense
            {1.0f, 1.0f, 1.2f, 0.1f, 1.3f, 0.0f, 0.0f, 0.0f},  // crisis
            {1.0f, 0.6f, 0.4f, 0.3f, 0.0f, 1.5f, 0.0f, 0.0f},  // triumph: the upbeat one
            {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}, // wartime
        };
        style_ = static_cast<style_id>(pick_w(r_, w[static_cast<int>(s_.m)], st_count));
        s_.style = styles[style_].name;
    }

    void choose_basics() {
        const mood m = s_.m;
        style_def const& S = styles[style_];
        // Tempo from the style; calm stays relaxed, triumph stays upbeat.
        float lo = S.bpm_lo, hi = S.bpm_hi;
        if(m == mood::calm) hi = std::min(hi, 120.0f), lo = std::min(lo, hi - 6.0f);
        if(m == mood::triumph) lo = std::max(lo, 112.0f), hi = std::max(hi, lo + 6.0f);
        s_.bpm = std::min(hi, std::round(r_.range(lo, hi + 0.99f))); // (rounding could give hi + 1)
        key_ = 45 + r_.range(0, 11); // tonic between A2 and G#3
        s_.key = key_;
        static constexpr int calm_modes[] = {m_ionian, m_lydian, m_mixolydian, m_dorian, m_ionian};
        static constexpr int tense_modes[] = {m_dorian, m_aeolian, m_aeolian, m_dorian, m_phrygian};
        static constexpr int crisis_modes[] = {m_aeolian, m_phrygian, m_harmonic, m_aeolian};
        static constexpr int triumph_modes[] = {m_ionian, m_mixolydian, m_ionian, m_lydian};
        static constexpr int wartime_modes[] = {m_aeolian, m_phrygian, m_harmonic, m_aeolian, m_dorian};
        int mode = m_ionian;
        switch(m) {
        case mood::calm: mode = r_.pick(calm_modes); break;
        case mood::tense: mode = r_.pick(tense_modes); break;
        case mood::crisis: mode = r_.pick(crisis_modes); break;
        case mood::triumph: mode = r_.pick(triumph_modes); break;
        case mood::wartime: mode = r_.pick(wartime_modes); break;
        }
        s_.mode = mode;
        mode_ = &modes[mode];

        // Lead register, per song: low (baritone, under the pads), mid or high.
        float cls_w[3] = {0.35f, 0.4f, 0.25f};
        if(m == mood::triumph) cls_w[0] = 0.2f, cls_w[2] = 0.35f;
        if(style_ == st_chip) cls_w[2] += 0.15f;
        if(style_ == st_breaks || style_ == st_synthwave) cls_w[0] += 0.1f;
        if(style_ == st_punk) cls_w[1] += 0.2f;
        lead_class_ = war_ ? 0 : pick_w(r_, cls_w, 3);
        // wartime: almost a bass, an octave over the bass line; otherwise a
        // 19-semitone window from A#2 (low), G3 (mid) or D4 (high)
        static constexpr int class_lo[3] = {46, 55, 62};
        lead_lo_ = (war_ ? 42 : class_lo[lead_class_]) + r_.range(0, 2);
        lead_hi_ = lead_lo_ + 19;
        lead_base_ = key_ + 12;
        while(lead_base_ < lead_lo_ - 1) lead_base_ += 12;
        while(lead_base_ > lead_lo_ + 10) lead_base_ -= 12;
        if(war_) pad_lo_ = 57; // the supersaw over the low lead
        else if(lead_class_ == 0) pad_lo_ = 58 + (key_ % 3); // the chords over a low lead
        else pad_lo_ = key_ + 7 <= 61 ? key_ + 7 : key_ - 5; // D3-C#4, under the lead
        pad_hi_ = pad_lo_ + 17;
        arp_base_ = lead_class_ == 2 || war_ ? key_ + 12 : key_ + 24;
        ctr_lo_ = war_ ? 60 : lead_class_ == 0 ? 62 : 50 + (key_ % 3);
        ctr_hi_ = ctr_lo_ + 15;

        switch(mode) {
        case m_mixolydian:
        case m_dorian:
        case m_aeolian: cad_deg_ = 6; break; // bVII - i
        case m_phrygian: cad_deg_ = 1; break; // bII - i
        default: cad_deg_ = 4; break;        // V - I
        }
        // minor songs sometimes close on a major V (harmonic minor) instead
        if((mode == m_aeolian || mode == m_dorian) && r_.chance(m == mood::calm ? 0.15f : 0.4f)) cad_deg_ = 4, cad_col_ = c_harm;
        // Progressions: verse (A), chorus (B, coloured ones likelier outside
        // calm: borrowed chords and mediants carry the emotion), bridge (C,
        // often centred on the relative key).
        std::vector<progression const*> plain, colored, pivot;
        for(auto const& p : progressions) {
            if(!(p.mode_mask & bit(mode))) continue;
            (p.pivot ? pivot : p.colored() ? colored : plain).push_back(&p);
        }
        auto pick_prog = [&](float colored_w) -> progression const* {
            const float wp = static_cast<float>(plain.size()), wc = static_cast<float>(colored.size()) * colored_w;
            if(!colored.empty() && r_.uniform() * (wp + wc) >= wp) return colored[r_.next() % colored.size()];
            if(!plain.empty()) return plain[r_.next() % plain.size()];
            return colored[r_.next() % colored.size()];
        };
        const float emo = m == mood::calm ? 0.4f : 2.0f;
        prog_a_ = pick_prog(m == mood::calm ? 0.3f : 0.8f);
        prog_b_ = pick_prog(emo);
        for(int tries = 0; tries < 4 && prog_b_ == prog_a_; ++tries) prog_b_ = pick_prog(emo);
        prog_c_ = !pivot.empty() && r_.chance(0.6f) ? pivot[r_.next() % pivot.size()] : pick_prog(emo);
        bpc_a_ = r_.chance(0.6f) ? 2 : 1;
        bpc_b_ = r_.chance(0.6f) ? 1 : 2;
        if(style_ == st_synthwave || style_ == st_lofi) bpc_a_ = 2; // slow songs: 2 bars a chord in verses
        song_swing_ = !war_ && r_.chance(styles[style_].swing_p) ? 1 : 0;
        swing_ = song_swing_;
        power_ = style_ == st_punk;
        seventh_ = !power_ && ((m == mood::calm || m == mood::triumph || style_ == st_lofi || style_ == st_synthwave) ? r_.chance(0.6f) : r_.chance(0.25f));
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
        switch(style_) {
        case st_lofi: P[ch_kick].kick_pitch = r_.range(110.0f, 140.0f), P[ch_kick].kick_decay = r_.range(0.22f, 0.3f), P[ch_kick].kick_click = 0.2f; break;
        case st_punk: P[ch_kick].kick_pitch = r_.range(150.0f, 190.0f), P[ch_kick].kick_decay = r_.range(0.18f, 0.26f), P[ch_kick].kick_click = 0.7f; break;
        case st_chip: P[ch_kick].kick_decay = r_.range(0.2f, 0.3f); break;
        case st_hardtrance: {
            // the hardtrance kick: a hard, fast pitch drop from ~280 Hz onto
            // the key's root (45-60 Hz), long body, driven into distortion
            auto& k = P[ch_kick];
            int root = key_ - 24;
            while(440.0f * std::exp2((static_cast<float>(root) - 69.0f) / 12.0f) < 44.0f) root += 12;
            k.kick_end = 440.0f * std::exp2((static_cast<float>(root) - 69.0f) / 12.0f);
            k.kick_pitch = r_.range(250.0f, 320.0f);
            k.kick_sweep = r_.range(0.012f, 0.02f);
            k.kick_decay = r_.range(0.42f, 0.55f);
            k.kick_click = r_.range(0.6f, 0.9f);
            k.kick_drive = r_.range(3.0f, 4.2f);
            break;
        }
        default: break;
        }
        P[ch_snare].kit = true;
        P[ch_snare].snare_tone = r_.range(170.0f, 230.0f);
        P[ch_snare].snare_decay = r_.range(0.12f, 0.2f);
        if(style_ == st_synthwave) P[ch_snare].snare_decay = r_.range(0.28f, 0.36f), P[ch_snare].snare_tone = r_.range(160.0f, 190.0f); // big, gated-reverb snare
        if(style_ == st_punk) P[ch_snare].snare_decay = r_.range(0.12f, 0.16f), P[ch_snare].snare_tone = r_.range(200.0f, 240.0f);
        if(style_ == st_breaks) P[ch_snare].snare_tone = r_.range(210.0f, 250.0f);
        P[ch_hats].kit = true;
        P[ch_hats].hat_decay = r_.range(0.03f, 0.06f);
        P[ch_hats].hat_tone = r_.range(0.8f, 1.25f);
        P[ch_fx].kit = true;
        P[ch_fx].mono = false;

        // bass
        patch& b = P[ch_bass];
        int bass_kind = r_.range(0, 3);
        switch(style_) {
        case st_synthwave: bass_kind = r_.chance(0.6f) ? 3 : 1; break;
        case st_punk:
        case st_chip: bass_kind = r_.chance(0.6f) ? 1 : 2; break;
        case st_breaks: bass_kind = 4; break;
        case st_lofi: bass_kind = r_.chance(0.6f) ? 0 : 2; break;
        case st_hardtrance: bass_kind = r_.range(2, 3); break;
        default: if(m == mood::calm) bass_kind = r_.range(0, 2); break;
        }
        // Sounds aim at late-90s / early-2000s analogue-modelling synths
        // (JP-8000 supersaws, Virus / Nord Lead style leads and basses, warm
        // filtered pads, rounded plucks): band-limited oscillators, unison,
        // 24 dB low-passes with envelopes, soft saturation, chorus. No
        // bit-crushing and no 8-bit wavetables (docs/14-audio.md#synthesis).
        b.glide = 0.0f;
        b.poles4 = true;
        switch(bass_kind) {
        case 0: // warm triangle + sine sub
            b.w1 = wave::triangle, b.w2 = wave::saw, b.mix2 = 0.25f, b.sub = 0.6f;
            b.cutoff = 800, b.resonance = 0.1f, b.fenv = 1.5f, b.fdecay = 0.12f;
            break;
        case 1: // analogue square bass
            b.w1 = wave::pulse, b.duty = 0.5f, b.w2 = wave::pulse, b.mix2 = 0.0f, b.sub = 0.5f;
            b.cutoff = 700, b.resonance = 0.2f, b.fenv = 1.8f, b.fdecay = 0.12f, b.drive = style_ == st_punk ? 0.45f : 0.2f;
            break;
        case 2: // Virus-style saw bass: two saws, sub, envelope on a 24 dB filter
            b.w1 = wave::saw, b.w2 = wave::saw, b.mix2 = 0.8f, b.detune = 5, b.sub = 0.5f;
            b.cutoff = 600, b.resonance = 0.25f, b.fenv = 2.2f, b.fdecay = 0.18f, b.drive = 0.2f;
            break;
        case 3: // resonant saw (acid-lite)
            b.w1 = wave::saw, b.w2 = wave::saw, b.mix2 = 0.5f, b.detune = 7, b.sub = 0.4f;
            b.cutoff = 500, b.resonance = style_ == st_synthwave ? 0.3f : r_.range(0.45f, 0.65f), b.fenv = 3.0f, b.fdecay = 0.14f, b.drive = 0.4f;
            break;
        default: // reese: two detuned saws, dark, held
            b.w1 = wave::saw, b.w2 = wave::saw, b.mix2 = 1.0f, b.detune = r_.range(12.0f, 18.0f), b.sub = 0.6f;
            b.cutoff = 380, b.resonance = 0.2f, b.fenv = 1.2f, b.fdecay = 0.3f, b.drive = 0.3f;
            break;
        }
        b.attack = 0.003f, b.decay = 0.25f, b.sustain = bass_kind == 4 ? 0.85f : 0.6f, b.release = bass_kind == 4 ? 0.12f : 0.06f, b.key_track = 0.3f;

        // lead
        patch& l = P[ch_lead];
        l.poles4 = true;
        if(war_) {
            // wartime: a low, almost bass-like lead instead of a high synth
            switch(r_.range(0, 2)) {
            case 0: // reese: saws a quarter-tone apart, driven
                l.w1 = wave::saw, l.unison = 2, l.spread = 22, l.w2 = wave::saw, l.mix2 = 0.8f, l.detune = r_.range(20.0f, 30.0f), l.sub = 0.25f;
                l.cutoff = 900, l.fenv = 2.2f, l.fdecay = 0.3f, l.resonance = 0.3f, l.drive = 0.25f, l.key_track = 0.4f;
                l.gain = 2.1f;
                break;
            case 1: // hoover-ish: PWM pulse against a detuned saw, gliding
                l.w1 = wave::pulse, l.duty = 0.3f, l.pwm_depth = 0.2f, l.pwm_rate = 0.9f, l.w2 = wave::saw, l.mix2 = 0.8f, l.detune = 15;
                l.cutoff = 1400, l.fenv = 1.4f, l.fdecay = 0.25f, l.resonance = 0.2f, l.drive = 0.3f, l.key_track = 0.4f;
                l.gain = 1.6f;
                break;
            default: // acid: one resonant saw, filter envelope, slides
                l.w1 = wave::saw, l.w2 = wave::saw, l.mix2 = 0.3f, l.detune = 6;
                l.cutoff = 520, l.fenv = 3.2f, l.fdecay = 0.18f, l.resonance = 0.65f, l.drive = 0.5f, l.key_track = 0.3f;
                l.gain = 2.0f;
                break;
            }
            l.attack = 0.005f, l.decay = 0.3f, l.sustain = 0.8f, l.release = 0.12f;
            l.vib_depth = r_.range(0.0f, 0.06f), l.vib_rate = 5.0f, l.vib_delay = 0.3f;
            l.glide = r_.chance(0.7f) ? 0.05f : 0.0f;
        } else {
            static constexpr int prefs[st_count][4] = {{0, 1, 2, 3}, {1, 0, 1, 2}, {2, 0, 1, 3}, {0, 2, 3, 1}, {1, 0, 2, 1}, {1, 0, 2, 3}, {0, 2, 3, 0}, {1, 1, 1, 1}};
            // 7aa4c09's table: its "chip sample" lead (kind 2) drew one more number
            static constexpr int prefs_7aa[st_count][4] = {{0, 1, 2, 3}, {1, 0, 1, 3}, {3, 0, 1, 3}, {2, 0, 2, 3}, {1, 0, 3, 1}, {3, 0, 2, 1}, {0, 2, 3, 0}, {1, 1, 1, 1}};
            const int pick = r_.range(0, 3);
            const int lead_kind = prefs[style_][pick];
            const bool old_draw = prefs_7aa[style_][pick] == 2;
            switch(lead_kind) {
            case 0: // PWM pulse pair, warm
                l.w1 = wave::pulse, l.w2 = wave::pulse, l.mix2 = 0.4f, l.detune = 9, l.duty = 0.35f, l.pwm_depth = 0.12f, l.pwm_rate = 0.7f;
                l.cutoff = 2200, l.fenv = 1.0f, l.resonance = 0.15f;
                l.gain = 1.15f;
                break;
            case 1: // supersaw lead (JP-8000 style)
                l.w1 = wave::saw, l.unison = 2, l.spread = 18, l.w2 = wave::saw, l.mix2 = 0.5f, l.detune = 7;
                l.cutoff = 2300, l.fenv = 1.2f, l.resonance = 0.2f;
                l.gain = 2.0f;
                break;
            case 2: // Nord-style lead: saw with a quiet square an octave up, resonant sweep, glide
                l.w1 = wave::saw, l.w2 = wave::pulse, l.duty = 0.5f, l.mix2 = 0.25f, l.semi2 = 12;
                l.cutoff = 1800, l.fenv = 1.6f, l.fdecay = 0.35f, l.resonance = 0.35f, l.drive = 0.15f;
                l.gain = 1.6f;
                break;
            default: // square + fifth, organ-like, filtered
                l.w1 = wave::pulse, l.duty = 0.5f, l.w2 = wave::triangle, l.mix2 = 0.5f, l.semi2 = 7;
                l.cutoff = 1800, l.fenv = 0.8f;
                l.gain = 1.15f;
                break;
            }
            if(old_draw) (void)r_.chance(0.5f); // was the wavetable choice: kept so later draws stay the same
            if(lead_class_ == 0) l.cutoff *= 1.4f, l.key_track = 0.3f, l.drive = l.unison ? l.drive : std::max(l.drive, 0.15f); // a low lead keeps its edge (unison saws need no drive)
            l.attack = 0.015f, l.decay = 0.3f, l.sustain = 0.65f, l.release = 0.15f;
            l.vib_depth = r_.range(0.05f, 0.12f), l.vib_rate = r_.range(4.5f, 5.5f), l.vib_delay = 0.25f;
            l.glide = r_.chance(lead_kind == 2 ? 0.8f : 0.5f) ? 0.045f : 0.0f;
        }
        l.fdecay = std::max(l.fdecay, 0.25f);
        staccato_ = r_.chance(war_ ? 0.2f : m == mood::crisis ? 0.2f : 0.1f);

        // arp: rounded plucks (a filter envelope on a 24 dB low-pass), or
        // (slower rates) short sustained notes
        patch& a = P[ch_arp];
        const int arp_kind = style_ == st_chip ? 2 : r_.range(0, 2); // (the draws keep 7aa4c09's order: same seed, same song)
        a.w1 = arp_kind == 0 ? wave::pulse : wave::saw;
        a.duty = r_.chance(0.5f) ? 0.5f : 0.35f;
        if(arp_kind == 2) a.unison = 2, a.spread = 14; // trance pluck
        a.w2 = wave::pulse, a.mix2 = 0.0f;
        a.attack = 0.003f, a.decay = r_.range(0.12f, 0.2f), a.sustain = 0.0f, a.release = 0.08f;
        a.cutoff = 900, a.fenv = 2.6f, a.fdecay = 0.12f, a.resonance = 0.25f, a.poles4 = true;
        a.gain = arp_kind == 2 ? 1.5f : 1.1f;

        // pad / chords
        patch& p = P[ch_pad];
        p.poles4 = true;
        // tracker-arpeggio chords only in the (rare) chip family
        // 7aa4c09 drew chip chords for many families; the same draw now only
        // keeps them in the chip family (and keeps every later draw in place)
        float chip_p = m == mood::calm ? 0.25f : 0.4f;
        if(style_ == st_chip) chip_p = 0.75f;
        if(style_ == st_synthwave || style_ == st_punk || war_) chip_p = 0.0f;
        if(style_ == st_breaks) chip_p = 0.15f;
        const bool old_chip = r_.chance(chip_p);
        chip_chords_ = old_chip && style_ == st_chip;
        if(chip_chords_) { // arpeggiated chord on a soft square
            p.w1 = wave::pulse, p.duty = 0.5f, p.w2 = wave::pulse, p.mix2 = 0.0f;
            p.cutoff = 1600, p.resonance = 0.1f;
            p.attack = 0.008f, p.decay = 0.3f, p.sustain = 0.5f, p.release = 0.25f;
        } else if(power_) { // a sampled-synth "guitar": driven saw + pulse power chords, chugs in the chorus
            p.w1 = wave::saw, p.unison = 1, p.spread = 10, p.w2 = wave::pulse, p.duty = 0.4f, p.mix2 = 0.6f, p.detune = 6;
            p.cutoff = 1800, p.resonance = 0.15f, p.fenv = 0.8f, p.fdecay = 0.2f, p.drive = 0.45f;
            if(!old_chip) (void)r_.chance(0.5f); // was the bit-crush choice: kept so later draws stay the same
            p.attack = 0.004f, p.decay = 0.3f, p.sustain = 0.75f, p.release = 0.12f;
        } else { // supersaw pad: unison saws, warm filter, chorus in the mixer
            p.w1 = wave::saw, p.unison = 2, p.spread = 16, p.w2 = wave::saw, p.mix2 = 0.7f, p.detune = old_chip ? 11.0f : r_.range(8.0f, 14.0f);
            p.cutoff = m == mood::crisis ? 900.0f : 1300.0f, p.resonance = 0.15f, p.fenv = 0.6f, p.fdecay = 0.6f;
            p.attack = 0.08f, p.decay = 0.6f, p.sustain = 0.8f, p.release = 0.6f;
            p.gain = 1.3f;
            if(war_) { // the JP-8000 supersaw: wide and bright, for pads and stabs
                p.unison = 4, p.spread = 28;
                p.mix2 = 0.6f;
                p.cutoff = 2100.0f;
                p.fenv = 1.0f;
                p.gain = 1.5f;
            }
            if(style_ == st_synthwave) p.unison = 3, p.spread = 18, p.detune = r_.range(10.0f, 16.0f), p.cutoff = 1600.0f, p.attack = 0.25f, p.release = 0.9f, p.sustain = 0.85f; // lush
            p.pwm_depth = 0.0f;
        }
        p.mono = true;
        p.key_track = 0.2f;

        // counter voice
        patch& c = P[ch_counter];
        c.poles4 = true;
        int ctr_kind = 0; // 0 strings, 1 brass, 2 soft square, 3 supersaw
        if(war_) ctr_kind = 3;
        else if(style_ == st_chip || style_ == st_eurodance) ctr_kind = 2;
        else if(style_ == st_punk || style_ == st_breaks || (style_ == st_techno && m != mood::calm)) ctr_kind = r_.chance(0.6f) ? 1 : 0;
        switch(ctr_kind) {
        case 0: // string machine
            c.w1 = wave::saw, c.unison = 1, c.spread = 12, c.w2 = wave::saw, c.mix2 = 0.8f, c.detune = 7;
            c.attack = 0.2f, c.decay = 0.5f, c.sustain = 0.85f, c.release = 0.5f, c.cutoff = 1400, c.fenv = 0.3f, c.fdecay = 0.5f;
            c.vib_depth = 0.04f, c.vib_rate = 5.0f, c.vib_delay = 0.3f, c.gain = 1.7f;
            break;
        case 1: // synth brass
            c.w1 = wave::saw, c.w2 = wave::pulse, c.duty = 0.4f, c.mix2 = 0.6f, c.detune = 5;
            c.attack = 0.04f, c.decay = 0.3f, c.sustain = 0.7f, c.release = 0.18f, c.cutoff = 900, c.fenv = 1.6f, c.fdecay = 0.25f, c.drive = 0.15f;
            c.gain = 1.4f;
            break;
        case 2: // soft square with a triangle an octave up
            c.w1 = wave::pulse, c.duty = 0.5f, c.w2 = wave::triangle, c.semi2 = 12, c.mix2 = 0.3f;
            c.attack = 0.012f, c.decay = 0.25f, c.sustain = 0.6f, c.release = 0.15f, c.cutoff = 1600, c.fenv = 0.5f;
            c.vib_depth = 0.06f, c.vib_rate = 5.0f, c.vib_delay = 0.3f, c.gain = 1.1f;
            break;
        default: // supersaw for the gated wartime chords
            c.w1 = wave::saw, c.unison = 3, c.spread = 0.6f * r_.range(22.0f, 30.0f), c.w2 = wave::saw, c.mix2 = 0.5f, c.detune = 7; // 13-18 cents: wide but in tune
            c.attack = 0.004f, c.decay = 0.2f, c.sustain = 0.85f, c.release = 0.07f, c.cutoff = 2400, c.resonance = 0.1f, c.fenv = 0.5f, c.fdecay = 0.2f;
            c.gain = 1.8f;
            break;
        }
        c.key_track = 0.3f;
        c.mono = true;
        if(war_) counter_mode_ = 3;
        else if(m == mood::calm) counter_mode_ = r_.chance(0.5f) ? 1 : 0;
        else counter_mode_ = r_.chance(0.55f) ? 2 : 1;
        counter_pulse_ = m != mood::calm && r_.chance(0.4f);

        // mix (levels tuned by ear on offline renders, see docs/14-audio.md)
        const float g[num_channels] = {war_ ? 0.8f : 0.72f, 0.8f, 0.45f, 0.62f, lead_class_ == 0 ? 0.42f : 0.36f, 0.45f, chip_chords_ ? 0.32f : 0.45f, 0.4f, war_ ? 0.3f : 0.28f, 0.36f};
        const float pan[num_channels] = {0.0f, 0.05f, 0.3f, 0.0f, 0.15f, -0.4f, -0.05f, 0.0f, -0.25f, 0.0f};
        const float dly[num_channels] = {0.0f, 0.04f, 0.06f, 0.0f, 0.28f, 0.32f, 0.12f, 0.1f, 0.18f, 0.1f};
        const float rev[num_channels] = {0.02f, 0.18f, 0.06f, 0.0f, 0.2f, 0.15f, 0.35f, 0.35f, 0.3f, 0.45f};
        const float duck[num_channels] = {0.0f, 0.0f, 0.15f, 0.55f, war_ ? 0.35f : 0.15f, 0.3f, 0.6f, 0.0f, war_ ? 0.6f : 0.35f, 0.3f};
        for(int ch = 0; ch < num_channels; ++ch) {
            X.gain[ch] = g[ch];
            X.pan[ch] = ch == ch_arp && r_.chance(0.5f) ? -pan[ch] : pan[ch];
            X.delay[ch] = dly[ch];
            X.reverb[ch] = rev[ch] * (m == mood::calm ? 1.3f : 1.0f);
            X.duck[ch] = duck[ch];
        }
        if(X.pan[ch_arp] > 0) X.pan[ch_lead] = -X.pan[ch_lead];
        X.pan[ch_counter] = -0.8f * X.pan[ch_lead];
        if(style_ == st_synthwave) X.reverb[ch_snare] = 0.45f, X.reverb[ch_pad] = 0.45f;
        // ensemble chorus on the chords and the counter voice, a touch on the arp
        X.chorus[ch_pad] = chip_chords_ ? 0.2f : style_ == st_synthwave ? 0.7f : 0.5f;
        X.chorus[ch_counter] = ctr_kind == 0 ? 0.6f : 0.35f;
        X.chorus[ch_arp] = 0.2f;
        // the low end belongs to the kick and bass: high-pass the chords,
        // arp and counter, and the lead below its lowest notes
        X.highpass[ch_pad] = 150.0f;
        X.highpass[ch_arp] = 180.0f;
        X.highpass[ch_counter] = 140.0f;
        X.highpass[ch_lead] = war_ ? 70.0f : 110.0f;
        X.highpass[ch_choir] = 180.0f;
        X.chorus[ch_choir] = 0.6f;
        X.bright[ch_choir] = true;
        // the choir (used only by a choir block; no random draws): unison
        // saws through "aah" formants, slow attack, gentle vibrato
        patch& ch = P[ch_choir];
        ch.w1 = wave::saw, ch.unison = 2, ch.spread = 9, ch.w2 = wave::saw, ch.mix2 = 0.7f, ch.detune = 6;
        ch.formant = 1, ch.poles4 = true, ch.cutoff = 5000, ch.resonance = 0.0f, ch.fenv = 0.0f, ch.key_track = 0.0f;
        ch.attack = 0.35f, ch.decay = 0.5f, ch.sustain = 0.9f, ch.release = 0.8f;
        ch.vib_depth = 0.12f, ch.vib_rate = 5.2f, ch.vib_delay = 0.2f;
        ch.gain = 1.6f; // featured, but under the kick: about -17 dBFS on its own in the choir drop
        ch.mono = true;
        X.bright[ch_lead] = X.bright[ch_arp] = X.bright[ch_pad] = X.bright[ch_counter] = true;
        X.delay_rows = r_.chance(0.7f) ? 3.0f : 2.0f;
        X.delay_feedback = r_.range(0.3f, 0.45f);
        X.reverb_size = m == mood::calm ? 0.85f : 0.75f;
    }

    bass_pat make_bass(bass_id id) {
        bass_pat b;
        auto set = [&](std::initializer_list<int> rows, std::initializer_list<int> roles) {
            auto ro = roles.begin();
            for(int r : rows) {
                b.mask = static_cast<uint16_t>(b.mask | (1u << r));
                b.role[r] = static_cast<int8_t>(ro != roles.end() ? *ro++ : br_root);
            }
        };
        switch(id) {
        case b_offbeat: b.mask = 0x4444; break;
        case b_rolling:
            b.mask = 0xEEEE;
            for(int i = 3; i < 16; i += 4) b.role[i] = r_.chance(0.3f) ? br_octave : br_root;
            break;
        case b_gallop:
            b.mask = 0xCCCC;
            b.role[15] = r_.chance(0.5f) ? br_octave : br_root;
            break;
        case b_octaves:
            b.mask = 0x5555;
            for(int i = 0; i < 16; ++i) b.role[i] = static_cast<int8_t>((i / 2) % 2 ? br_octave : br_root);
            break;
        case b_house: {
            // 4-6 hits, always one near the downbeat
            b.mask = r_.chance(0.5f) ? 0x0001 : 0x0004;
            const int hits = r_.range(4, 6);
            while(__builtin_popcount(b.mask) < hits) b.mask = static_cast<uint16_t>(b.mask | (1u << r_.range(1, 15)));
            static constexpr int8_t roles[] = {br_root, br_root, br_octave, br_fifth, br_root, br_below};
            for(int i = 2; i < 16; ++i) b.role[i] = r_.pick(roles);
            break;
        }
        case b_walking:
            set({0, 4, 8, 12}, {br_root, br_third, br_fifth, br_approach});
            b.legato = true;
            break;
        case b_syncop: set({0, 3, 6, 8, 11, 14}, {br_root, br_root, br_fifth, br_octave, br_root, br_approach}); break;
        case b_long:
            set({0, 14}, {br_root, br_approach});
            if(r_.chance(0.5f)) set({0, 10, 14}, {br_root, br_fifth, br_approach});
            b.legato = true;
            break;
        case b_pedal16:
            b.mask = 0xFFFF;
            b.role[14] = br_octave;
            break;
        case b_bounce: set({0, 3, 8, 11}, {br_root, br_octave, br_root, br_octave}); break;
        case b_pulse8:
            b.mask = 0x5555;
            if(r_.chance(0.5f)) b.role[12] = br_fifth;
            if(r_.chance(0.4f)) b.role[14] = br_octave;
            break;
        case b_octave16:
            b.mask = 0xFFFF;
            for(int i = 0; i < 16; ++i) b.role[i] = static_cast<int8_t>(i % 2 ? br_octave : br_root);
            break;
        default: break;
        }
        return b;
    }

    void choose_groove() {
        const mood m = s_.m;
        style_def const& S = styles[style_];
        static constexpr float first_twice[3] = {2.0f, 1.0f, 1.0f};
        groove_a_ = static_cast<groove_id>(S.groove_a[pick_w(r_, first_twice, 3)]);
        groove_b_ = r_.chance(0.35f) ? groove_a_ : static_cast<groove_id>(S.groove_b[pick_w(r_, first_twice, 3)]);
        if(war_) groove_b_ = g_hardtrance;
        // a song's own extra kick (not in four-on-the-floor trance or punk)
        if(!war_ && style_ != st_punk && r_.chance(0.35f)) {
            static constexpr int rows[] = {3, 7, 11, 14};
            kick_extra_ = static_cast<uint16_t>(1u << r_.pick(rows));
        }
        static constexpr uint16_t ghost_sets[] = {0x0080, 0x8000, 0x8400, 0x4080, 0x0000, 0x8808};
        ghost_extra_ = war_ || style_ == st_punk ? 0 : r_.pick(ghost_sets);
        float clap_p = 0.7f;
        switch(style_) {
        case st_synthwave: clap_p = 0.2f; break;
        case st_punk: clap_p = 0.05f; break;
        case st_chip: clap_p = 0.5f; break;
        case st_breaks: clap_p = 0.25f; break;
        case st_lofi: clap_p = 0.3f; break;
        case st_hardtrance: clap_p = 0.85f; break;
        default: break;
        }
        backbeat_ = r_.chance(clap_p) ? dr_clap : dr_snare;

        // bass rhythms (verse and chorus may differ); calm avoids the busy ones
        auto pick_bass = [&] {
            bass_id id = static_cast<bass_id>(S.bass[r_.range(0, 3)]);
            if(m == mood::calm && (id == b_pedal16 || id == b_octave16 || id == b_rolling || id == b_gallop)) id = r_.chance(0.5f) ? b_long : b_house;
            return id;
        };
        bass_a_ = pick_bass();
        bass_b_ = r_.chance(0.45f) ? bass_a_ : pick_bass();
        bpat_a_ = make_bass(bass_a_);
        bpat_b_ = bass_b_ == bass_a_ ? bpat_a_ : make_bass(bass_b_);
        bass_octave_layer_ = style_ == st_techno || style_ == st_eurodance || style_ == st_hardtrance || style_ == st_chip;

        // arpeggio order over chord tones (0 root, 1 third, 2 fifth, 3 octave, 4 octave+third)
        static constexpr int orders[][8] = {{0, 1, 2, 3, -1}, {3, 2, 1, 0, -1}, {0, 1, 2, 3, 2, 1, -1}, {0, 2, 1, 3, -1},
                                            {0, 2, 3, 1, 2, 3, 0, 2}, {0, 3, 1, 3, 2, 3, -1}, {0, 2, 4, 2, -1}, {2, 0, 3, 0, 4, 0, -1},
                                            {0, 1, 2, -1}, {0, 2, 3, 4, 3, -1}, {0, 2, 1, 3, 2, 4, 3, -1}};
        auto const& o = orders[r_.next() % 11];
        for(int v : o) {
            if(v < 0) break;
            arp_order_.push_back(v);
        }
        // rate: 16ths, 8ths or dotted 8ths (a 3-row cycle that drifts across the bar)
        if(m == mood::calm) arp_rate_ = r_.chance(0.5f) ? 2 : 1;
        else if(style_ == st_chip || war_) arp_rate_ = r_.chance(0.75f) ? 1 : 3;
        else {
            static constexpr int rates[] = {1, 1, 2, 3, 3};
            arp_rate_ = r_.pick(rates);
        }
        arp_len_ = arp_rate_ == 1 ? 1 : r_.chance(0.6f) ? arp_rate_ : 1;
        if(arp_len_ > 1) { // held arp notes: a little sustain
            s_.patches[ch_arp].sustain = 0.3f;
            s_.patches[ch_arp].decay = 0.25f;
            s_.patches[ch_arp].release = 0.12f;
        }
        arp_cont_ = m != mood::calm && r_.chance(0.5f);

        // chord stabs
        switch(style_) {
        case st_punk: {
            static constexpr uint16_t st[] = {0x5555, 0x5455, 0x5555};
            stab_mask_ = r_.pick(st);
            break;
        }
        case st_eurodance: {
            static constexpr uint16_t st[] = {0x4444, 0x5494, 0x4444};
            stab_mask_ = r_.pick(st);
            break;
        }
        case st_synthwave:
        case st_lofi: stab_mask_ = 0; break; // held chords
        case st_breaks: {
            static constexpr uint16_t st[] = {0x0909, 0x2492, 0x0490};
            stab_mask_ = r_.pick(st);
            break;
        }
        default: {
            static constexpr uint16_t stabs[] = {0x5494 /* 2,4,7,10,12,14 (reference) */, 0x4444, 0x1249, 0x4924, 0x2492};
            stab_mask_ = r_.pick(stabs);
            break;
        }
        }

        // motifs: a rhythmic approach per song and per theme
        static constexpr float mw[mood_count][2][4] = {
            // A: riff, legato, syncop, anthem; B: riff, legato, syncop, anthem
            {{0.35f, 0.45f, 0.2f, 0.0f}, {0.2f, 0.5f, 0.1f, 0.2f}}, // calm
            {{0.35f, 0.3f, 0.35f, 0.0f}, {0.15f, 0.3f, 0.2f, 0.35f}}, // tense
            {{0.4f, 0.25f, 0.35f, 0.0f}, {0.3f, 0.3f, 0.0f, 0.4f}}, // crisis
            {{0.35f, 0.3f, 0.35f, 0.0f}, {0.2f, 0.3f, 0.0f, 0.5f}}, // triumph
            {{0.4f, 0.2f, 0.4f, 0.0f}, {0.0f, 0.35f, 0.35f, 0.3f}}, // wartime
        };
        style_a_ = static_cast<mstyle>(pick_w(r_, mw[static_cast<int>(m)][0], 4));
        style_b_ = static_cast<mstyle>(pick_w(r_, mw[static_cast<int>(m)][1], 4));
        const float density = m == mood::calm ? 0.8f : m == mood::crisis ? 1.3f : war_ ? 1.15f : 1.0f;
        motif_a_ = make_motif(density, prog_a_->deg[0], prog_a_->deg[bpc_a_ == 1 ? 1 : 0], style_a_);
        motif_b_ = make_motif(density * 1.2f, prog_b_->deg[0], prog_b_->deg[bpc_b_ == 1 ? 1 : 0], style_b_);
    }

    // A 2-bar motif. Rhythm by style: riff (weighted rows, strong beats
    // likelier), legato (few onsets, each held to the next), syncopated
    // (3-3-2 cells), anthem (long notes and wide leaps). Pitch: a stepwise
    // contour with leap recovery, strong beats on chord tones; riffs leave
    // the last 4 rows empty so the answer can breathe.
    motif make_motif(float density, int root0, int root1, mstyle st) {
        motif out;
        bool used[32] = {};
        int span = 28;
        if(st == mstyle::riff) {
            const int target = std::clamp(static_cast<int>(std::round(r_.range(4.0f, 6.5f) * density)), 3, 10);
            auto weight = [&](int row) {
                if(row % 8 == 0) return 5.0f;
                if(row % 4 == 0) return 3.0f;
                if(row % 2 == 0) return 2.2f;
                return 0.6f * density;
            };
            const bool repeat_rhythm = r_.chance(0.45f);
            span = repeat_rhythm ? 16 : 28;
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
        } else {
            static constexpr int legato_r[][6] = {{0, 6, 12, 16, 22, -1}, {0, 8, 14, 20, -1}, {0, 4, 10, 16, 24, -1}, {2, 8, 14, 18, 24, -1}, {0, 10, 16, 22, -1}};
            static constexpr int syncop_r[][8] = {{0, 3, 6, 8, 11, 14, 16, 22}, {0, 3, 6, 10, 12, 16, 19, 22}, {0, 2, 6, 8, 10, 14, 16, 24},
                                                  {0, 3, 6, 12, 16, 19, 22, -1}, {0, 6, 8, 11, 14, 16, 22, -1}};
            static constexpr int anthem_r[][5] = {{0, 6, 8, 16, -1}, {0, 8, 12, 16, 24}, {0, 4, 8, 16, -1}, {0, 12, 16, 22, -1}, {0, 8, 16, -1, -1}};
            int const* rows = nullptr;
            int n = 0;
            if(st == mstyle::legato) rows = legato_r[r_.next() % 5], n = 6;
            else if(st == mstyle::syncop) rows = syncop_r[r_.next() % 5], n = 8;
            else rows = anthem_r[r_.next() % 5], n = 5;
            for(int i = 0; i < n && rows[i] >= 0; ++i) used[rows[i]] = true;
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
                    int mag = x < 0.55f ? 1 : x < 0.8f ? 2 : x < 0.9f ? 0 : r_.range(3, 4);
                    if(st == mstyle::anthem) mag = x < 0.3f ? 1 : x < 0.55f ? 2 : x < 0.9f ? r_.range(3, 5) : 0; // wide, singing intervals
                    if(st == mstyle::legato && mag == 0) mag = 1;
                    step = r_.chance(0.5f) ? mag : -mag;
                }
                deg += step;
                if(deg > 7) deg -= 2 * std::abs(step);
                if(deg < -3) deg += 2 * std::abs(step);
                prev_step = step;
            }
            const int root = row < 16 ? root0 : root1;
            if(row % 4 == 0) deg = nearest_chord_tone(deg + root0, root) - root0;
            const int next_row = i + 1 < rows.size() ? rows[i + 1] : (st == mstyle::riff ? 32 : 30);
            int len = next_row - row;
            if(st == mstyle::riff) len = std::min(len, staccato_ ? 2 : 6);
            else if(st == mstyle::syncop) len = std::min(len, 6);
            else len = std::min(len, 14); // legato / anthem: held notes
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
            uint8_t out = 255; // a fixed hand-over (epic form)
            uint8_t flag = 0;  // 1 quiet intro, 2 build-up
        };
        static const std::vector<item> forms[] = {
            {{K::intro, 8}, {K::verse, 16}, {K::chorus, 16}, {K::bridge, 8}, {K::breakdown, 8}, {K::drop, 16}, {K::outro, 8}},
            {{K::intro, 8}, {K::verse, 16}, {K::chorus, 16}, {K::breakdown, 16}, {K::drop, 16}, {K::outro, 8}},
            {{K::intro, 16}, {K::verse, 16}, {K::chorus, 16}, {K::breakdown, 8}, {K::verse, 8}, {K::drop, 16}, {K::outro, 8}},
            {{K::intro, 8}, {K::verse, 16}, {K::chorus, 8}, {K::verse, 8}, {K::chorus, 16}, {K::breakdown, 8}, {K::drop, 16}, {K::outro, 8}},
            // song forms: verse / chorus twice, a bridge, the last chorus
            {{K::intro, 8}, {K::verse, 16}, {K::chorus, 16}, {K::verse, 8}, {K::chorus, 16}, {K::bridge, 8}, {K::chorus, 16}, {K::outro, 8}},
            {{K::intro, 8}, {K::verse, 8}, {K::verse, 8}, {K::chorus, 16}, {K::breakdown, 8}, {K::chorus, 16}, {K::outro, 8}},
            {{K::intro, 8}, {K::chorus, 8}, {K::verse, 16}, {K::bridge, 8}, {K::drop, 16}, {K::outro, 8}},
            // club form: drop, breakdown, drop
            {{K::intro, 16}, {K::drop, 16}, {K::breakdown, 16}, {K::drop, 16}, {K::outro, 8}},
        };
        // wartime: trance forms with long (16/32-bar) breakdowns and builds, always full length
        static const std::vector<item> war_forms[] = {
            {{K::intro, 16}, {K::verse, 16}, {K::breakdown, 16}, {K::drop, 32}, {K::outro, 8}},
            {{K::intro, 8}, {K::verse, 16}, {K::chorus, 16}, {K::breakdown, 16}, {K::drop, 16}, {K::outro, 8}},
            {{K::intro, 16}, {K::chorus, 16}, {K::breakdown, 32}, {K::drop, 16}, {K::outro, 8}},
            {{K::intro, 8}, {K::verse, 16}, {K::breakdown, 16}, {K::drop, 16}, {K::breakdown, 8}, {K::drop, 16}, {K::outro, 8}},
            {{K::intro, 8}, {K::drop, 16}, {K::breakdown, 16}, {K::drop, 16}, {K::outro, 8}},
            {{K::intro, 16}, {K::verse, 8}, {K::chorus, 8}, {K::breakdown, 16}, {K::drop, 32}, {K::outro, 8}},
        };
        std::vector<item> form = war_ ? war_forms[r_.next() % 6] : forms[r_.next() % 8];
        const double bar_s = 240.0 / static_cast<double>(s_.bpm);
        // Length: 2-3.5 minutes, or (some songs, most often in wartime: trance
        // tracks run long) a long song of 5-10 minutes built from extra
        // verse / chorus or breakdown / drop cycles.
        static constexpr float long_p[mood_count] = {0.1f, 0.15f, 0.15f, 0.1f, 0.35f};
        const bool long_song = r_.chance(long_p[static_cast<int>(s_.m)]);
        const double target = long_song ? r_.range(300.0f, 580.0f) : r_.range(140.0f, 195.0f);
        const double max_s = long_song ? 600.0 : 210.0;
        // About one song in four is a short intermission (30-80 s). Its own
        // generator, so a full-length song's basics do not depend on it.
        rng ir(s_.seed ^ 0x3C6EF372FE94F82Bull);
        if(ir.chance(0.25f) && !war_) {
            static const std::vector<item> short_forms[] = {
                {{K::intro, 4}, {K::verse, 8}, {K::outro, 4}},
                {{K::intro, 4}, {K::verse, 8}, {K::chorus, 8}, {K::outro, 4}},
                {{K::intro, 8}, {K::chorus, 8}, {K::breakdown, 8}, {K::drop, 8}, {K::outro, 4}},
            };
            form = short_forms[ir.next() % 3];
        }
        const bool intermission = form.back().bars == 4;
        auto total = [&] {
            int b = 0;
            for(auto const& it : form) b += it.bars;
            return b;
        };
        auto seconds = [&] { return total() * bar_s; };
        if(intermission) {
            // 30-80 s at any tempo: double sections at fast tempos, halve them at slow ones
            for(int guard = 0; guard < 6 && seconds() < 32.0; ++guard) {
                for(auto& it : form) {
                    if(it.k != K::intro && it.k != K::outro && it.bars <= 8) {
                        it.bars *= 2;
                        break;
                    }
                }
            }
            for(int guard = 0; guard < 6 && seconds() > 80.0; ++guard) {
                for(auto& it : form) {
                    if(it.k != K::outro && it.bars >= 8) {
                        it.bars /= 2;
                        break;
                    }
                }
            }
        }
        // Fit 2-3 minutes. Shorten: halve a 16-bar intro or breakdown (a
        // 32-bar one in wartime), drop a bridge, halve a 16-bar verse /
        // chorus / drop, drop a repeated verse. Extend: double an 8-bar verse
        // or chorus, then add an 8-bar chorus after the last chorus / drop.
        // The epic long form, after the 16-minute reference module: a pads-
        // and-arps intro, a build where the kick and bass come in, main
        // sections between breakdowns (the first with a riser), a long drop,
        // a percussion-heavy groove section, a main with a stop in the middle,
        // a near-silent pause before the final main, an outro onto the pads.
        // Section lengths keep the reference's proportions, scaled to the target.
        epic_ = long_song && !intermission && r_.chance(war_ ? 0.6f : 0.45f);
        if(epic_) {
            struct bp {
                K k;
                float s;
                uint8_t out, flag;
            };
            static constexpr bp blueprint[] = {
                {K::intro, 60, 255, 1},     {K::verse, 75, 255, 2},    {K::chorus, 60, 255, 0},    {K::breakdown, 30, tr_riser, 0},
                {K::drop, 150, 255, 0},     {K::breakdown, 30, 255, 0}, {K::chorus, 60, 255, 0},   {K::breakdown, 45, 255, 0},
                {K::groove, 75, 255, 0},    {K::breakdown, 30, 255, 0}, {K::drop, 60, tr_stop, 0}, {K::drop, 60, tr_pause, 0},
                {K::drop, 90, 255, 0},      {K::outro, 37, 255, 0},
            };
            float sum = 0;
            for(bp const& b : blueprint) sum += b.s;
            form.clear();
            for(bp const& b : blueprint) {
                const double bars = static_cast<double>(b.s) / static_cast<double>(sum) * target / bar_s;
                form.push_back({b.k, std::max(8, static_cast<int>(std::lround(bars / 8.0)) * 8), b.out, b.flag});
            }
            for(int guard = 0; guard < 40 && seconds() > max_s - 2.0; ++guard) { // trim the longest section
                auto it = std::max_element(form.begin(), form.end(), [](item const& a, item const& b) { return a.bars < b.bars; });
                if(it->bars <= 8) break;
                it->bars -= 8;
            }
        }
        auto fits = [&] { return seconds() <= std::min(target + 10.0, max_s - 2.0); };
        // long songs: whole cycles before the outro (breakdown -> drop in
        // club and trance forms, verse -> chorus or bridge -> chorus otherwise)
        const bool club = war_ || std::any_of(form.begin(), form.end(), [](item const& it) { return it.k == K::drop; });
        for(int cycle = 0; long_song && !epic_ && !intermission && cycle < 24; ++cycle) {
            item a{K::bridge, 8}, b{K::chorus, 16};
            if(club) a = {K::breakdown, 16}, b = {K::drop, war_ ? 32 : 16};
            else if(cycle % 2 == 0) a = {K::verse, 16};
            if((total() + a.bars + b.bars) * bar_s > target + 20.0) break;
            const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(form.size()) - 1; // before the outro
            form.insert(form.begin() + at, b);
            form.insert(form.begin() + at, a);
        }
        for(int guard = 0; guard < 32 && !intermission && !epic_ && !fits(); ++guard) {
            bool done = false;
            for(auto& it : form) {
                if((it.bars == 16 && (it.k == K::intro || (it.k == K::breakdown && !war_))) || (war_ && it.k == K::breakdown && it.bars == 32)) {
                    it.bars /= 2;
                    done = true;
                    break;
                }
            }
            for(std::size_t i = 0; !done && i < form.size(); ++i) {
                if(form[i].k == K::bridge) {
                    form.erase(form.begin() + static_cast<std::ptrdiff_t>(i));
                    done = true;
                }
            }
            for(std::size_t i = 0; !done && i < form.size(); ++i) {
                if(form[i].bars >= 16 && form[i].k != K::outro) {
                    form[i].bars /= 2;
                    done = true;
                }
            }
            for(std::size_t i = 3; !done && i + 1 < form.size(); ++i) {
                if(form[i].k == K::verse && form[i - 1].k != form[i + 1].k) {
                    form.erase(form.begin() + static_cast<std::ptrdiff_t>(i));
                    done = true;
                }
            }
            if(!done) break;
        }
        for(int guard = 0; guard < 8 && !intermission && !long_song && seconds() < target - 15; ++guard) {
            bool done = false;
            for(auto& it : form) {
                if((it.k == K::verse || it.k == K::chorus || it.k == K::drop) && it.bars == 8) {
                    it.bars = 16;
                    done = true;
                    break;
                }
            }
            for(std::size_t i = form.size(); !done && i-- > 0;) {
                if(form[i].k == K::drop) {
                    form.insert(form.begin() + static_cast<std::ptrdiff_t>(i) + 1, item{K::chorus, 8});
                    done = true;
                } else if(form[i].k == K::chorus) { // a bridge, then the last chorus again
                    form.insert(form.begin() + static_cast<std::ptrdiff_t>(i) + 1, {item{K::bridge, 8}, item{K::chorus, 8}});
                    done = true;
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
            case K::groove:
            case K::arp_solo:
            case K::choir:
            case K::outro: p.prog = prog_a_, p.bars_per_chord = bpc_a_; break;
            case K::chorus:
            case K::drop: p.prog = prog_b_, p.bars_per_chord = bpc_b_; break;
            case K::bridge: p.prog = prog_c_, p.bars_per_chord = 2; break;
            case K::breakdown: p.prog = r_.chance(0.35f) ? prog_c_ : r_.chance(0.5f) ? prog_a_ : prog_b_, p.bars_per_chord = 2; break;
            }
            p.forced_out = it.out;
            p.quiet = it.flag == 1;
            p.build_up = it.flag == 2;
            if(it.k == K::verse) p.variant = verses++ > 0 ? 1 : 0;
            if(it.k == K::chorus || it.k == K::drop) p.variant = choruses++ > 0 ? 1 : 0;
            plan_.push_back(p);
            s_.sections.push_back(p.sec);
            bar += static_cast<uint32_t>(it.bars);
        }
        s_.total_bars = bar;

        // Key change: the last chorus / drop (and what follows) a step up,
        // prepared by the new key's dominant in the bar before.
        const float mod_p = s_.m == mood::calm ? 0.0f : s_.m == mood::triumph ? 0.5f : war_ ? 0.35f : 0.3f;
        int last_big = -1, bigs = 0;
        for(std::size_t i = 0; i < plan_.size(); ++i) {
            const K k = plan_[i].sec.kind;
            if(k == K::chorus || k == K::drop) {
                ++bigs;
                if(bigs >= 2 && plan_[i - 1].sec.kind != K::chorus && plan_[i - 1].sec.kind != K::drop) last_big = static_cast<int>(i);
            }
        }
        if(epic_) last_big = static_cast<int>(plan_.size()) - 2; // the final main, after the pause
        if(last_big > 0 && r_.chance(mod_p)) {
            static constexpr int shifts[] = {2, 2, 2, 1, 3};
            mod_shift_ = r_.pick(shifts);
            mod_bar_ = plan_[static_cast<std::size_t>(last_big)].sec.start_bar;
            for(std::size_t i = static_cast<std::size_t>(last_big); i < plan_.size(); ++i) plan_[i].shift = mod_shift_;
            plan_item& before = plan_[static_cast<std::size_t>(last_big - 1)];
            // the new key's V: +2 -> VI major (secondary dominant), +1 -> bVI, +3 -> bVII
            before.pivot_deg = mod_shift_ == 3 ? 6 : 5;
            before.pivot_col = mod_shift_ == 2 ? c_med6 : c_minor;
        }
        s_.final_key = key_ + mod_shift_;
    }

    // --- arrangement: transitions and melodic development (tr_) -----------
    // Section boundaries get one of several hand-overs, picked per seed and
    // per context, so a riser is one option among many (docs/14-audio.md#transitions).
    enum tr_kind : uint8_t {
        tr_plain,        // straight cut, crash on the next downbeat
        tr_fill,         // snare fill on the last 2 beats
        tr_toms,         // tom fill on the last beat
        tr_roll,         // snare roll build-up (quarters / 8ths, then 16ths)
        tr_riser,        // noise riser (2 bars, 4 out of a breakdown)
        tr_short_riser,  // one-bar sweep up
        tr_filter_close, // bus low-pass closes over the last 2 bars
        tr_filter_open,  // the section starts dark and the low-pass opens across it
        tr_kick_out,     // the kick drops out for the last bar
        tr_stop,         // everything but the lead stops for the last beats
        tr_kick_return,  // breakdown: the kick comes back for its last bars, with a roll
        tr_drum_break,   // the melody, arp and chords drop out for the last 4 bars: drums and bass only
        // crescendo devices (builds into a chorus / drop, or out of a breakdown)
        tr_clap_roll,    // accelerating clap roll, crescendo
        tr_tom_cascade,  // descending tom cascades, 8ths then 16ths
        tr_layer_in,     // breakdown / intro: hats, kick, bass, snare enter one by one
        tr_hat_ramp,     // hats densify and open: 8ths, 16ths, open 8ths, rising
        tr_arp_climb,    // the arpeggio speeds up and climbs an octave per bar
        tr_stab_ramp,    // chord stabs: quarters, 8ths, 16ths, rising
        tr_pad_swell,    // one pad chord swells from silence over the build
        tr_kick_double,  // the kick doubles: quarters to 8ths, then a 16th kick roll
        tr_lead_climb,   // the melody ends early; the lead runs up a scale and bends into the downbeat
        tr_pause,        // near-silent drop-out: everything stops for the last bar (a tension pause)
        tr_count
    };
    // build length in bars for a crescendo device out of section `p`
    [[nodiscard]] int build_bars(plan_item const& p) const {
        const int bars = static_cast<int>(p.sec.bars);
        if(p.sec.kind == section_kind::breakdown || p.sec.kind == section_kind::intro) return std::min(war_ ? 8 : 4, bars / 2);
        return std::min(war_ ? 4 : 2, bars / 4);
    }
    static char const* tr_name(uint8_t k) {
        static char const* const names[] = {"cut", "fill", "toms", "roll", "riser", "sweep", "filter-close", "filter-open", "kick-out", "stop", "kick-return", "drum-break",
                                             "clap-roll", "tom-cascade", "layer-in", "hat-ramp", "arp-climb", "stab-ramp", "pad-swell", "kick-double", "lead-climb", "pause"};
        return k < sizeof names / sizeof names[0] ? names[k] : "?";
    }

    void choose_arrangement() {
        using K = section_kind;
        seq_step_ = tr_.chance(0.45f) ? 1 : tr_.chance(0.5f) ? 2 : -1;
        dev_invert_ = tr_.chance(0.5f);
        dev_steps_ = tr_.chance(0.5f) ? -2 : 3;
        risers_left_ = tr_.chance(0.25f) ? 1 : 0; // most songs have no long riser at all
        int sweeps_left = tr_.chance(0.5f) ? 1 : 0;
        hard_drop_ = war_ || (groove_b_ == g_four && bass_b_ != b_offbeat && tr_.chance(s_.m == mood::calm ? 0.25f : 0.45f));
        // trance gates over a bar of 16ths (bit r = row r)
        static constexpr uint16_t gates[] = {0xB6DB /* x.xx.xx.xx.x.xx.x */, 0x6DB6, 0xDDDD /* x.xxx.xx... */, 0x5555, 0xF6F6};
        gate_mask_ = tr_.chance(war_ ? 1.0f : 0.4f) ? tr_.pick(gates) : 0;
        beat_intro_ = tr_.chance(0.3f); // 90s style: the intro opens on drums and bass, the chords come in halfway
        // this song's taste: every device weight scaled by 0.4-1.6
        float taste[tr_count];
        for(float& t : taste) t = tr_.range(0.4f, 1.6f);
        if(war_) taste[tr_kick_double] *= 2.5f; // kick rolls into the drop
        plan_.back().cadence = true;
        cad_steps_ = tr_.chance(0.4f) ? 2 : tr_.chance(0.5f) ? -1 : 1;
        // Sections end on a chord that holds a tonic-triad tone, so the
        // closing phrase can land on a stable note: II (lydian), bVII, bII,
        // ii... at the very end become a modal cadence onto I for one bar.
        for(plan_item& p : plan_) {
            if(p.sec.kind == K::intro || p.cadence || p.pivot_deg >= 0) continue;
            const int idx = ((static_cast<int>(p.sec.bars) - 1) / p.bars_per_chord) % 4;
            const int last = p.prog->deg[idx];
            p.end_tonic = p.prog->col[idx] != c_home || (!is_chord_tone(0, last) && !is_chord_tone(4, last) && !is_chord_tone(2, last));
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
            const bool melodic = k == K::verse || k == K::chorus || k == K::drop || k == K::bridge || k == K::breakdown;
            const float L = melodic && p.sec.bars >= 8 ? 1.0f : 0.0f;
            if(k == K::breakdown) {
                o = {{tr_roll, 1.0f},       {tr_clap_roll, 0.8f},   {tr_riser, 1.8f * R},  {tr_filter_open, 1.0f}, {tr_kick_return, 1.0f},
                     {tr_short_riser, 0.9f * S}, {tr_stop, 0.7f},   {tr_layer_in, 1.2f},   {tr_hat_ramp, 0.9f},    {tr_arp_climb, 0.9f},
                     {tr_pad_swell, 1.0f},  {tr_lead_climb, 0.8f},  {tr_tom_cascade, 0.6f}, {tr_stab_ramp, 0.7f}, {tr_kick_double, war_ ? 1.0f : 0.0f}};
            } else if(next == K::chorus || next == K::drop) {
                o = {{tr_roll, 1.0f},        {tr_clap_roll, 0.8f}, {tr_fill, 1.0f},      {tr_toms, 0.4f},       {tr_tom_cascade, 1.0f},
                     {tr_riser, 1.6f * R},   {tr_short_riser, 0.8f * S}, {tr_kick_out, 0.7f}, {tr_stop, 0.5f}, {tr_plain, 0.3f},
                     {tr_drum_break, k != K::intro && p.sec.bars >= 16 ? 0.6f : 0.0f}, {tr_hat_ramp, 0.9f}, {tr_arp_climb, 0.8f},
                     {tr_stab_ramp, 0.8f},   {tr_pad_swell, 0.6f}, {tr_kick_double, 1.4f}, {tr_lead_climb, 0.9f * L},
                     {tr_layer_in, k == K::intro ? 1.0f : 0.0f}, {tr_filter_open, k == K::intro ? 0.8f : 0.0f}};
            } else if(next == K::breakdown) {
                o = {{tr_filter_close, 1.6f}, {tr_fill, 0.7f}, {tr_toms, 0.5f}, {tr_stop, 0.6f}, {tr_plain, 0.6f}};
            } else if(next == K::outro) {
                o = {{tr_fill, 1.0f}, {tr_toms, 0.7f}, {tr_plain, 0.8f}, {tr_filter_close, 0.6f}, {tr_kick_out, 0.6f}};
            } else if(k == K::intro) {
                o = {{tr_filter_open, 1.0f}, {tr_fill, 0.8f}, {tr_toms, 0.5f}, {tr_plain, 0.6f}, {tr_kick_out, 0.4f},
                     {tr_layer_in, 0.9f},    {tr_hat_ramp, 0.7f}, {tr_arp_climb, 0.7f}, {tr_pad_swell, 0.5f}};
            } else {
                o = {{tr_fill, 1.2f}, {tr_toms, 0.8f}, {tr_plain, 0.9f}, {tr_kick_out, 0.6f}, {tr_stop, 0.4f},
                     {tr_drum_break, p.sec.bars >= 16 ? 0.6f : 0.0f}, {tr_hat_ramp, 0.5f}, {tr_tom_cascade, 0.4f}};
            }
            for(opt& x : o) {
                x.w *= taste[x.k];
                if(x.k == prev) x.w = 0.0f; // never the same hand-over twice in a row
                if(x.k == tr_lead_climb && plan_[i + 1].shift != p.shift) x.w = 0.0f; // the key change has its own pivot
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
                for(auto const& c : o) {
                    if(c.w > 0) return c.k;
                }
                return static_cast<uint8_t>(tr_plain);
            };
            const uint8_t t = p.forced_out != 255 ? p.forced_out : pick();
            if(t == tr_riser) --risers_left_;
            if(t == tr_short_riser) --sweeps_left;
            p.out = t;
            prev = t;
            const bool up = next == K::chorus || next == K::drop;
            p.reverse = t != tr_riser && t != tr_pad_swell && tr_.chance(t == tr_stop ? 0.5f : up ? 0.15f : 0.08f);
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

    // A roll from `bar0` over `bars` bars, ending `gap_rows` before the
    // boundary, crescendo from soft to accented. It accelerates in musical
    // steps and tops out at 16ths: quarters for the first half of a long
    // build, 8ths, then 16ths for the last quarter (at most two bars); only
    // a build of 2+ bars may end on one beat of 32nds.
    void snare_roll(uint32_t bar0, int bars, int gap_rows, uint8_t drum = dr_snare) {
        const int total = bars * 16 - gap_rows;
        const int sixteenths = std::min(32, std::max(4, total / 4));
        const int quarters = bars >= 4 ? total / 2 : 0;
        const bool thirty_seconds = bars >= 2 && tr_.chance(0.3f);
        for(int r = 0; r < total; ++r) {
            const int left = total - r;
            const int step = r < quarters ? 4 : left > sixteenths ? 2 : 1;
            if(r % step) continue;
            const int vel = 40 + 80 * r / std::max(1, total);
            const uint32_t tick = (bar0 + static_cast<uint32_t>(r / 16)) * ticks_per_bar + static_cast<uint32_t>((r % 16) * ticks_per_row);
            add_ticks(tick, ch_snare, drum, 2, vel);
            if(thirty_seconds && left <= 4) add_ticks(tick + 3, ch_snare, drum, 2, vel - 6);
        }
    }

    // The crescendo devices that need their own events (the rolls, risers
    // and filter sweeps are emitted in render_section's bar loop).
    void render_build(std::size_t index) {
        plan_item const& p = plan_[index];
        if(index + 1 >= plan_.size()) return;
        const uint8_t T = p.out;
        const int bars = static_cast<int>(p.sec.bars);
        const int nb = std::max(1, build_bars(p));
        const int from = bars - nb;
        const uint32_t b0 = p.sec.start_bar;
        const uint32_t t0 = (b0 + static_cast<uint32_t>(from)) * ticks_per_bar, t1 = (b0 + static_cast<uint32_t>(bars)) * ticks_per_bar;
        auto mute = [&](uint32_t chans) { mutes_.push_back({t0, t1, chans}); };
        const std::size_t mark = s_.events.size(); // everything from here goes to post_
        switch(T) {
        case tr_tom_cascade:
            // 8ths high-mid in the first bars, then 16th cascades high -> low each beat
            for(int bi = from; bi < bars; ++bi) {
                const bool last = bi == bars - 1;
                for(int row = 0; row < 16; row += last ? 1 : 2) {
                    static constexpr uint8_t casc[4] = {dr_tom_high, dr_tom_high, dr_tom_mid, dr_tom_low};
                    const uint8_t tom = last ? casc[row % 4] : static_cast<uint8_t>(row % 4 == 0 ? dr_tom_high : dr_tom_mid);
                    add(b0 + static_cast<uint32_t>(bi), row, ch_fx, {tom}, 2, 60 + 50 * ((bi - from) * 16 + row) / (nb * 16));
                }
            }
            break;
        case tr_layer_in: {
            // instruments enter one by one: hats, then kick, then off-beat bass, then a snare roll
            mute((1u << ch_kick) | (1u << ch_hats) | (1u << ch_bass) | (1u << ch_snare));
            for(int bi = from; bi < bars; ++bi) {
                const int stage = (bi - from) * 4 / nb;
                const uint32_t bar = b0 + static_cast<uint32_t>(bi);
                const int root = chord_at(p, bi);
                for(int row = 0; row < 16; row += 2) add(bar, row, ch_hats, {row % 4 == 2 ? dr_hat_open : dr_hat_closed}, 1, 60 + 6 * stage);
                if(stage >= 1) {
                    for(int row = 0; row < 16; row += 4) add(bar, row, ch_kick, {dr_kick}, 2, 104 + 4 * stage);
                }
                if(stage >= 2) {
                    for(int row = 2; row < 16; row += 4) add(bar, row, ch_bass, {pitch(key_ - 12, root)}, 2, 100);
                }
            }
            snare_roll(b0 + static_cast<uint32_t>(bars - std::max(1, nb / 4)), std::max(1, nb / 4), 0);
            break;
        }
        case tr_hat_ramp:
            mute(1u << ch_hats);
            for(int bi = from; bi < bars; ++bi) {
                const int stage = (bi - from) * 3 / nb; // 8ths, 16ths, 16ths + open 8ths
                for(int row = 0; row < 16; row += stage == 0 ? 2 : 1) {
                    const int vel = 50 + 60 * ((bi - from) * 16 + row) / (nb * 16);
                    add(b0 + static_cast<uint32_t>(bi), row, ch_hats, {stage == 2 && row % 2 == 0 ? dr_hat_open : dr_hat_closed}, 1, vel);
                }
            }
            break;
        case tr_arp_climb:
            mute(1u << ch_arp);
            for(int bi = from; bi < bars; ++bi) {
                const int k = bi - from;
                const int rate = k * 2 < nb ? 2 : 1; // 8ths, then 16ths
                const int root = chord_at(p, bi);
                const int shift = 12 * (k * 2 / std::max(1, nb)) - 12; // an octave up halfway
                int idx = 0;
                for(int row = 0; row < 16; row += rate) {
                    const int tone = arp_order_[static_cast<std::size_t>(idx++) % arp_order_.size()];
                    const int deg = root + (tone == 3 ? 7 : tone == 4 ? 9 : tone * 2) + (row / 4) % 2;
                    const int vel = 56 + 50 * (k * 16 + row) / (nb * 16);
                    add(b0 + static_cast<uint32_t>(bi), row, ch_arp, {pitch(arp_base_ + shift, deg)}, 1, vel);
                }
            }
            break;
        case tr_stab_ramp:
            mute(1u << ch_pad);
            for(int bi = from; bi < bars; ++bi) {
                const int stage = (bi - from) * 3 / nb; // quarters, 8ths, 16ths
                const int step = stage == 0 ? 4 : stage == 1 ? 2 : 1;
                for(int row = 0; row < 16; row += step) {
                    emit_chord(b0 + static_cast<uint32_t>(bi), row, chord_at(p, bi), 1, 64 + 50 * ((bi - from) * 16 + row) / (nb * 16), layer_always, 0, true);
                }
            }
            break;
        case tr_pad_swell: {
            // A crescendo that follows the harmony: the first chord swells
            // from silence over its own length, each later chord of the build
            // enters a step louder (it used to hold the first chord across
            // the build's chord changes). The extra chords draw no random
            // numbers and leave the voice leading as it was, so the rest of
            // the song is unchanged.
            mute(1u << ch_pad);
            int saved[4] = {0, 0, 0, 0};
            int saved_n = 0;
            for(int bi = from; bi < bars;) {
                const int cid = chord_id(p, bi);
                int end = bi + 1;
                while(end < bars && chord_id(p, end) == cid) ++end;
                const int vel = 60 + 50 * (end - from) / nb;
                if(bi == from) {
                    emit_chord(b0 + static_cast<uint32_t>(bi), 0, chord_at(p, bi), end >= bars ? nb * 16 : (end - bi) * 16, end >= bars ? 110 : vel, layer_always);
                    s_.events.back().fx = note_fx::swell;
                    std::copy(last_voicing_, last_voicing_ + 4, saved);
                    saved_n = voicing_n_;
                    no_jitter_ = true;
                } else {
                    emit_chord(b0 + static_cast<uint32_t>(bi), 0, chord_at(p, bi), (end - bi) * 16, vel, layer_always);
                }
                bi = end;
            }
            no_jitter_ = false;
            std::copy(saved, saved + 4, last_voicing_);
            voicing_n_ = saved_n;
            break;
        }
        case tr_kick_double: {
            // quarters to 8ths, then the kick roll: the last beat in 16ths
            // (wartime: the last two beats, the hardtrance kick roll)
            const int kb = std::min(nb, 2);
            const int roll_from = war_ ? 8 : 12;
            mutes_.push_back({t1 - static_cast<uint32_t>(kb) * ticks_per_bar, t1, 1u << ch_kick});
            for(int bi = bars - kb; bi < bars; ++bi) {
                const bool last = bi == bars - 1;
                for(int row = 0; row < 16; row += (last && row >= roll_from) ? 1 : 2) {
                    add(b0 + static_cast<uint32_t>(bi), row, ch_kick, {dr_kick}, 1, row % 4 == 0 ? 120 : 96 + row);
                }
            }
            break;
        }
        case tr_lead_climb: {
            // the melody stopped nb bars early (render_melody): a scale run up
            // in 8ths, then the dominant held and bent up a fourth onto the tonic
            const int run_bars = std::max(1, nb - 1);
            const int start = last_lead_ >= 0 ? last_lead_ : pitch(lead_base_, 0);
            int deg = 0;
            int base = pitch(lead_base_, 0);
            while(base > start + 2) base -= 12;
            while(base < start - 9) base += 12;
            const int steps = run_bars * 8;
            for(int i = 0; i < steps; ++i) {
                const int bi = from + i / 8;
                deg = i * 7 / steps; // one octave over the run
                const int note = pitch(base, deg);
                add(b0 + static_cast<uint32_t>(bi), (i % 8) * 2, ch_lead, {std::min(note, 96)}, 2, 70 + 30 * i / steps);
            }
            const int dom = pitch(base, 4); // bends a fourth up, onto the tonic an octave above the run's start
            add(b0 + static_cast<uint32_t>(bars - 1), 0, ch_lead, {dom, dom + 5}, 16, 100, layer_always, note_fx::slide_up);
            break;
        }
        default: break;
        }
        post_.insert(post_.end(), s_.events.begin() + static_cast<std::ptrdiff_t>(mark), s_.events.end());
        s_.events.resize(mark);
    }

    // --- the arp solo -------------------------------------------------------
    // After the reference track (analysed for docs/14): an
    // 8-bar stretch (16 in wartime and long songs) after the first chorus or
    // drop where the arpeggio carries the melody. A one-bar breath (no kick,
    // no bass) introduces it; kick, off-beat bass, hats and a backbeat come
    // back under it; the pads hold the chords quietly; no lead, no counter.
    // The arp starts in 8ths, runs wide 16th cells from bar 3 with a
    // crescendo (the filter opens with velocity), and its last bar jumps an
    // octave on every beat. It is planned after the arrangement and drawn
    // from its own generator, and the sections around it render exactly as
    // before, so a seed keeps the rest of its song.
    void insert_arp_solo() {
        using K = section_kind;
        static constexpr float p_solo[mood_count] = {0.2f, 0.4f, 0.35f, 0.45f, 0.45f};
        arp_wide_ = sr_.chance(0.45f);
        const bool want = sr_.chance(p_solo[static_cast<int>(s_.m)]);
        const float pick = sr_.uniform();
        const bool two = sr_.chance(0.5f);
        if(!want || s_.total_bars < 40) return; // not in intermissions
        std::size_t j = 0;
        for(std::size_t i = 1; i + 1 < plan_.size(); ++i) {
            if(big(plan_[i].sec.kind)) {
                j = i + 1;
                break;
            }
        }
        if(j == 0 || plan_[j].sec.kind == K::outro) return;
        plan_item& prev = plan_[j - 1];
        if(prev.pivot_deg >= 0 || plan_[j].shift != prev.shift) return; // not into the key change
        const int bars = war_ || s_.duration_seconds() > 300.0 ? 16 : 8;
        if((s_.total_bars + static_cast<uint32_t>(bars)) * 240.0 / static_cast<double>(s_.bpm) > 598.0) return;
        // progressions in the reference's spirit: held chords, the relative
        // major, dorian ii, a major V, VII held before the return
        static const progression solo_progs[] = {
            {minorish, {0, 0, 5, 6}},                                                   // i i VI VII
            {bit(m_aeolian) | bit(m_dorian), {0, 1, 5, 4}, {c_home, c_dorian, c_home, c_harm}}, // i ii VI V
            {bit(m_aeolian) | bit(m_dorian) | bit(m_harmonic), {2, 6, 5, 5}},           // III VII VI VI (relative major)
            {bit(m_aeolian) | bit(m_dorian), {0, 1, 6, 6}, {c_home, c_dorian, c_home, c_home}}, // i ii VII VII
            {bit(m_aeolian) | bit(m_phrygian), {0, 5, 2, 6}},                           // i VI III VII
            {majorish, {0, 0, 3, 4}},                                                   // I I IV V
            {bit(m_ionian) | bit(m_mixolydian), {5, 3, 0, 4}},                          // vi IV I V
            {bit(m_ionian) | bit(m_mixolydian) | bit(m_lydian), {0, 5, 6, 0}, {c_home, c_minor, c_minor, c_home}}, // I bVI bVII I
        };
        std::vector<progression const*> fit;
        for(auto const& pr : solo_progs) {
            if(pr.mode_mask & bit(s_.mode)) fit.push_back(&pr);
        }
        plan_item solo;
        solo.sec = {K::arp_solo, prev.sec.start_bar + prev.sec.bars, static_cast<uint32_t>(bars)};
        solo.prog = fit.empty() ? prog_b_ : fit[std::min(fit.size() - 1, static_cast<std::size_t>(pick * static_cast<float>(fit.size())))];
        solo.bars_per_chord = bars >= 16 || two ? 2 : 1;
        solo.shift = prev.shift;
        solo.downlift = prev.downlift; // the section after the solo keeps its entry
        solo.impact = prev.impact;
        solo.out = tr_plain;
        const int idx = ((bars - 1) / solo.bars_per_chord) % 4;
        const int last = solo.prog->deg[idx];
        solo.end_tonic = solo.prog->col[idx] != c_home || (!is_chord_tone(0, last) && !is_chord_tone(4, last) && !is_chord_tone(2, last));
        prev.orig_next = static_cast<uint8_t>(plan_[j].sec.kind);
        const std::string after = section_name(prev.sec.kind);
        plan_.insert(plan_.begin() + static_cast<std::ptrdiff_t>(j), solo);
        solo_bar_ = solo.sec.start_bar;
        for(std::size_t i = j + 1; i < plan_.size(); ++i) plan_[i].sec.start_bar += static_cast<uint32_t>(bars);
        if(mod_shift_ && mod_bar_ >= plan_[j].sec.start_bar) mod_bar_ += static_cast<uint32_t>(bars);
        s_.sections.clear();
        for(plan_item const& it : plan_) s_.sections.push_back(it.sec);
        s_.total_bars += static_cast<uint32_t>(bars);
        transitions_ += (transitions_.empty() ? "" : "; ") + std::string("arp solo after ") + after;
    }

    void render_arp_solo(std::size_t index) {
        plan_item const& p = plan_[index];
        rng* const keep_jit = jit_;
        jit_ = &sr_; // no draw from the main generators
        int keep_voicing[4];
        std::copy(last_voicing_, last_voicing_ + 4, keep_voicing);
        const int keep_n = voicing_n_;
        swing_ = song_swing_;
        const bool calm = s_.m == mood::calm;
        const int bars = static_cast<int>(p.sec.bars);
        const uint32_t b0 = p.sec.start_bar;
        add(b0, 0, ch_fx, {dr_crash}, 16, 84);
        for(int bi = 0; bi < bars; ++bi) {
            const uint32_t bar = b0 + static_cast<uint32_t>(bi);
            const int root = chord_at(p, bi);
            const bool breath = bi == 0;
            const bool last_bar = bi == bars - 1;
            // drums: the breath, then four-on-the-floor, hats, a backbeat from bar 3
            if(!breath && !(calm && bi < 2)) {
                for(int row = 0; row < 16; row += 4) add(bar, row, ch_kick, {dr_kick}, 2, 116);
            }
            for(int row = 0; row < 16; row += 2) {
                if(row % 4 == 2 && !breath) add(bar, row, ch_hats, {dr_hat_open}, 2, 78);
                else add(bar, row, ch_hats, {dr_hat_closed}, 1, row % 4 == 0 ? 58 : 68);
            }
            if(bi >= 2 && !(last_bar && bars > 1)) {
                for(int row : {4, 12}) add(bar, row, ch_snare, {backbeat_}, 2, calm ? 84 : 100);
            }
            if(last_bar) {
                add(bar, 4, ch_snare, {backbeat_}, 2, 100);
                for(int row = 12; row < 16; ++row) add(bar, row, ch_snare, {dr_snare}, 1, 70 + 10 * (row - 12)); // a 16th pickup
            }
            // bass: off-beat roots (calm: the root held)
            if(!breath) {
                if(calm) add(bar, 0, ch_bass, {pitch(key_ - 12, root)}, 14, 96);
                else {
                    for(int row = 2; row < 16; row += 4) add(bar, row, ch_bass, {pitch(key_ - 12, root)}, 2, 104);
                }
            }
            // pads: the chords held, quietly
            const bool new_chord = bi == 0 || chord_id(p, bi - 1) != chord_id(p, bi);
            if(new_chord) {
                int span = 1;
                while(bi + span < bars && chord_id(p, bi + span) == chord_id(p, bi)) ++span;
                emit_chord(bar, 0, root, span * 16 - 1, 54, layer_always);
            }
        }
        // the arp as the lead: 8ths for two bars (calm: throughout), then wide
        // 16th cells, crescendo, the last bar jumping an octave every beat
        const int base = key_ + 12;
        int step = 0;
        for(int pos = 0; pos < bars * 16;) {
            const int b = pos / 16, row = pos % 16;
            const int rate = calm || b < 2 ? 2 : 1;
            int deg = wide_deg(p, b, step, b);
            if(b == bars - 1 && step % 4 == 0) deg += 7;
            int n = pitch(base, deg);
            while(n > 90) n -= 12;
            const int vel = 74 + 40 * pos / (bars * 16) + (row % 4 == 0 ? 8 : 0);
            add(b0 + static_cast<uint32_t>(b), row, ch_arp, {n}, rate == 2 ? 2 : 1, std::min(127, vel));
            ++step;
            pos += rate;
        }
        add(b0 + static_cast<uint32_t>(bars), 0, ch_fx, {dr_crash}, 16, 100);
        std::copy(keep_voicing, keep_voicing + 4, last_voicing_);
        voicing_n_ = keep_n;
        jit_ = keep_jit;
    }

    // --- the choir block -----------------------------------------------------
    // After the fourth reference (data/eclipse.mp3, 160 BPM): late in the
    // track the kick turns to the off-beats ("mini drops"), then drops out
    // but for pickups every two bars while the synth carries on alone; a
    // choir comes in over the bare low end; a roll and a riser; then the
    // choir drop with the kick back. Inserted before the last chorus /
    // drop, from the arp devices' generator, like the arp solo, so the
    // rest of the song is untouched. Not in calm songs.
    void insert_choir() {
        using K = section_kind;
        static constexpr float p_choir[mood_count] = {0.0f, 0.25f, 0.3f, 0.3f, 0.4f};
        const bool want = sr_.chance(p_choir[static_cast<int>(s_.m)]);
        if(!want || s_.total_bars < 40) return;
        std::size_t j = 0;
        for(std::size_t i = plan_.size(); i-- > 2;) {
            if(big(plan_[i].sec.kind)) {
                j = i;
                break;
            }
        }
        if(j < 2) return;
        plan_item& prev = plan_[j - 1];
        if(prev.pivot_deg >= 0 || prev.shift != plan_[j].shift || prev.sec.kind == K::arp_solo || prev.orig_next != 255) return;
        const int bars = war_ || s_.duration_seconds() > 300.0 ? 24 : 16;
        if((s_.total_bars + static_cast<uint32_t>(bars)) * 240.0 / static_cast<double>(s_.bpm) > 598.0) return;
        plan_item blk;
        blk.sec = {K::choir, prev.sec.start_bar + prev.sec.bars, static_cast<uint32_t>(bars)};
        blk.prog = plan_[j].prog; // the harmony of the drop it leads into
        blk.bars_per_chord = 2;
        blk.shift = prev.shift;
        blk.downlift = prev.downlift;
        blk.impact = prev.impact;
        blk.out = tr_plain;
        const int idx = ((bars - 1) / 2) % 4;
        const int last = blk.prog->deg[idx];
        blk.end_tonic = blk.prog->col[idx] != c_home || (!is_chord_tone(0, last) && !is_chord_tone(4, last) && !is_chord_tone(2, last));
        prev.orig_next = static_cast<uint8_t>(plan_[j].sec.kind);
        const std::string before = section_name(plan_[j].sec.kind);
        plan_.insert(plan_.begin() + static_cast<std::ptrdiff_t>(j), blk);
        for(std::size_t i = j + 1; i < plan_.size(); ++i) plan_[i].sec.start_bar += static_cast<uint32_t>(bars);
        if(mod_shift_ && mod_bar_ >= plan_[j].sec.start_bar) mod_bar_ += static_cast<uint32_t>(bars);
        if(solo_bar_ != ~0u && solo_bar_ >= plan_[j].sec.start_bar) solo_bar_ += static_cast<uint32_t>(bars);
        s_.sections.clear();
        for(plan_item const& it : plan_) s_.sections.push_back(it.sec);
        s_.total_bars += static_cast<uint32_t>(bars);
        transitions_ += (transitions_.empty() ? "" : "; ") + std::string("choir block before ") + before;
    }

    // A choir chord in its own register (the third and fifth on top), not
    // touching the pads' voice leading.
    void choir_chord(uint32_t bar, int row, int root, int len, int vel, note_fx fx = note_fx::none) {
        int a = pitch(key_, root);
        while(a < 55) a += 12;
        while(a > 66) a -= 12;
        int b = pitch(key_, root + 2);
        while(b <= a) b += 12;
        int c = pitch(key_, root + 4);
        while(c <= b) c += 12;
        add(bar, row, ch_choir, {a, b, c, a + 12}, len, vel, layer_always, fx);
    }

    void render_choir(std::size_t index) {
        plan_item const& p = plan_[index];
        rng* const keep_jit = jit_;
        jit_ = &sr_;
        swing_ = 0;
        const int bars = static_cast<int>(p.sec.bars);
        const uint32_t b0 = p.sec.start_bar;
        // phases: mini drops, the synth alone, the choir enters, the choir drop
        const int q = bars / 4;
        const int p1 = q, p2 = 2 * q, p3 = bars - (bars >= 24 ? 8 : 4); // 16 bars: 4/4/4/4; 24 bars: 6/6/4/8
        int keep_voicing[4];
        std::copy(last_voicing_, last_voicing_ + 4, keep_voicing);
        const int keep_n = voicing_n_;
        for(int bi = 0; bi < bars; ++bi) {
            const uint32_t bar = b0 + static_cast<uint32_t>(bi);
            const int root = chord_at(p, bi);
            const bool drop = bi >= p3;
            const bool new_chord = bi == 0 || chord_id(p, bi - 1) != chord_id(p, bi);
            // kick: off-beat 8ths (mini drops) -> out, a pickup every 2 bars -> four-on-the-floor in the drop
            if(bi < p1) {
                for(int row = 2; row < 16; row += 4) add(bar, row, ch_kick, {dr_kick}, 2, 112);
            } else if(!drop) {
                if(bi % 2 == 1 && bi != p3 - 1) for(int row = 12; row < 16; ++row) add(bar, row, ch_kick, {dr_kick}, 1, 84 + 8 * (row - 12));
            } else {
                for(int row = 0; row < 16; row += 4) add(bar, row, ch_kick, {dr_kick}, 2, 120);
            }
            // hats: thin while the kick is away, 16ths in the drop
            for(int row = 0; row < 16; row += drop ? 1 : 2) add(bar, row, ch_hats, {row % 4 == 2 ? dr_hat_open : dr_hat_closed}, 1, drop ? (row % 4 == 2 ? 86 : 60) : 54);
            if(drop || bi < p1) {
                for(int row : {4, 12}) add(bar, row, ch_snare, {backbeat_}, 2, drop ? 112 : 96);
            }
            // bass: only with the kick (the choir enters over a bare low end)
            if(drop || bi < p1) {
                for(int row = 2; row < 16; row += 4) add(bar, row, ch_bass, {pitch(key_ - 12, root)}, 2, 106);
            }
            // the repeating synth: the arp's wide cells throughout
            for(int row = 0; row < 16; ++row) {
                int deg = wide_deg(p, bi, bi * 16 + row, bi);
                int n = pitch(key_ + 12, deg);
                while(n > 88) n -= 12;
                add(bar, row, ch_arp, {n}, 1, (bi >= p1 && bi < p2 ? 96 : 76) + (row % 4 == 0 ? 8 : 0));
            }
            // the choir: swells in from the third phase, held chords, full in the drop
            if(bi >= p2 && (new_chord || bi == p3)) {
                int span = 1;
                while(bi + span < bars && chord_id(p, bi + span) == chord_id(p, bi) && !(bi < p3 && bi + span == p3)) ++span;
                choir_chord(bar, 0, root, span * 16 - 1, drop ? 118 : 92, bi == p2 ? note_fx::swell : note_fx::none);
            }
            if(drop && new_chord) emit_chord(bar, 0, root, 16 * std::max(1, std::min(2, bars - bi)) - 1, 60, layer_always);
            // the build into the drop: a 16th snare roll and a riser
            if(bi == p3 - 1) {
                for(int row = 0; row < 16; ++row) add(bar, row, ch_snare, {dr_snare}, 1, 60 + 3 * row);
                add(bar, 0, ch_fx, {dr_riser}, 16, 96);
            }
            if(bi == p3) add(bar, 0, ch_fx, {dr_crash}, 16, 110), add(bar, 0, ch_fx, {dr_impact}, 16, 104);
        }
        std::copy(keep_voicing, keep_voicing + 4, last_voicing_);
        voicing_n_ = keep_n;
        jit_ = keep_jit;
    }

    // --- section rendering --------------------------------------------------
    [[nodiscard]] static bool big(section_kind k) { return k == section_kind::chorus || k == section_kind::drop; }

    void render_section(std::size_t index) {
        using K = section_kind;
        plan_item const& p = plan_[index];
        const K k = p.sec.kind;
        const K next = p.orig_next != 255 ? static_cast<K>(p.orig_next) : index + 1 < plan_.size() ? plan_[index + 1].sec.kind : K::outro;
        const bool last_section = index + 1 == plan_.size();
        const int bars = static_cast<int>(p.sec.bars);
        const uint32_t b0 = p.sec.start_bar;
        const bool into_big = big(next);
        const uint8_t T = last_section ? static_cast<uint8_t>(tr_plain) : p.out;
        plan_item const* prev = index > 0 ? &plan_[index - 1] : nullptr;
        const uint32_t end_tick = (b0 + static_cast<uint32_t>(bars)) * ticks_per_bar;
        constexpr uint32_t all_but_lead_fx = (1u << ch_kick) | (1u << ch_snare) | (1u << ch_hats) | (1u << ch_bass) | (1u << ch_arp) | (1u << ch_pad) | (1u << ch_counter);
        const bool group_b = big(k);
        groove_def const& G = grooves[group_b ? groove_b_ : groove_a_];
        bass_pat const& BP = group_b ? bpat_b_ : bpat_a_;
        swing_ = G.swing >= 0 ? G.swing : song_swing_;
        const bool calm = s_.m == mood::calm;

        // the hand-over at the end of this section
        int gap_rows = 0; // silence (but the lead and fx tails) before the next downbeat
        int roll_bars = 0;
        if(!last_section) {
            switch(T) {
            case tr_roll:
            case tr_clap_roll: roll_bars = std::max(1, build_bars(p)); break;
            case tr_riser: roll_bars = k == K::breakdown ? 2 : 0; break;
            case tr_filter_open:
            case tr_kick_return: roll_bars = k == K::breakdown ? std::min(2, bars / 2) : 0; break;
            default: break;
            }
            if(k == K::breakdown && (T == tr_roll || T == tr_clap_roll || T == tr_riser)) gap_rows = 4;
            if(T == tr_stop) gap_rows = bars >= 16 || k == K::breakdown ? 16 : 8;
            if(gap_rows > 0) mutes_.push_back({end_tick - static_cast<uint32_t>(gap_rows * ticks_per_row), end_tick, all_but_lead_fx});
            if(T == tr_drum_break) mutes_.push_back({end_tick - 4 * ticks_per_bar, end_tick, (1u << ch_lead) | (1u << ch_arp) | (1u << ch_pad) | (1u << ch_counter)});
            if(T == tr_kick_out) mutes_.push_back({end_tick - ticks_per_bar, end_tick, (1u << ch_kick) | (bars >= 16 ? (1u << ch_bass) : 0u)});
            if(T == tr_pause) mutes_.push_back({end_tick - ticks_per_bar, end_tick, all_but_lead_fx | (1u << ch_lead)});
        }
        if(p.build_up) {
            // The build is drums, bass and arps layering in; the melody waits
            // for the first main section. (A low lead entering over the bare
            // gallop bass muddied it. Muting keeps every random draw, so the
            // rest of the song is unchanged.)
            mutes_.push_back({b0 * ticks_per_bar, end_tick, 1u << ch_lead});
        }
        const int kick_return_from = T == tr_kick_return ? bars - std::min(4, bars / 2) : bars;

        for(int bi = 0; bi < bars; ++bi) {
            const uint32_t bar = b0 + static_cast<uint32_t>(bi);
            const bool last_bar = bi == bars - 1;
            const bool final_bar = last_section && last_bar;
            const bool phrase_end = (bi % 8) == 7;
            const int root = chord_at(p, bi);
            const int cid = chord_id(p, bi);
            const int next_root = last_bar ? (last_section ? 0 : chord_at(plan_[index + 1], 0)) : chord_at(p, bi + 1);
            const int next_cid = last_bar ? (last_section ? 0 : chord_id(plan_[index + 1], 0)) : chord_id(p, bi + 1);
            const bool chord_change = next_cid != cid;
            const bool fill = last_bar && !last_section && (T == tr_fill || T == tr_toms) && k != K::breakdown;
            const bool rolling = k != K::breakdown && roll_bars > 0 && bi >= bars - roll_bars;

            // build-up: kick and bass, then hats, then the backbeat, then everything
            const int stage = p.build_up ? bi * 4 / std::max(1, bars) : 3;
            const bool drums = !p.quiet;
            // kick (the groove's pattern; its variant on a phrase's last bar)
            bool kick = drums && (k != K::breakdown || bi >= kick_return_from);
            if(k == K::intro && calm && bi < bars / 2 && !beat_intro_) kick = false;
            if(k == K::outro && bi >= bars - 1) kick = false;
            if(kick) {
                uint16_t km = k == K::breakdown ? 0x1111 : phrase_end && !last_bar ? G.kick_alt : G.kick;
                if(k != K::breakdown && !war_ && (bi % 2) == 1) km = static_cast<uint16_t>(km | kick_extra_);
                for(int row = 0; row < 16; ++row) {
                    if(!(km & (1u << row))) continue;
                    if(fill && row >= 12 && into_big) continue; // leave room for the fill
                    const int vel = k == K::breakdown ? 96 + 6 * (bi - kick_return_from) : row % 4 == 0 ? 120 : 108;
                    add(bar, row, ch_kick, {dr_kick}, 2, vel);
                }
            }
            if(final_bar) add(bar, 0, ch_kick, {dr_kick}, 2, 120);

            // backbeat, ghosts, fills
            const bool backbeat = drums && stage >= 2 && (k != K::intro || bi >= bars / 2) && !rolling;
            if(backbeat && k != K::breakdown && !final_bar) {
                for(int row = 0; row < 16; ++row) {
                    if(!(G.snare & (1u << row))) continue;
                    if(fill && row >= 12) continue;
                    add(bar, row, ch_snare, {backbeat_}, 2, 112);
                    if(big(k)) add(bar, row, ch_snare, {backbeat_ == dr_clap ? dr_snare : dr_clap}, 2, 80, layer_min_high); // layered at high intensity
                }
                const uint16_t gm = static_cast<uint16_t>((G.ghost | ghost_extra_) & ~G.snare);
                for(int row = 0; row < 16; ++row) {
                    if((gm & (1u << row)) && !(fill && row >= 8)) add(bar, row, ch_snare, {dr_snare}, 1, 42, layer_min_mid);
                }
                // a small fill at the end of 4-bar phrases (active moods)
                if(!calm && !war_ && (bi % 4) == 3 && !last_bar && !phrase_end && r_.chance(0.35f)) {
                    for(int row : {13, 14, 15}) add(bar, row, ch_snare, {dr_snare}, 1, 58 + 8 * (row - 13), layer_min_mid);
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
            if(roll_bars > 0 && bi == bars - roll_bars) snare_roll(bar, roll_bars, gap_rows, T == tr_clap_roll ? dr_clap : dr_snare);

            // hats
            const bool hats = drums && stage >= 1 && k != K::breakdown && !(k == K::outro && bi >= bars - 2) && !final_bar;
            if(hats) {
                const bool open = big(k) || k == K::bridge || (k == K::verse && p.variant == 1);
                for(int row = 0; row < 16; ++row) {
                    const uint16_t bitm = static_cast<uint16_t>(1u << row);
                    if(fill && row >= 12) continue;
                    if(G.hat_open & bitm) {
                        if(open || k == K::verse) add(bar, row, ch_hats, {dr_hat_open}, 2, 92, open ? layer_always : layer_min_mid);
                        if(!open) add(bar, row, ch_hats, {dr_hat_closed}, 1, 85, k == K::verse ? layer_max_mid : layer_always);
                    } else if(G.hat_closed & bitm) {
                        const int vel = G.hat_closed == 0xFFFF || G.hat_closed == 0xBBBB ? (row % 4 == 0 ? 84 : row % 2 == 0 ? 70 : 56) : (row % 4 == 0 ? 70 : 82);
                        add(bar, row, ch_hats, {dr_hat_closed}, 1, vel);
                    } else if(G.hat_extra & bitm) {
                        add(bar, row, ch_hats, {dr_hat_closed}, 1, 52, k == K::intro ? layer_min_high : layer_min_mid);
                    }
                }
                if(big(k)) {
                    for(int row = 0; row < 16; ++row) add(bar, row, ch_fx, {row % 4 == 2 ? dr_rim : dr_shaker}, 1, row % 4 == 2 ? 55 : 40, layer_min_high);
                }
                if(style_ == st_punk && big(k) && bi % 2 == 0) add(bar, 0, ch_fx, {dr_crash}, 16, 84); // crash-ride punk
                if(k == K::groove) {
                    // percussion: shaker 16ths, rim on the off-8ths, a tom figure every other bar
                    for(int row = 0; row < 16; ++row) add(bar, row, ch_fx, {row % 4 == 2 ? dr_rim : dr_shaker}, 1, row % 4 == 2 ? 70 : row % 2 ? 44 : 56);
                    static constexpr int tom_rows[2][4] = {{3, 6, 10, 13}, {6, 10, 12, 14}};
                    static constexpr uint8_t toms[4] = {dr_tom_high, dr_tom_mid, dr_tom_mid, dr_tom_low};
                    for(int i = 0; i < 4; ++i) add(bar, tom_rows[bi % 2][i], ch_fx, {toms[i]}, 2, 72 + 6 * i);
                }
            }
            if(k == K::breakdown && (bi < bars - 2 || bi >= kick_return_from) && !calm) {
                for(int row = 2; row < 16; row += 4) add(bar, row, ch_hats, {dr_hat_closed}, 1, 45, layer_min_mid);
            }

            // bass (the hardtrance drop: off-beat roots, octave on top at high intensity)
            const bool offbeat_drop = k == K::drop && hard_drop_ && !(war_ && bass_b_ != b_offbeat && p.variant == 0);
            bass_pat ob;
            ob.mask = 0x4444;
            bass_pat const& B = offbeat_drop ? ob : BP;
            bool bass = drums && (k != K::intro || (bars >= 16 && bi >= bars - 4) || beat_intro_);
            if(k == K::breakdown || (k == K::outro && bi >= bars - 3)) bass = false;
            // a walking run into the next chord at the end of an 8-bar phrase (active moods)
            const bool bass_run = bass && !calm && !war_ && phrase_end && !last_bar && !offbeat_drop && style_ != st_eurodance;
            if(bass) {
                for(int row = 0; row < 16; ++row) {
                    if(!(B.mask & (1u << row))) continue;
                    if(fill && row >= 12 && into_big) continue;
                    if(bass_run && row >= 12) continue;
                    int next_hit = row + 1;
                    while(next_hit < 16 && !(B.mask & (1u << next_hit))) ++next_hit;
                    const int len = B.legato ? std::max(1, next_hit - row) : std::max(1, std::min(next_hit - row, 4) * 3 / 4);
                    int role = B.role[row];
                    // the second bar of a chord moves: the fifth or octave on beat 3
                    if(!calm && !B.legato && role == br_root && (bi % 2) == 1 && row == 8 && B.mask != 0x4444) role = (bi / 2) % 2 ? br_fifth : br_octave;
                    int deg = root;
                    int oct = 0;
                    switch(role) {
                    case br_octave: oct = 12; break;
                    case br_fifth: deg = root + 4; break;
                    case br_below: deg = root - 1; break;
                    case br_third: deg = root + 2; break;
                    case br_approach: deg = chord_change ? next_root + (next_root > root ? -1 : 1) : root + 4; break;
                    default: break;
                    }
                    if(chord_change && next_hit >= 16 && row >= 10 && !offbeat_drop && role != br_approach) deg = next_root + (next_root > root ? -1 : 1); // approach tone
                    const int n = pitch(key_ - 12, deg) + oct;
                    // at high intensity root notes gain an octave (an alternative event, not an extra one)
                    const bool root_hit = role == br_root && bass_octave_layer_;
                    add(bar, row, ch_bass, {n}, len, row % 4 == 0 ? 112 : 104, root_hit ? layer_max_high : layer_always);
                    if(root_hit) add(bar, row, ch_bass, {n, n + 12}, len, 104, layer_min_high);
                }
                if(bass_run) {
                    const int target = next_root + (next_root <= root ? 7 : 0);
                    for(int i = 0; i < 4; ++i) add(bar, 12 + i, ch_bass, {pitch(key_ - 12, target - 4 + i)}, 1, 96 + 4 * i);
                }
            }
            if(k == K::breakdown && bi < bars - 1 && (bi % p.bars_per_chord) == 0) {
                // a soft sub note under the breakdown pads
                add(bar, 0, ch_bass, {pitch(key_ - 12, root)}, p.bars_per_chord * 16 - 2, 70, layer_min_mid);
            }
            if(k == K::outro && bi == bars - 2) add(bar, 0, ch_bass, {pitch(key_ - 12, root)}, 14, 96); // the cadence chord's root
            if(final_bar) add(bar, 0, ch_bass, {pitch(key_ - 12, 0)}, 16, 110);

            // pads (a new chord at each chord change; gated in the breakdown when the song has a gate)
            const bool pad_sustain = (k == K::intro && !(beat_intro_ && bi < bars / 2 && !p.quiet)) || k == K::breakdown || k == K::bridge || k == K::outro || (k == K::verse && stage >= 1) ||
                                     (big(k) && stab_mask_ == 0);
            const bool new_chord = bi % p.bars_per_chord == 0 || chord_id(p, bi - 1) != cid;
            int chord_rows = 0;
            for(int b = bi; b < bars && (b == bi || (b % p.bars_per_chord != 0 && chord_id(p, b) == cid)); ++b) chord_rows += 16;
            // suspensions: a sus4 resolving to the triad on phrase starts and cadences
            // (never on the song's very first chord: an opening suspension sounds like a wrong chord;
            // tested after the draw so the random sequence stays the same)
            const bool sus_drawn = new_chord && chord_rows >= 16 && !power_ && color_at(p, bi) == c_home && (bi % 8 == 0 || root == cad_deg_) && r_.chance(calm ? 0.15f : 0.4f);
            const bool sus = sus_drawn && !(index == 0 && bi == 0);
            if(sus_drawn && !sus && !final_bar && new_chord && pad_sustain) (void)r_.range(-5, 5); // the dropped sus chord's velocity draw
            if(final_bar) {
                emit_chord(bar, 0, 0, 16, 92, layer_always);
            } else if(new_chord && pad_sustain) {
                const int vel = k == K::verse ? 62 : k == K::intro ? 70 : 85;
                const uint8_t lay = k == K::verse && stab_mask_ != 0 ? layer_max_high : layer_always;
                const uint16_t gate = k == K::breakdown ? gate_mask_ : 0;
                emit_chord(bar, 0, root, chord_rows - 1, vel, lay, 0, false, gate, sus);
            }
            if(!final_bar && stab_mask_ != 0 && (big(k) || (k == K::verse && p.variant == 1))) {
                for(int row = 0; row < 16; ++row) {
                    if(!(stab_mask_ & (1u << row))) continue;
                    if(fill && row >= 12) continue;
                    emit_chord(bar, row, root, 1, 92, k == K::verse ? layer_min_high : layer_always, 0, true);
                }
                if(k != K::verse && new_chord) emit_chord(bar, 0, root, chord_rows - 1, 50, layer_max_mid, 0, false, offbeat_drop ? gate_mask_ : 0);
            }

            // fx: crashes, impacts, risers, sweeps
            const bool entry = bi == 0 && (big(k) || k == K::groove || (k == K::verse && index > 1) || k == K::outro || (prev && prev->downlift));
            if(entry) add(bar, 0, ch_fx, {dr_crash}, 16, 100);
            if(bi == 0 && prev && prev->impact) add(bar, 0, ch_fx, {dr_impact}, 16, 110);
            if(bi == 0 && prev && prev->downlift) add(bar, 0, ch_fx, {dr_downlifter}, 32, 96);
            if(bi == 0 && T == tr_filter_open && !last_section) add(bar, 0, ch_fx, {dr_lp_open}, bars * 16 - 1, 100);
            if(!last_section && last_bar) {
                if(T == tr_short_riser) add(bar, 0, ch_fx, {dr_riser}, 16, 92);
                if(T == tr_pause) add(bar, 8, ch_fx, {dr_swell}, 8, 72); // only a faint swell into the downbeat
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
        render_counter(index);
        render_build(index);
    }

    // Chord voicing with smooth voice leading: of all inversions/octaves
    // inside the pad register, take the one closest to the previous voicing.
    // `sus`: a sus4 for the first half bar, then the chord (a suspension).
    void emit_chord(uint32_t bar, int row, int root, int len, int vel, uint8_t lay, int cut_rows = 0, bool stab = false, uint16_t gate = 0, bool sus = false) {
        const int tones = power_ ? 3 : seventh_ && !stab ? 4 : 3;
        int pcs[4];
        for(int i = 0; i < tones; ++i) pcs[i] = pitch(key_, root + 2 * i);
        if(power_) pcs[1] = pitch(key_, root + 4), pcs[2] = pitch(key_, root) + 12; // root, fifth, octave
        int best[4] = {}, best_cost = 1 << 30;
        const int lo = pad_lo_, hi = pad_hi_;
        for(int inv = 0; inv < tones; ++inv) {
            for(int oct = -1; oct <= 1; ++oct) {
                int v[4];
                for(int i = 0; i < tones; ++i) {
                    int n = pcs[(i + inv) % tones] + 12 * oct + ((i + inv) >= tones ? 12 : 0);
                    v[i] = n;
                }
                if(power_ && inv != 0) continue; // power chords stay in root position
                bool ok = true;
                for(int i = 0; i < tones; ++i) ok = ok && v[i] >= lo - (power_ ? 7 : 0) && v[i] <= hi + (power_ ? 5 : 0);
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
        int l = cut_rows > 0 ? std::min(len, cut_rows) : len;
        const note_fx fx = chip_chords_ ? note_fx::arp : stab ? note_fx::stab : note_fx::none;
        if(sus && !stab && l > 8) {
            // the third (or the tone a step above the root) moves up a step: sus4
            int s[4];
            std::copy(best, best + tones, s);
            const int third = pitch(key_, root + 2) % 12;
            for(int i = 0; i < tones; ++i) {
                if(((s[i] % 12) + 12) % 12 == ((third % 12) + 12) % 12) s[i] = pitch(key_, root + 3) + 12 * ((s[i] - pitch(key_, root + 2)) / 12);
            }
            if(tones == 4) add(bar, row, ch_pad, {s[0], s[1], s[2], s[3]}, 8, vel, lay, fx);
            else add(bar, row, ch_pad, {s[0], s[1], s[2]}, 8, vel, lay, fx);
            s_.events.back().gate = gate;
            row += 8;
            l -= 8;
            bar += static_cast<uint32_t>(row / 16);
            row %= 16;
        }
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
    // drop) for low leads; never in wartime (the lead stays low).
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
        const int up = high && !war_ && lead_class_ == 0 && mx + 12 <= 80 ? 12 : 0; // the drop's second half, an octave up
        for(lead_note const& n : phrase_) {
            const int note = n.note + up;
            if(n.dbl && !war_ && note + 12 <= 86) {
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
    // `start_row`, every arp_rate_ rows (a 3-row rate drifts across the
    // bar). `evolve`: a long line in 4-bar arcs, the pattern rotating every
    // 2 bars and climbing a chord tone in the second half of each arc.
    void place_arp(plan_item const& p, int bi, int bars, int start_row, int vel, uint8_t lay, int base_shift = 0, bool evolve = false) {
        const int nb = static_cast<int>(p.sec.bars);
        const int len = static_cast<int>(arp_order_.size());
        int idx = 0;
        for(int pos = start_row;; pos += arp_rate_) {
            const int b = bi + pos / 16, row = pos % 16;
            if(b >= bi + bars || b >= nb) break;
            const int k = b - bi;
            const int root = chord_at(p, b);
            int tone = arp_order_[static_cast<std::size_t>((idx + (evolve ? k / 2 : 0)) % len)];
            if(evolve && k % 4 >= 2) tone = std::min(4, tone + 1);
            const int step = idx;
            ++idx;
            int deg = root + (tone == 3 ? 7 : tone == 4 ? 9 : tone * 2);
            if(arp_wide_ && p.sec.start_bar > solo_bar_) deg = wide_deg(p, b, step, b - bi); // after the solo, the arp keeps its new figure
            int n = pitch(arp_base_ + base_shift, deg);
            while(n > 88) n -= 12;
            const int accent = row % 4 == 0 ? 12 : row % 2 == 0 ? 4 : 0;
            const int arc = evolve ? 4 * (k % 4) : 0;
            add(p.sec.start_bar + static_cast<uint32_t>(b), row, ch_arp, {n}, arp_len_, vel + accent + arc, lay);
        }
    }

    // The reference track's arp: a wide 4-note cell per beat (the fifth an
    // octave up, the third low, the root an octave up, the fifth), about
    // 1.5 octaves across; over a chord held into the next bar a pedal (the
    // high fifth on every other step); an added ninth on major chords now
    // and then; and the cell's first note jumps an octave on every other
    // bar's downbeat. `step` counts the arp's notes, `k` its bars.
    [[nodiscard]] int wide_deg(plan_item const& p, int b, int step, int k) const {
        const int root = chord_at(p, b);
        const bool held = b + 1 < static_cast<int>(p.sec.bars) && chord_id(p, b + 1) == chord_id(p, b) && chord_id(p, std::max(0, b - 1)) == chord_id(p, b);
        const bool major = pitch(key_, root + 2) - pitch(key_, root) == 4;
        const int s = step % 4;
        static constexpr int cell[4] = {11, 2, 7, 4};
        int deg = root + cell[s];
        if(held && s % 2 == 0) deg = root + 11;                                     // the pedal
        if(held && s == 3) deg = root + ((step / 4) % 2 ? 9 : 4);                     // the moving voice under it
        if(major && s == 3 && (step / 4) % 4 == 3) deg = root + 8;                    // the ninth (an octave up)
        if(s == 0 && (step / 4) % 4 == 0 && k % 2 == 1) deg += 7;                     // octave jump
        return deg;
    }

    // Phrase plans. A 4-cell (8-bar) phrase is statement, sequence,
    // restatement or development, cadence; the last cell of a section always
    // resolves on a held stable tone.
    void render_melody(std::size_t index) {
        using K = section_kind;
        plan_item const& p = plan_[index];
        const bool calm = s_.m == mood::calm;
        // before a drum break the melody ends (with its cadence) 4 bars early
        const bool more = index + 1 < plan_.size();
        const int bars = static_cast<int>(p.sec.bars) - (more && p.out == tr_drum_break ? 4 : more && p.out == tr_lead_climb ? std::max(1, build_bars(p)) : 0);
        const int root_a = prog_a_->deg[0], root_b = prog_b_->deg[0];
        const cell develop = dev_invert_ ? cell{0, cell_op::invert, ending::keep, false} : cell{dev_steps_, cell_op::retro_tail, ending::keep, false};
        const cell cadence{cad_steps_, cell_op::cadence, ending::close, false};
        const int under = arp_base_ >= key_ + 24 ? -12 : 0; // chorus arps sit under the lead
        switch(p.sec.kind) {
        case K::groove: break; // drums, bass and percussion only
        case K::arp_solo: // rendered by render_arp_solo
        case K::choir: break; // rendered by render_choir
        case K::intro:
            if(p.quiet) {
                place_arp(p, 0, bars, 0, 52, layer_always, 0, true); // pads and a long arp, no drums
            } else if(calm) {
                if(bars >= 8) place_arp(p, bars - 4, 4, 0, 58, layer_min_mid);
            } else if(bars >= 4) {
                place_arp(p, bars / 2, bars - bars / 2, 0, 54, layer_always, 0, true);
            }
            break;
        case K::verse:
            // call (lead, bars 1-2) and response (arp, bars 3-4), or a
            // continuous evolving arp under the lead
            if(arp_cont_) place_arp(p, 0, bars, 0, 50, layer_always, under, true);
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
                if(p.build_up && ph < bars / 2) continue; // (kept for the random sequence: the build's lead is muted below)
                place_motif(motif_a_, root_a, p, ph, 96, cl);
                if(!arp_cont_) place_arp(p, ph + 2, 2, 0, 66, layer_always);
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
                place_arp(p, ph, std::min(8, bars - ph), 0, calm ? 50 : 54, calm ? layer_min_mid : layer_always, under, true);
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
                // long (wartime) breakdowns keep developing: plain, sequence, inversion, ...
                static constexpr int steps[6] = {0, 0, -1, 2, 1, 0};
                const int st = j == 1 ? seq_step_ : steps[j % 6];
                const cell_op op = j % 3 == 2 ? cell_op::invert : j % 3 == 0 && j > 0 ? cell_op::retro_tail : cell_op::keep;
                const cell cl = last ? cell{j == 0 ? 0 : 2, cell_op::cadence, ending::close, false} : cell{st, op, ending::open, false};
                place_motif(mt, mroot, p, ph, 82 + std::min(j, 4) * 3, cl, 2);
                if(j % 2 == 1) flush_phrase();
            }
            flush_phrase();
            if(bars >= 16) place_arp(p, 0, 8, 0, 54, layer_min_mid, 0, !calm);
            else if(!calm && bars >= 8) place_arp(p, 0, bars / 2, 0, 48, layer_min_mid, 0, true);
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

    // The counter voice: guide tones (the chords' thirds, fifths and
    // sevenths, moving by the smallest step, with suspensions resolving down
    // over the bar line) in choruses, drops, bridges and repeated verses;
    // fills into each 4-bar phrase in verses; in wartime, 16th-gated
    // supersaw chords.
    void render_counter(std::size_t index) {
        using K = section_kind;
        if(counter_mode_ == 0) return;
        plan_item const& p = plan_[index];
        const K k = p.sec.kind;
        const bool calm = s_.m == mood::calm;
        const bool more = index + 1 < plan_.size();
        const int stop = static_cast<int>(p.sec.bars) - (more && p.out == tr_drum_break ? 4 : 0);
        const uint32_t b0 = p.sec.start_bar;
        auto span = [&](int bi) {
            int rows = 0;
            const int cid = chord_id(p, bi);
            for(int b = bi; b < stop && (b == bi || (b % p.bars_per_chord != 0 && chord_id(p, b) == cid)); ++b) rows += 16;
            return rows;
        };
        auto new_chord = [&](int bi) { return bi == 0 || bi % p.bars_per_chord == 0 || chord_id(p, bi - 1) != chord_id(p, bi); };
        if(counter_mode_ == 3) {
            if(!(big(k) || k == K::verse)) return;
            const uint16_t gate = gate_mask_ ? gate_mask_ : 0xB6DB;
            for(int bi = 0; bi < stop; ++bi) {
                if(!new_chord(bi)) continue;
                const int root = chord_at(p, bi);
                int a = pitch(key_, root);
                while(a < ctr_lo_) a += 12;
                while(a > ctr_lo_ + 11) a -= 12;
                int b = pitch(key_, root + 4);
                while(b <= a) b += 12;
                int c = pitch(key_, root + 2) + 12;
                while(c <= b) c += 12;
                add(b0 + static_cast<uint32_t>(bi), 0, ch_counter, {a, b, c}, span(bi) - 1, k == K::drop ? 96 : 78, k == K::verse ? layer_min_high : layer_always);
                s_.events.back().gate = gate;
            }
            return;
        }
        const bool guide = big(k) || k == K::bridge || (k == K::verse && !calm && p.variant == 1);
        if(guide) {
            const uint8_t lay = calm || k == K::verse ? layer_min_mid : layer_always;
            const int vel = calm ? 64 : 76;
            for(int bi = 0; bi < stop; ++bi) {
                if(!new_chord(bi)) continue;
                const uint32_t bar = b0 + static_cast<uint32_t>(bi);
                const int root = chord_at(p, bi);
                const int rows = span(bi);
                auto choose = [&](int avoid) {
                    int best = -1, best_cost = 1 << 30;
                    for(int t : {2, 4, 6, 0}) {
                        if(t == 6 && !seventh_) continue;
                        for(int oct = -2; oct <= 3; ++oct) {
                            const int n = snap(pitch(key_, root + t) + 12 * oct, bar);
                            if(n < ctr_lo_ || n > ctr_hi_ || n == avoid) continue;
                            const int cost = last_ctr_ < 0 ? std::abs(n - (ctr_lo_ + 7)) : 2 * std::abs(n - last_ctr_) + (n == last_ctr_ ? 3 : 0) + (t == 0 ? 2 : 0);
                            if(cost < best_cost) best_cost = cost, best = n;
                        }
                    }
                    return best;
                };
                const int n = choose(-1);
                if(n < 0) continue;
                int row = 0;
                if(!calm && bi > 0 && last_ctr_ - n >= 1 && last_ctr_ - n <= 2 && r_.chance(0.5f)) {
                    add(bar, 0, ch_counter, {last_ctr_}, 4, vel + 6, lay); // the suspension
                    row = 4;
                }
                if(counter_pulse_ && rows >= 16) {
                    add(bar, row, ch_counter, {n}, 8 - row, vel, lay);
                    last_ctr_ = n;
                    const int m2 = choose(n);
                    if(m2 >= 0) {
                        add(bar, 8, ch_counter, {m2}, rows - 9, vel - 4, lay);
                        last_ctr_ = m2;
                    }
                } else {
                    add(bar, row, ch_counter, {n}, rows - row - 1, vel, lay);
                    last_ctr_ = n;
                }
            }
        }
        if(k == K::verse && counter_mode_ == 2) {
            // a 4-note run in 8ths at the end of every 4-bar phrase, falling
            // stepwise onto the next chord's third
            for(int bi = 3; bi < stop - 1; bi += 4) {
                const int target = chord_at(p, bi + 1) + 2;
                int base = key_;
                while(pitch(base, target + 3) < ctr_lo_) base += 12;
                while(pitch(base, target + 3) > ctr_hi_) base -= 12;
                for(int i = 0; i < 4; ++i) add(b0 + static_cast<uint32_t>(bi), 8 + 2 * i, ch_counter, {pitch(base, target + 4 - i)}, 2, 66 + 4 * i, layer_always);
                last_ctr_ = pitch(base, target + 1);
            }
        }
    }

    void describe() {
        char buf[512];
        const int pc = ((key_ % 12) + 12) % 12;
        auto prog_str = [](progression const* pr) {
            std::string s;
            for(int i = 0; i < 4; ++i) {
                if(i) s += "-";
                s += color_names[pr->col[i]];
                if(pr->col[i] != c_med3 && pr->col[i] != c_med6) s += std::to_string(pr->deg[i] + 1);
            }
            return s;
        };
        std::string form;
        for(auto const& sec : s_.sections) {
            if(!form.empty()) form += " ";
            form += section_name(sec.kind);
            form += std::to_string(sec.bars);
        }
        static char const* const reg[] = {"low", "mid", "high"};
        std::string mod;
        if(mod_shift_) mod = ", modulates +" + std::to_string(mod_shift_) + " at bar " + std::to_string(mod_bar_);
        std::snprintf(buf, sizeof buf,
                      "seed %llu, %s, %s, %.0f BPM, %s %s, prog %s / %s / %s, groove %s/%s, bass %s/%s, %s lead, motifs %s/%s%s%s%s, %u bars (%.0f s): ",
                      static_cast<unsigned long long>(s_.seed), mood_name(s_.m), s_.style.c_str(), static_cast<double>(s_.bpm), note_names[pc], mode_->name,
                      prog_str(prog_a_).c_str(), prog_str(prog_b_).c_str(), prog_str(prog_c_).c_str(), grooves[groove_a_].name, grooves[groove_b_].name,
                      bass_names[bass_a_], bass_names[bass_b_], reg[lead_class_], mstyle_names[static_cast<int>(style_a_)], mstyle_names[static_cast<int>(style_b_)],
                      song_swing_ ? ", swing" : "", chip_chords_ ? ", chip chords" : "", mod.c_str(), s_.total_bars, s_.duration_seconds());
        s_.description = buf + form + "; transitions: " + (transitions_.empty() ? std::string("none") : transitions_);
    }
};

} // namespace

song compose(uint64_t seed, mood m) { return composer(seed, m).run(); }

} // namespace geosim::client::audio
