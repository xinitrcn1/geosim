# 14 — Audio: procedural dynamic music

All music is generated in-game. No recorded songs ship. A seeded **composer**
writes a tracker-style score of 2–3.5 minutes; 10–35 % of songs, depending
on the mood, are long songs of 5–10 minutes, and about one in four of the
rest is a 30–80 s intermission. A small **synthesizer** plays it
through SDL3 audio, and the **mood** of the player's nation steers both the
next song and the arrangement of the one playing now. Code: `src/client/audio`.

| File | Role | Thread |
|---|---|---|
| `song.hpp` | score data: patches, events, sections, mix | — |
| `composer.*` | seed + mood → `song` (form, harmony, motifs, rhythm, sounds) | worker |
| `synth.*` | `player`: sequencer, voices, drum models, mixer, effects, limiter | audio |
| `jukebox.*` | song → gap → next song; mood wrap-up | audio |
| `music.*` | `music_system`: SDL3 audio stream, worker thread, lock-free handoff | UI / worker / audio |
| `render.*` | offline WAV rendering and measurements | main |

`geosim_audio` (all of these except `music.cpp`) has no SDL dependency. It is
always compiled with `-O2`, because in a Debug build the synth would not keep
up with real time.

## Style target and what the references taught

The user's style references are a long (16:38) Ogg render and its Symphonie
Pro module, an early-2000s amateur hardtrance track (7:30 MP3), and a
mid-1990s FastTracker 2 module (a cover of a 1997 eurodance hit, 32
channels). They are used for analysis only. They are not in the repository,
and nothing derived from their audio or samples ships. What was measured
(ffmpeg + small Python scripts; low-band onset autocorrelation; the module's
chunk headers, position list and pattern events):

- **Tempo:** 146.4–146.6 BPM in every 40 s window. The module's tempo field
  is 118 (× 1.24 = 146.3 BPM). Patterns are 128 rows at speed 3, i.e. 8 rows
  per beat and 4 bars per pattern. The text header says "Hardtrance, 16
  channels, 16-bit samples, 1998".
- **Groove:** kick on every beat, an off-beat instrument on the "and"s,
  closed hats/shaker on 16ths, clap on 2 and 4. Chord stabs with octave
  doubling sit at the syncopated 16th steps 2, 4, 7, 10, 12, 14.
- **Harmony:** the off-beat bass runs an 8-bar cycle, i i VII VI | i i VI iv
  in D minor, with long one-root pedal sections in the middle.
- **Structure:** phrases of 8 bars and sections of 16–32 bars. The loudness
  curve shows an intro build (~50 s), three breakdowns (short-term loudness
  drops 5–10 dB) and an outro fade. Integrated −18.8 LUFS, LRA 9 LU, true
  peak −3.4 dBTP.

The hardtrance track (same tools, plus band-split per-bar energies and
spectrograms of each section change; bar = 1.50 s):

- **Tempo:** 160.2 BPM (onset autocorrelation, then a tempo/phase fit over
  80–200 s and 240–400 s: 160.2 and 160.15). Loud master: −7.4 LUFS
  integrated, LRA 5.4 LU.
- **Structure:** a pad-only intro of 8 bars; kick and bass enter at bar 8,
  more mid layers at 16, hats at 32; the full section starts at bar 48 and
  runs 64 bars with a crash every 8; 8 bars with the bass out; a ~44-bar
  breakdown (kick out, sustained sub and pads, the lead building in 4-bar
  cycles); a 96-bar second main section; a 32-bar second breakdown; a short
  fade.
- **Transitions — no long noise riser anywhere.** What it does instead:
  elements simply enter on 8/16-bar lines (with a crash); a low-pass sweep
  closes the top end within ~1 s into the bass-less part; a falling noise
  sweep (downlifter) after the breakdown starts; one short pitched sweep up
  over a single bar (1.2 → 5.4 kHz, then held) right before the second
  drop; the kick drops out and comes back.
- **Groove:** pumping off-beat bass under a four-on-the-floor kick in the
  main sections (sidechain-like gaps visible in the low band).

The XM module (own parser for the header, order list and packed patterns;
ffmpeg's libopenmpt decoder for loudness):

- **Tempo:** speed 6, BPM 140 (16th-note rows, 4-bar patterns), A minor;
  5:02, 46 orders, −10.0 LUFS, LRA 5.6 LU.
- **Groove:** two layered kicks on every beat with a doubled kick on the
  last 8th of each 4-bar pattern, closed hats on the beats with 16th
  pickups, open hats on the off-beats, clap on 2 and 4 with a pickup on the
  pattern's last 16th, a crash at the start of each pattern. Bass in 3-3-2
  (rows 0, 6, 12) in the verses and a syncopated 16th pattern in the
  choruses.
- **Harmony and lead:** long held chords (3 channels, note-off after 3
  beats, a new chord every 2 bars); a syncopated riff doubled an octave up
  and **echoed by hand** on extra channels 2 rows later at lower volume,
  panned (2360 volume and 705 panning commands). 8th-note chord pulses and
  echoed arpeggios in the choruses.
- **Structure and transitions:** it opens on drums and bass at full level,
  then 8 bars of chords alone, then layers arrive pattern by pattern; the
  main sections swap layers every 4–8 bars; a **drums-and-bass-only break**
  (the melody and chords drop out, the kick keeps going) before a chorus;
  a fade-out at the end. No riser at all.

Adopted: 4/4 on a 16th-note grid with tracker ticks, four-on-the-floor,
off-beat hats and bass, the stab rhythm, the i–i–VII–VI cycle (one of the
progressions), 8-bar phrasing, breakdown → build → drop, and a loudness
target near −18 LUFS in game. From the hardtrance track: varied hand-overs
instead of a riser every time (filter sweeps, downlifters, one-bar sweeps,
kick drop-outs and returns, plain cuts), and an off-beat-bass drop. From
the module: the drums-and-bass break and the beat-first intro. Its tracker
echo is what the ping-pong delay already does. Changed for this game: songs
last 2–3.5 minutes (long ones up to 10) instead of 16, and use synthesised "sample-like" waveforms
instead of samples. The first rounds kept every song at 110–128 BPM of
"informal" techno. The style target is now broader: "upbeat" applies only
to the joyful mood (triumph), and the other moods draw on several Amiga-era
style families (techno, synthwave, Amiga punk, chip/retro, breaks,
eurodance, lo-fi; see [Composer](#composer)) at 82–172 BPM. Wartime is
early-2000s amateur hardtrance at 138–148 BPM.

## Synthesis

The tracker model has 9 channels: **kick, snare/clap, hats, bass, lead, arp,
pad, fx, counter** (a second melodic voice). Each channel has one patch and
a pool of 6 voices.

- **Tonal voices** aim at late-90s / early-2000s analogue-modelling synths
  (JP-8000 supersaw, Virus / Nord Lead style), not chiptune:
  - Oscillators: two band-limited (PolyBLEP) saw / pulse oscillators with
    PWM, or triangle, sine or noise, plus a sine sub-octave.
  - **Unison** (`patch::unison`, 0–4): extra PolyBLEP saws per note spread
    over ±`spread` cents, i.e. a supersaw.
  - Gentle tanh saturation.
  - The filter: a TPT state-variable low-pass with resonance, its own
    envelope, key tracking and velocity. It is 12 dB, or **24 dB**
    (`poles4`: a plain first stage, then the resonant one), which every
    melodic patch now uses.
  - Then a linear-attack / exponential ADSR. Leads add delayed vibrato and
    optional glide.

  The synth can still do a 32-step 8-bit wavetable and Paula-style
  sample-and-hold, but no patch uses them since the sound rework: they were
  the main source of the "8-bit" sharpness and of aliasing.
- **Chords** are paraphonic: one voice, up to 4 note oscillators into one
  filter. With `note_fx::arp` they play as a **tracker arpeggio**, cycling
  the chord notes every tick (~48 Hz at 120 BPM), the classic chip chord;
  only half the songs of the (now rare) chip family use it.
- **Mono channels** behave like tracker channels: a new note cuts the
  previous one with a 6 ms fade (no clicks), or with the patch release on
  the pad. Kit channels choke earlier hits (a closed hat cuts the open hat)
  but layer hits on the same tick.
- **Drum models** (selected by GM-like note numbers on kit channels):
  - kick: sine with exponential pitch drop (`kick_sweep`, 32 ms by
    default), noise click and tanh saturation (`kick_drive`, 1.6 by
    default; the hardtrance kick uses 12–20 ms and 3–4.2);
  - snare: two-sine body and high-passed noise;
  - clap: 3 band-passed noise bursts and a tail;
  - closed/open hat and crash: six TR-808 square oscillators and noise,
    high-passed, then low-passed at 11 kHz (less fizz);
  - rim, shaker and 3 toms;
  - riser: noise band-pass swept up 5 octaves, plus a rising tone;
  - downlifter: noise band-pass falling 4 octaves with a falling tone;
  - swell (reverse cymbal), and impact (low boom and noise burst).
- **Bus filter sweep:** two fx-channel events are not sounds: `dr_lp_close`
  closes a resonant low-pass (16 kHz → 280 Hz, smoothstep) over the event
  length and reopens at its end; `dr_lp_open` starts at 280 Hz and opens.
  While a sweep runs, every channel except the kick and the fx goes through
  its own SVF, so the beat keeps going while the rest darkens.
- **Trance gate:** `note_event::gate` is a 16-step mask; a gated pad voice
  is open for 70 % of each enabled 16th (1.5 ms attack, 5 ms release).
- **Stabs:** `note_fx::stab` gives a chord a 2 ms attack and a fast decay to
  silence whatever the patch, so the detuned-saw pad also plays short
  supersaw-like stabs.
- **Song end:** when a song plays to its end, the bass, lead and pad release
  over at least 1.4 s, so the final chord and note decay instead of being
  cut.

## Mixer

Each channel is a mono sum scaled by its gain, then:

- **Sidechain ducking:** a 4 ms dip, recovering over ~90 ms after every
  kick. It is per channel (pad 0.6, bass 0.55, arp 0.3) and gives the
  "pumping" groove.
- **Constant-power pan:** arp and lead are panned to opposite sides, in an
  Amiga spirit but softened.
- **Ensemble chorus** (`mix_settings::chorus`, per channel): a 2048-sample
  delay line read by two taps at 7–13 ms, swept in quadrature at 0.6 Hz,
  one per side. Pads get 0.5 (synthwave 0.7, arpeggiated chip chords 0.2),
  the counter voice 0.35 (strings 0.6), the arp 0.2.
- **Sends:** a tempo-synced **ping-pong delay** (dotted 8th or 8th, feedback
  0.3–0.45, damped) and a Freeverb-style **reverb** (4 combs + 2 allpasses
  per side).
- **Master:** DC blocker → volume (smoothed) → **look-ahead peak limiter**
  (64 samples, ceiling −1 dBFS, ~50 ms release) → soft clip above 0.9,
  bounded below 1.0. Non-finite samples are replaced by silence before the
  limiter.

Levels were set from solo renders. For seed 7, tense, the channel RMS
values were: kick −20.6, bass −22.4, lead −27.9, pad −29.1, snare −35.0,
arp −37.1, hats −38.8, fx −39.5 dBFS. The lead was then lowered (mix gain
0.6 → 0.36) and normalised per patch (PWM 1.0, detuned saws 1.95, chip
sample 1.5, square + fifth 1.0, measured): where the lead plays it now sits
about 4.5–8 dB (mean 6.4) under the rest of the mix instead of −0.1 to
8 dB (mean 3.9).

## Composer

`compose(seed, mood)` is deterministic: it uses its own splitmix64 PRNG and
no `std::` distributions, so the same seed and mood give a byte-identical
WAV.

1. **Style.** Each song belongs to a style family, drawn with weights that
   depend on the mood. The family sets the tempo range, the groove
   templates, the bass patterns, the stab rhythm and the sounds. Only
   triumph (the joyful mood) stays "upbeat": its tempo is at least 112 BPM.
   Calm stays relaxed (at most 120 BPM) and keeps the mundane parts (arp
   answers, sparse counter voice, no key change).

   **Sounds** (per song, from a few designs per role, all on 24 dB
   filters):
   - bass: warm triangle + sub, analogue square + sub, Virus-style two-saw
     bass, resonant acid saw, reese;
   - lead: warm PWM pair, supersaw (2 unison saws ± 18 cents), Nord-style
     saw with a quiet square an octave up and glide, filtered square +
     fifth; vibrato 0.05–0.12 semitones;
   - arp: rounded plucks (pulse, saw or a 2-saw trance pluck; 900 Hz
     cutoff with a 2.6-octave filter envelope);
   - pad: a supersaw pad (2 unison saws + a detuned saw; wartime: 4 unison
     saws ± 28 cents, JP-8000 style) with chorus;
   - counter: string machine, synth brass, soft square, or a supersaw for
     the gated wartime chords.

   | Family | BPM | Grooves (verse / chorus) | Bass | Sound | Moods (weight) |
   |---|---|---|---|---|---|
   | techno | 116–128 | four, broken, electro | off-beat, rolling, house, octaves | as before | calm 1, tense 1.2, crisis 1, triumph 1 |
   | synthwave | 88–108 | synthwave, half-time / synthwave, four (outrun) | 8th or 16th pedal, octaves | lush saw pad, big reverb snare, held chords | calm 1.2, tense 1.4, crisis 1, triumph 0.6 |
   | amiga punk | 152–172 | punk, d-beat | 8th roots with fifths, walking, gallop | a sampled-synth "guitar" (saw + unison + pulse, soft drive, 24 dB) for power chords (8th chugs in the chorus), tight snare, crash-ride | tense 0.6, crisis 1.2, triumph 0.4 |
   | chip | 124–146 | chip, breakbeat, four | octaves, bounce, walking, 8ths | arpeggiated soft-square chords (50 %), square bass; rare since the sound rework | calm 0.15, tense 0.15, crisis 0.1, triumph 0.3 |
   | breaks | 136–156 | breakbeat, amen, half-time | held reese, syncopated, house | rave stabs | tense 1, crisis 1.3 |
   | eurodance | 124–136 | four, chip / four | off-beat, octave 16ths, rolling | stabs on the off-beats | triumph 1.5 |
   | lo-fi | 82–100 | half-time, shuffle, breakbeat | held, walking, bounce | soft kick, triangle bass, heavy swing | calm 2 |
   | hardtrance | 138–148 | hardtrance, trance / hardtrance | off-beat, gallop, rolling | see *Wartime* | wartime only |

2. **Basics.** The tonic is A2–G#3. The mode depends on the mood (calm:
   ionian / lydian / mixolydian / dorian; tense: dorian / aeolian /
   phrygian; crisis: aeolian / phrygian / harmonic minor; triumph: ionian /
   mixolydian / lydian; wartime: aeolian / phrygian / harmonic minor /
   dorian). Swing (1 tick ≈ 58 % on odd 16ths) depends on the family; the
   shuffle groove swings by 2 ticks. 7th chords are likelier in calm,
   triumph, lo-fi and synthwave songs; punk uses power chords (root, fifth,
   octave).
3. **Harmony.** There are three 4-chord progressions per song: A (verse),
   B (chorus) and C (bridge). They come from a table of 41 entries:
   - the diatonic ones (I–V–vi–IV, i–VI–III–VII, i–i–VII–VI...);
   - borrowed and chromatic ones: I–bVI–bVII–I, I–iv–I–V, IV–iv–I,
     I–III–vi–IV (chromatic mediant), I–VI–ii–V (secondary dominant),
     vi–IV–I–bVII, i–VII–VI–V (andalusian), i–iv–V–i and i–VI–iv–V with a
     major V, i–III–VII–IV, i–VI–VII–I (picardy), i–VI–i–bII (neapolitan);
   - relative-key progressions for bridges and breakdowns (vi–iii–IV–I,
     III–VII–iv–III, VI–III–iv–V...).

   Outside calm, choruses weigh the coloured progressions ×2, and 60 % of
   bridges use a relative-key one. A coloured chord is written as the
   diatonic chord on the same degree plus a *colour*: a local scale given
   by a tonic offset, a mode and a degree (parallel aeolian, ionian,
   harmonic minor, dorian or phrygian; E ionian or A mixolydian for the
   mediants III and VI over C). Each bar gets a pitch-class mask of its
   local scale and chord. Every tonal note emitted in a coloured bar is
   **snapped** into that scale: it stays if it belongs; otherwise it moves a
   semitone to a chord tone, or failing that to a scale tone. So the
   melody, arp, bass and pad logic stay diatonic while the harmony
   borrows, and a vi in the table sounds as bVI. A held lead or counter
   note that crosses into a bar where it would sound different is split
   there and re-sung in the new harmony. Minor songs close on a major V
   (harmonic minor) in 40 % of cases instead of bVII.
   - **Suspensions:** on phrase starts and on the cadence chord (40 %, calm
     15 %), the pad plays a sus4 for half a bar, then the triad.
   - **Key change:** 30 % of tense, crisis and wartime songs, and 50 % of
     triumph songs, with two or more choruses or drops modulate for the
     last one and what follows: +2 semitones (60 %), +1 or +3. The bar
     before plays the new key's dominant (VI major, bVI or bVII).
     `song::final_key` is the tonic after the change, and the cadence and
     the final tonic are in that key.
4. **Groove templates.** Every song has one template for its intro, verse,
   bridge and outro, and one for its choruses and drops. They are the same
   template 35 % of the time; wartime drops are always hardtrance. A
   template has 16-step masks for the kick, the kick on a phrase's last
   bar, the backbeat, ghost notes, closed, open and extra hats, plus a
   swing. The templates are four-on-the-floor, broken 3-3-2, electro
   (1, 2a, 3+), breakbeat, amen, half-time (snare on 3), shuffle,
   synthwave (kick 1 and 3), punk, d-beat, chip, hardtrance and trance.
   The song adds its own extra kick on every other bar (35 %, never in
   hardtrance or punk) and a ghost-note set. Outside calm and wartime,
   4-bar phrases sometimes end on a three-16th snare pickup (layer ≥ 0.4).
5. **Bass patterns** (one per section group): off-beat, rolling, gallop,
   octaves, house (generated), walking (root, third, fifth, approach,
   legato), syncopated (3-3-2 with fifth, octave and approach), held (whole
   bar, then an approach), 16th pedal, bounce, 8th pulse, octave 16ths.
   Each hit has a role (root, octave, fifth, third, below, approach).
   Outside calm, the bass moves on the second bar of a chord (fifth or
   octave on beat 3) and runs up four 16ths into the next chord at the end
   of 8-bar phrases. Approach tones lead into chord changes as before.
6. **Motifs.** There are two 2-bar motifs, A for verses and B for choruses
   (B is denser). Each has a rhythmic approach per song, weighted by mood:
   - **riff:** weighted sampling of the 32 rows, as before, notes at most
     6 rows long; staccato in 10–20 % of songs;
   - **legato:** 4–5 onsets, each held to the next (up to 14 rows);
   - **syncopated:** 3-3-2 cells (rows 0, 3, 6, 8, 11, 14...);
   - **anthem:** 3–5 long notes with wide, singing intervals (leaps of a
     4th to a 6th, then recovery).

   Pitch is a contour of steps, skips and rare leaps, with leap recovery.
   Strong beats always land on chord tones.
7. **Placement on the harmony** (unchanged). A motif is placed as 2-bar
   **cells**. A cell is transposed diatonically with the chord root
   (wrapped to ±3 steps), plus an optional melodic **sequence** step, and
   may be transformed: *invert*, *retro-tail* or *cadence*. Endings are
   **open** (a non-root chord tone, held up to half a bar) or **closed**
   (the most stable chord tone, held to the cell's end). A cell cut by the
   section's end gives its ending to its last placed note. Phrase plans
   (4 cells = 8 bars): statement, sequence (open), restatement or
   development, cadence. If a section's last chord shares no tone with the
   tonic triad, or is coloured, that last bar becomes the tonic.
8. **Registers.** The lead's register is chosen per song: **low** (a
   19-semitone window from A#2–C3, 35 %), **mid** (from G3, 40 %) or
   **high** (from D4, 25 %). Triumph and chip favour high, breaks and
   synthwave low, punk mid. Each cell takes the octave that keeps its notes
   in the window and joins the previous note smoothly. Only low leads go an
   octave up in the second half of a drop, and the high-intensity octave
   doubling stops at D6. With a low lead the pads move above it (floor
   A#3–C4); otherwise they voice within 17 semitones from `key + 7` (or
   `key − 5`). Arps sit an octave above the tonic (key + 12) under a high
   lead, two octaves (key + 24) otherwise, and always stay below E6.
9. **Arpeggios.** There are 11 chord-tone orders of 3–8 steps, at 16ths,
   8ths or dotted 8ths (a 3-row rate that drifts across the bar). Notes are
   plucks, or held for the full step (60 % at the slower rates). Outside
   calm the arps are **long, evolving lines**: in 4-bar arcs the pattern
   rotates every 2 bars and climbs a chord tone in the arc's second half,
   with rising velocity. They run through every chorus and drop (layer
   "always"; in calm ≥ 0.4), the second half of the intro, half the
   breakdown, and in 50 % of songs the whole verse instead of the 2-bar
   answers.
10. **Counter voice** (channel 9, `ch_counter`). Calm songs have none
    (50 %) or guide tones at intensity ≥ 0.4; other songs have guide tones
    (45 %) or guide tones and fills (55 %). Guide tones are the chords'
    thirds, fifths and sevenths, moving by the smallest step in their own
    window: under a mid or high lead from D3–E3, over a low lead from D4.
    Before a step down, the previous note is held 4 rows into the new bar
    and then resolves (a suspension, 50 %). 40 % of songs re-sing the
    guide tone on beat 3 with a neighbouring chord tone. They play in
    choruses, drops, bridges and repeated verses. Fills are four 8ths
    falling onto the next chord's third at the end of every 4-bar verse
    phrase. The patch is strings, brass, chip pulse or (wartime) supersaw.
11. **Form.** There are 8 templates. The earlier 4 are joined by two
    verse–chorus song forms (one with a bridge and a last chorus), an
    intro–chorus–verse–bridge–drop form, and a club form (drop, breakdown,
    drop). Fitting to the target length (140–195 s, at most 210 s) halves a 16-bar intro or
    breakdown, then drops a bridge, then halves 16-bar sections, then drops
    a repeated verse that would not join two equal sections. Fast songs are
    extended by doubling 8-bar verses and choruses, then by adding a chorus
    after the last drop (or a bridge and a chorus after the last chorus).
    Intermissions are fitted to 32–80 s at any tempo. The section rules
    (intro, verse call and response, chorus / drop, bridge, breakdown,
    outro and its final cadence) are as before. Synthwave and lo-fi
    choruses hold their chords instead of stabbing, and punk choruses chug
    power-chord 8ths.

    **Long songs** (5–10 min; 10 % of calm and triumph songs, 15 % of tense
    and crisis, 35 % of wartime) aim for 300–580 s and never pass 600 s.
    Some grow from a template by whole cycles before the outro: breakdown →
    drop in club and trance forms, otherwise alternating verse → chorus and
    bridge → chorus. The others (45 %, wartime 60 %) use the **epic form**,
    which follows the 16-minute reference module's layout (a hint from the
    user, not a fixed rule). Each section keeps the reference's share of
    the length, scaled to the target and rounded to 8 bars:

    | Section | Share (s of 862) | What happens |
    |---|---|---|
    | intro | 60 | pads and one long evolving arp; no drums, no bass |
    | build (verse) | 75 | kick and bass, then hats, then the backbeat, then the melody (from halfway) |
    | main (chorus) | 60 | |
    | breakdown | 30 | bass out; leaves on a riser (noise sweep and roll) |
    | drop | 150 | the main stretch |
    | break (breakdown) | 30 | |
    | main (chorus) | 60 | |
    | breakdown | 45 | pads and melody (the augmented theme) |
    | **groove** | 75 | a percussion section: drums, bass, shaker 16ths, rim off-8ths, a tom figure; no chords or melody |
    | break (breakdown) | 30 | |
    | main (drop) | 60 | ends on a *stop*: the middle break of the main |
    | main (drop) | 60 | ends on a *pause*: the last bar is almost silent, only a faint swell |
    | final main (drop) | 90 | the key change, if any, lands here |
    | outro | 37 | thins down to the pads |

    `section_kind::groove` is the new section kind. The player treats it
    like any other (the wrap-up jumps to the outro from it).

12. **Transitions** (`choose_arrangement`, own generator). Each section
   boundary gets one hand-over, weighted by context and never the same
   twice in a row:

   | Kind | What happens | Used for |
   |---|---|---|
   | cut | nothing but the crash on the next downbeat | anywhere |
   | fill | snare fill on the last 2 beats (the kick leaves beat 4) | anywhere |
   | toms | tom fill on the last beat | anywhere |
   | roll | snare roll, crescendo (2 bars; 4 out of a breakdown, 8 in wartime): quarters for the first half of a 4+-bar build, 8ths, then 16ths for the last quarter (at most 2 bars); only a 2+-bar build may end on one beat of 32nds (30 %) | into chorus / drop, out of a breakdown |
   | riser | noise riser, 2 bars (4 out of a breakdown, with a roll) | at most once per song, possible in 25 % of songs |
   | sweep | one-bar pitched sweep up | at most once per song, possible in 50 % |
   | filter-close | the bus low-pass closes over the last 2 bars | into a breakdown or outro |
   | filter-open | the section starts dark and the low-pass opens across it | intro, breakdown |
   | kick-out | the kick (and the bass in long sections) drops out for the last bar, with a short snare pickup | into chorus / drop, verse, outro |
   | stop | everything but the lead (and fx tails) stops for the last 2 beats, or the whole last bar | anywhere |
   | kick-return | the breakdown's kick comes back for its last bars, with a roll | out of a breakdown |
   | drum-break | lead, arp and chords drop out for the last 4 bars (the melody ends with its cadence before); drums and bass only | 16-bar verse / chorus / drop |

   **Crescendo devices.** Builds into a chorus or drop, and out of an intro
   or breakdown, choose among these too (the build lasts 2 bars, 4 out of an
   intro / breakdown; in wartime 4 and 8). Each song draws its own "taste"
   (every weight × 0.4–1.6), and the previous hand-over is excluded, so the
   same device never comes twice in a row. A device that replaces a
   channel's part mutes that channel over the build and adds its own events
   after the mutes.

   | Device | What happens | Used for |
   |---|---|---|
   | roll / clap-roll | accelerating snare or clap roll in the steps of *roll* above (never faster than 16ths except one final beat of 32nds), 40 → 120 velocity | builds |
   | tom-cascade | toms in 8ths (high / mid), the last bar 16th cascades high → low each beat, crescendo | builds, verse ends |
   | layer-in | the drums are rebuilt one by one: hats, then the kick, then off-beat bass roots, then a snare roll | intro, breakdown |
   | hat-ramp | hats replaced by 8ths, then 16ths, then 16ths with open 8ths, rising | builds |
   | arp-climb | the arp in 8ths then 16ths, an octave up halfway, with passing tones, rising | builds |
   | stab-ramp | chord stabs in quarters, 8ths, then 16ths, rising | builds |
   | pad-swell | one pad chord whose attack lasts the whole build (`note_fx::swell`) | builds |
   | kick-double | the kick in 8ths for the last 1–2 bars, its last beat in 16ths (wartime: the last two beats, a hardtrance kick roll) | into chorus / drop; out of a wartime breakdown |
   | pause | everything (the lead too) stops for the last bar, with a faint swell: a near-silent tension pause | epic form, before the final main |
   | lead-climb | the melody ends (with its cadence) a build early; the lead runs an octave up a scale in 8ths, then holds the fifth and bends a fourth up onto the tonic into the downbeat (`note_fx::slide_up`) | melodic sections, breakdown |

   Risers are now occasional: a long riser is possible in 25 % of songs and
   a one-bar sweep in 50 %, and both compete with every device above.

   Extras: a reverse cymbal into the next downbeat, a downlifter (and a
   crash) at the start of a calmer section, an impact on the drop. The
   chosen hand-overs are listed at the end of the song's description.
13. **Voice leading.** Each pad chord takes the inversion and octave, within
   the pad register, closest to the previous voicing.

### Wartime (hardtrance)

The fifth mood, for a player at war, is early-2000s amateur hardtrance.
It runs at **138–148 BPM**; the references run 140 (the XM), 146 (the
Symphonie module) and 160 BPM (the MP3). The earlier 140 BPM cap is gone,
so wartime now sits clearly above every other mood except punk. The rest
of the recipe:

- **Kick as the lead element.** The hardtrance kick drops in pitch fast
  (`kick_sweep` 12–20 ms) from 250–320 Hz onto the key's root (45–60 Hz).
  It has a long body (0.42–0.55 s), a loud click, `kick_drive` 3–4.2 (a
  distorted, pitched kick) and mix gain 0.8. It plays four-on-the-floor,
  with a 16th on the last beat of each phrase's last bar (`hardtrance`
  template). Kick rolls into the drops are 2.5× as likely (`kick-double`,
  ending on two beats of 16th kicks), and are possible out of a breakdown.
- **A low, almost bass-like lead** replaces the high synth. Its window
  starts at F#2–G#2 (19 semitones, song medians 49–57), an octave over the
  bass. Patches: a driven reese (two saws 20–30 cents apart), a hoover-ish
  PWM pulse against a detuned saw, or an acid saw (resonance 0.65, filter
  envelope, slides). Glide is on in 70 % of songs. The lead never goes up
  an octave and is never doubled an octave up. It ducks with the kick
  (0.35).
- **Bass:** off-beat, gallop (rows 2–3 of each beat) or rolling (all three
  16ths after the kick). The first drop keeps the song's own bass, and
  later drops switch to off-beat roots.
- **Gated supersaw:** the counter voice plays root–fifth–tenth chords on a
  supersaw (detune 25–32 cents) through the song's 16th trance gate, in
  drops and choruses (and verses at high intensity). The pads stay the
  wide detuned saw, gated in breakdowns.
- Hats are closed 16ths with open off-beats (or open off-beats only in
  the `trance` verse template), with a clap backbeat in 85 % of songs.
  Arps run in 16ths (or dotted 8ths) an octave above the tonic. Minor
  modes, no swing.
- **Forms:** six trance forms, always full length, with 16–32-bar
  breakdowns and 8-bar builds out of intros and breakdowns. The new ones
  are intro → drop → breakdown → drop, and intro 16 → verse 8 → chorus 8 →
  breakdown 16 → drop 32. A key change for the last drop is possible.

### Intensity layers

Events carry a layer tag: always, ≥ 0.4, ≥ 0.7, < 0.4 or < 0.7. The
complementary tags give alternatives rather than duplicates: for example
"root" below 0.7 and "root + octave" from 0.7 up. The player decides at
note-on from the live intensity, so layers come and go on note boundaries,
never mid-note. Layered parts:

- 16th hats, ghost snares, open-vs-closed hats in verses;
- a second backbeat sound, and rim/shaker 16ths at high intensity;
- the bass octave and the lead octave doubling at high intensity;
- the background arps (calm), the breakdown arps and sub, the counter
  voice in calm songs and verses, the gated supersaw in wartime verses.

Intensity also sets the brightness of lead, arp and pad: their cutoff is
multiplied by 2^((i − 0.55) · 1.6). Intensity is smoothed with a ~6 s time
constant, which gives gentle cross-fades.

## Mood mapping

`app.cpp` (`derive_music_mood`) reads only what the HUD already shows, every
2 s:

- `stress` = share of indicators coloured *bad* + 0.15 × bad alerts of the
  last 120 days (max 4) + 0.4 if a war alert involves the player;
- **wartime** whenever any nation the player controls is at war in the
  simulation (`sim_runner::player_at_war`: the nation's `at_war` array in
  the DCON container, read directly like the indicators); it overrides
  every other mood, with intensity at least 0.75;
- otherwise **crisis** if there is a war alert or stress ≥ 0.65; **tense** if ≥ 0.3;
  **triumph** if at least 40 % of indicators are *good* and none is bad, or
  after a peace with low stress; otherwise **calm**;
- intensity = 0.25 + 0.65 × stress (triumph: 0.55 + 0.3 × good share),
  +0.1 at speed ≥ 4, clamped to 0.15–1.

`music_system::set_mood(m, intensity)`:

- Intensity applies at once, smoothed in the player.
- A different mood held for 20 s makes the jukebox **wrap the song up**: at
  the next 8-bar line (not in the intro or outro) the player jumps to the
  outro. War starting or ending (to or from wartime) wraps up at once, at
  the next 8-bar line.
- The worker keeps the queued next song composed for the current mood.
- Between songs there are 3 s of silence while the delay and reverb tails
  ring out. The next song uses a new seed (splitmix chain).

## Real-time design

The audio callback (`SDL_OpenAudioDeviceStream`, F32 stereo 48 kHz) renders
blocks of 512 frames into a member buffer and hands them to
`SDL_PutAudioStreamData`. Inside it:

- **No allocation, no locks, no logging.** Voices, delay lines (2 s),
  reverb and limiter buffers are allocated when the player is constructed.
- **Parameters** (mood, intensity, volume, mute) travel as relaxed atomics.
- **Songs** are composed on the worker thread. The worker publishes the
  next song with `atomic<song*>::exchange` on `next_`, and the callback
  takes it with an exchange. Finished songs go into one of 8
  `atomic<song*>` retire slots by CAS, and the worker frees them. The audio
  thread never frees memory.
- **Sequencing is sample-accurate.** Ticks are counted in samples
  (`samples_per_tick` as a double, so tempo never drifts), and events are
  read sequentially from the pre-sorted array. The outro jump uses a binary
  search, which does not allocate.

SDL's own stream queue may allocate inside `SDL_PutAudioStreamData`; that is
SDL's design. The client does not feed a second stream to avoid it.

Offline, songs render 20–40× faster than real time on this machine (65×
before the unison saws and chorus; wartime, with 4-saw supersaw chords,
is the slowest at about 20×), so live playback costs up to about 5 % of
one core.

**No device:** if `SDL_InitSubSystem(SDL_INIT_AUDIO)` or opening the device
fails, the client logs `music: ... continuing without music` and runs
normally. Both cases were checked: `SDL_AUDIO_DRIVER=nonexistent`, and this
machine's missing ALSA device. `SDL_AUDIO_DRIVER=dummy` exercises the full
path without sound.

Keys and options: **M** mutes / unmutes; `--music 0`,
`--music-volume 0..1` (default 0.6), `--music-seed N` (default random).

## Offline rendering

```
geosim --render-music out.wav [--seed N] [--mood calm|tense|crisis|triumph|wartime]
       [--intensity 0..1] [--songs N] [--switch SECONDS:MOOD] [--solo CHANNEL]
```

This renders through the same jukebox and player, with no window and no
audio device, to 16-bit stereo 48 kHz. The report gives:

- duration, peak, RMS, and non-finite and clipped sample counts (the exit
  code is non-zero if any sample is non-finite or clipped);
- per song: score and played length, BPM, and an **independent tempo
  estimate** (autocorrelation of the low-band onset envelope);
- the share of low-band onsets on the 16th grid.

`--solo kick` isolates the kick for an exact grid check (any channel name,
including `counter`, works). `--switch` tests a
mid-song mood change and the wrap-up. `tests/test_music.cpp` (built with
`GEOSIM_BUILD_TESTS`) checks determinism, 30 s – 10 min length (full songs
at least 2 min), BPM range (calm ≤ 120, triumph ≥ 112, wartime 138–148), the
score's invariants, the final tonic chord and the lead's held final tonic
(in `final_key`), the roll cap (no run of three or more snare / clap
onsets closer than a 16th, except one beat of 32nds just before a section
boundary), the wartime low lead (median below C4) and hardtrance kick, and
40 s of finite output below 0 dBFS at full intensity, for every mood and
seven seeds. Over all of them it also checks that at least 4 style
families occur and that the songs' median lead notes span an octave or
more.

### Verification (12 songs: 4 moods × seeds 1, 7, 42)

- **Duration:** 146–164 s (score lengths 146.4–164.6 s).
- **BPM:** 113–128. The audio estimate matches the score within 0.1 BPM in
  every song.
- **Kick solo:** 100 % of detected kicks fall within 10 ms of the 16th
  grid, and the number detected equals the number played (213–287 per
  song).
- **Full mix:**
  - peak −0.84 to −2.16 dBFS;
  - 0 clipped and 0 non-finite samples;
  - seed 7 tense: −16.3 LUFS integrated at render volume 0.8, which is
    about −18.8 at the in-game default of 0.6;
  - the short-term loudness follows the form: intro −19, verse/chorus −16,
    breakdown −19.5, drop −14.4 LUFS.
- **Mood switch:** a calm song switched to crisis at 40 s wrapped up through
  its outro at 81.8 s. After the 3 s gap, a crisis song at 125 BPM began.
- **Determinism:** two renders of seed 99, triumph, are byte-identical.

Full-mix onset counts also include bass notes and kick/bass interference,
so their on-grid share (55–99 %) is not a timing measure. The kick-solo
check is.

### Verification of the lead, phrasing and transitions (200 scores)

Score statistics for 4 moods × seeds 1–50, before → after (an analysis
tool over `compose()`; not part of the build):

- **Lead register:** lowest lead note 45 (A2) → 60 (C4), median lowest 58 →
  66; share of lead notes above the pad's top note 35 % → 86 %; leaps over
  9 semitones within a bar 775 → 532 (what remains is in the motifs).
- **Lead level** (20 songs, intensity 0.5, RMS where the lead plays, lead
  solo vs the mix without it): −3.9 dB mean (−8.2 to +0.1) → −6.4 dB
  (−7.9 to −4.5).
- **Phrase ends** (a lead note followed by half a bar of rest, or a
  section's end): on a chord tone 85 % → 90 %, on a stable tone 49 % → 68 %,
  held ≥ 6 rows 41 % → 62 %; section ends on the tonic 45 % → 62 %; songs
  whose lead ends on a held tonic (or third) in the last bar 0 → 200.
- **Repetition:** songs with 2–3 identical consecutive 2-bar lead cells
  14 → 0.
- **Transitions:** 2–7 hand-overs per song (mean 5.2), of 12 kinds plus
  extras. Riser events 477 (every song) → 80 in 68 songs, of which 29 songs
  have a long riser and the rest a one-bar sweep.
- **The two reported songs** keep their basics (the summary line is
  unchanged up to the form). Seed 6509162505067969530 (calm): lead 66–93 →
  71–88, 100 % above the pads, lead/rest −1.4 → −5.1 dB (calm intensity
  0.3); the song now ends on a held E over a V–I cadence, where the lead
  used to stop with the drop, 9 bars before the end. Seed
  13761442689889167167 (calm): lead 59–80 → 71–88 (17 % → 83 % above the
  pads), lead/rest −1.5 → −5.0 dB; the breakdown sings E–G#–B–G# |
  D#–F#–B–G# | E (held) instead of the same figure twice with repeated
  notes, and the chorus and drop end on I (the lydian II of their last bar
  becomes I) instead of hanging on II.
- Renders of 10 songs (both seeds above, 4 moods, two intermissions of
  32.5 s and 76.5 s): 0 clipped and 0 non-finite samples, peaks −0.87 to
  −2.97 dBFS, audio tempo within 0.1 BPM of the score; seed 99 triumph
  renders byte-identical twice; a calm → crisis switch at 40 s wraps up
  through the outro at 85.4 s. The final chord now holds for its whole bar
  (−19 dBFS) and decays over ~1.5 s after the song's end, where it used to
  stop half a beat early.

### Verification of the crescendo devices and wartime

Hand-overs over 200 scores (4 moods × seeds 1–50), previous arrangement →
now: long risers 29 → 7 songs, one-bar sweeps 51 → 7; snare rolls 102 →
41 + 43 clap rolls; new devices: arp-climb 72, hat-ramp 71, layer-in 43,
pad-swell 29, kick-double 28, tom-cascade 27, stab-ramp 26, lead-climb 21;
the others: downlifter 192, cut 143, fill 123, reverse 117 (was 236),
filter-close 117, toms 78, kick-out 56, filter-open 41, stop 35,
drum-break 22, kick-return 19. No song repeats a hand-over back to back.
Lead and phrase statistics are unchanged (lowest note 60, 86 % above the
pads, 89 % of phrase ends on chord tones, 200/200 held final tonics).

Wartime, 50 scores: 137–159 s, 132–140 BPM, all full forms, 50/50 held
final tonics, 89 % of phrase ends on chord tones, 1 long riser. Renders of
seeds 1, 2, 3, 7, 42: 0 clipped / non-finite samples, peaks −0.95 to
−0.99 dBFS (limiter), audio tempo within 0.1 BPM; seed 5 renders
byte-identical twice; a calm song switched to wartime at 40 s jumps to its
outro at the next 8-bar line and ends at 68.4 s, then a 138 BPM wartime
song plays (estimated from audio: 137.9). In the client the switch comes
from the sim's `at_war` state; scenario 2020 has no war within 1 200 days
(headless run), so it was checked through the renderer, not a live war.

### Verification of the style rework (250 scores, 70 renders)

These numbers answer the feedback on wartime not being hardtrance enough,
claps rolling absurdly fast, leads too high, songs sharing one rhythm,
short notes and mundane parts. Score statistics are over 5 moods × seeds
1–50, from an analysis tool over `compose()` that is not part of the
build. Each figure is the previous composer → now.

- **Rolls.** The previous composer had 200 snare / clap rolls faster than
  16ths (runs of 3+ onsets under a 16th apart) away from a build's last
  beat, the longest lasting 123 ticks (5 beats of 32nds; 10 beats in
  wartime). Now there are 0. The 67 remaining 32nd runs each last one beat
  (21 ticks) and end a build.
  - Seed 31 calm, the reported case: its old roll ran 32nds for 1⅓ bars
    at 118 BPM. Its new song (lo-fi, 83 BPM) has 4 bars of quarters → 8ths
    → 16ths with one beat of 32nds.
  - Snare-solo renders (35 old, 35 new): onsets less than ¾ of a 16th
    apart, 22.3 per song (max 71) → 3.1 (max 24; most are clap and snare
    layered on one tick). The most onsets in any one beat: 9 → 4–5 in
    tense, crisis and wartime, 8 (the final beat) in calm and triumph.
- **Lead register.** Song-median lead note in the low (< C4) / mid /
  high (≥ C5) bins: 0 / 18 / 232 → 89 / 124 / 37 (wartime 49 / 1 / 0).
  Lead-solo renders of 7 seeds per mood, strongest partial (MIDI, mean of
  the song medians): calm 76.9 → 66.4, tense 77.2 → 66.9, crisis 80.7 →
  66.9, triumph 81.3 → 70.2, wartime 79.2 → 54.7. Spectral centroid:
  wartime 2754 → 724 Hz, the others 1816–2779 → 1220–1952 Hz.
- **Note lengths.** Mean lead note 4.9 / 4.6 / 3.4 / 4.5 / 4.2 rows → 6.4
  / 6.4 / 6.0 / 6.5 / 5.9 (calm / tense / crisis / triumph / wartime).
  Notes of 8+ rows: 7–15 % → 25–35 %. The longest arp run per song: about
  20 bars → 34–42 bars outside calm (calm 21).
- **Diversity.** Five style families per mood outside wartime (for example
  crisis: 8 punk, 15 breaks, 6 chip, 13 synthwave, 8 techno). Distinct
  verse + chorus skeletons (kick / backbeat / bass masks): 29 / 19 / 15 /
  18 / 14 per 50 songs → 46 / 46 / 48 / 42 / 19. In style + groove + bass
  terms, 42–46 of 50 songs per mood differ (wartime 16; it is always
  hardtrance). Moving bass, distinct pitches per bar: 1.35–2.27 → 1.32–2.51.
- **Active parts.** Counter-voice notes per song: 0 → 64–71 outside calm
  (calm 9, sparse on purpose). Key changes: 0 → 16 / 18 / 24 / 19 of 50
  (tense / crisis / triumph / wartime; calm none).
- **Length.** 28 intermissions (30–80 s), 192 songs of 2–3.5 min, and 30
  long songs of 5–10 min (max 584 s), 15 of them in the epic form.
  - Epic wartime seed 8 (348 s, 138 BPM), RMS per bar: the quiet intro
    −27 dBFS, the build and mains −11 to −13, breakdowns −16 to −18, the
    groove section −14, the mid-main stop bar −17, the pause bar −30.
- **Full-mix renders** (10 songs over all moods and 6 families, plus the
  epic one): 0 clipped and 0 non-finite samples, peaks −0.86 to −2.45
  dBFS, RMS −12.6 (wartime) to −20.0 dBFS (lo-fi).
  - The audio tempo estimate now searches 75–180 BPM and matches the
    score within 0.1 BPM. The exception is punk at 166, which reads as 83
    (half time: its kick and snare alternate on the beats).
  - Seed 5 wartime renders byte-identical twice.
  - `test_music` passes.

### Verification of the sound rework (late-90s / early-2000s synth)

This answers the feedback that songs were "too 8-bit" and some instruments
too sharp. Each figure is the previous build (7aa4c09) → now.

- **Aliasing**, from a probe that holds one note through each song's
  patch: vibrato off, dry, measured 0.5 s in. The figure is the energy
  folded below the fundamental (minus the sub-octave) relative to the
  total, at C7 over seeds 1–8 of calm, tense and triumph. Worst / mean:
  - lead −21.7 / −54.4 → −42.8 / −62.9 dB;
  - arp −22.3 / −70.0 → −84.1 / −87.6 dB;
  - pad −36.1 / −83.1 → −81.0 / −81.3 dB;
  - bass about −61 dB, unchanged.

  The old worst cases were the bit-crushed 8-bit wavetable lead and arp.
  At C5 the wartime lead measures −51.2 → −45.9 dB mean. That residue is
  intermodulation from saturating the detuned reese saws, its intended
  growl: with drive off the same patches measure about −85 dB.
- **Energy above 6 kHz** on held C7 notes: lead 4.56 → 0.58 %, arp 3.70 →
  0.01 %, pad 0.35 → 0.00 %.
- **Per-voice solo renders** (5 moods × seeds 1–3; the songs differ in
  detail, because the draw order changed). Mean spectral centroid:
  - lead 1467 → 883 Hz;
  - arp 2251 → 1515 Hz;
  - pad 1038 → 735 Hz;
  - counter 764 → 545 Hz;
  - bass 316 → 279 Hz;
  - hats 13.9 → 9.6 kHz.

  Energy above 6 kHz: lead 0.07 → 0.00 %, arp 0.48 → 0.10 %, hats
  95.4 → 88.8 %. The snare is unchanged (8.3–8.6 kHz).
- **Levels.** On a held A4 the patches stay within 1 dB: lead −10.4 →
  −10.4, pad −14.8 → −15.0, bass −15.3 → −14.8, counter −11.4 → −11.4,
  arp −13.6 → −12.6 dB.
- **Full mixes** (8 songs, two of them long wartime songs): peaks −0.87 to
  −1.77 dBFS, 0 clipped and 0 non-finite samples, RMS −12.7 (wartime) to
  −20.3 dBFS (lo-fi). Seed 5 wartime renders byte-identical twice.
- **Tests.** `test_music` also checks that no voice is bit-crushed or uses
  the 8-bit wavetable, that every melodic voice has a 24 dB filter, that
  wartime is always hardtrance, and that the chip family stays rare (at
  most 1 song in 10 over 200 seeds).

## Limitations and next steps

- The loudness range is about 4 LU against the reference's 9. Breakdowns
  could drop further, and the intro and outro could fade more.
- 8 form templates (+3 intermissions, the epic long form) and 2 motifs per
  song. Key changes happen only once, for the last chorus. Chord colours
  are realised by snapping, so a melody held over a colour change is
  re-sung at the bar line, not voice-led.
- Wartime songs share one skeleton family (hardtrance, 3 bass patterns).
  The variety there comes from the bass, the forms, the lead patch, the
  key and the progressions.
- Pads are paraphonic (one filter per chord) and there is no per-note
  stereo spread. The reverb is a basic Schroeder design.
- The mood comes only from HUD tones and alerts. Explicit inputs such as
  unrest, a war's state or an election night would be better once the HUD
  shows them.
- There is no fade-out on quit, no settings UI (only `M` and command-line
  options) and no sound effects bus yet.
