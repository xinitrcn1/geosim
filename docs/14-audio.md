# 14 — Audio: procedural dynamic music

All music is generated in-game. No recorded songs ship. A seeded **composer**
writes a tracker-style score of 2–3 minutes. A small **synthesizer** plays it
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
Pro module. They are used for analysis only. They are not in the repository,
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

Adopted: 4/4 on a 16th-note grid with tracker ticks, four-on-the-floor,
off-beat hats and bass, the stab rhythm, the i–i–VII–VI cycle (one of the
progressions), 8-bar phrasing, breakdown → riser → drop, and a loudness
target near −18 LUFS in game. Changed for this game: the tempo is
**110–128 BPM** ("upbeat but not too upbeat", "informal" techno) instead of
146. Songs last 2–3 minutes instead of 16, and use synthesised "sample-like"
waveforms instead of samples.

## Synthesis

The tracker model has 8 channels: **kick, snare/clap, hats, bass, lead, arp,
pad, fx**. Each channel has one patch and a pool of 6 voices.

- **Tonal voices:** two oscillators (PolyBLEP saw / pulse with PWM, triangle,
  sine, noise, or a 32-step "sample-like" 8-bit wavetable: organ, bass, vox,
  6-bit pluck), plus a sine sub-octave, optional saturation, and optional
  **Paula-style sample-and-hold + 8-bit quantisation** (16.7 / 20 kHz).
  After that comes a TPT state-variable low-pass with resonance, its own
  envelope, key tracking and velocity, then a linear-attack / exponential
  ADSR. Lead voices add delayed vibrato and optional glide.
- **Chords** are paraphonic: one voice, up to 4 note oscillators into one
  filter. With `note_fx::arp` they play as a **tracker arpeggio**, cycling
  the chord notes every tick (~48 Hz at 120 BPM), the classic chip chord.
- **Mono channels** behave like tracker channels: a new note cuts the
  previous one with a 6 ms fade (no clicks), or with the patch release on
  the pad. Kit channels choke earlier hits (a closed hat cuts the open hat)
  but layer hits on the same tick.
- **Drum models** (selected by GM-like note numbers on kit channels):
  - kick: sine with exponential pitch drop, noise click and tanh;
  - snare: two-sine body and high-passed noise;
  - clap: 3 band-passed noise bursts and a tail;
  - closed/open hat and crash: six TR-808 square oscillators and noise,
    high-passed;
  - rim, shaker and 3 toms;
  - riser: noise band-pass swept up 5 octaves, plus a rising tone;
  - swell, and impact (low boom and noise burst).

## Mixer

Each channel is a mono sum scaled by its gain, then:

- **Sidechain ducking:** a 4 ms dip, recovering over ~90 ms after every
  kick. It is per channel (pad 0.6, bass 0.55, arp 0.3) and gives the
  "pumping" groove.
- **Constant-power pan:** arp and lead are panned to opposite sides, in an
  Amiga spirit but softened.
- **Sends:** a tempo-synced **ping-pong delay** (dotted 8th or 8th, feedback
  0.3–0.45, damped) and a Freeverb-style **reverb** (4 combs + 2 allpasses
  per side).
- **Master:** DC blocker → volume (smoothed) → **look-ahead peak limiter**
  (64 samples, ceiling −1 dBFS, ~50 ms release) → soft clip above 0.9,
  bounded below 1.0. Non-finite samples are replaced by silence before the
  limiter.

Levels were set from solo renders. For seed 7, tense, the channel RMS
values are: kick −20.6, bass −22.4, lead −27.9, pad −29.1, snare −35.0,
arp −37.1, hats −38.8, fx −39.5 dBFS.

## Composer

`compose(seed, mood)` is deterministic: it uses its own splitmix64 PRNG and
no `std::` distributions, so the same seed and mood give a byte-identical
WAV.

1. **Basics.** BPM is fixed for the song: calm 112–118, tense 116–122,
   triumph 120–126, crisis 122–128. The tonic is A2–G#3. The mode depends on
   the mood (calm: ionian / lydian / mixolydian / dorian; tense: dorian /
   aeolian; crisis: aeolian / phrygian / harmonic minor; triumph: ionian /
   mixolydian / lydian). There are three diatonic 4-chord progressions (A,
   B, bridge), taken from a table of progressions that only use major or
   minor triads of the mode (e.g. I–V–vi–IV, i–VI–III–VII, i–i–VII–VI,
   i–bII–i–bVII). Harmonic rhythm is 1 or 2 bars per chord. Swing is 0 or 1
   tick (≈ 58 %) on odd 16ths and is likelier when calm. Calm and triumph
   songs often use 7th chords.
2. **Sounds.** A patch is picked per song, per mood, from a few designs per
   role. Bass: triangle + sub, chip pulse, 8-bit "sample", resonant saw.
   Lead: PWM pulse, detuned saws, chip sample, square + fifth. Arp: plucks.
   Pad: a detuned saw pad, or a chip arpeggio chord.
3. **Groove** (fixed for the song, so it repeats like a tracker pattern):
   - kick: four-on-the-floor, or a broken pattern when calm, with optional
     phrase-end pickups;
   - backbeat: clap or snare on 2 and 4, plus a ghost-note set;
   - hats: closed on 8ths or quarters, open on the off-beats, the remaining
     16ths as an intensity layer;
   - bass rhythm: off-beat (reference), rolling 16ths around the kick, a
     generated house pattern with note roles (root / octave / fifth /
     below), or octave 8ths;
   - arpeggio order (8 shapes over root, 3rd, 5th, octave, 10th);
   - stab rhythm (5 masks, one of them the reference's).
4. **Motifs.** There are two 2-bar motifs: A for verses and B for choruses
   (B is denser). Rhythm comes from weighted sampling of the 32 rows
   (downbeats 5, beats 3, 8ths 2.2, 16ths 0.6 × density). Half the time bar
   2 echoes bar 1's rhythm, and the last 4 rows stay silent so the answer
   has room. Pitch is a contour of steps (55 %), skips, repeats and rare
   leaps, with leap recovery, inside a 1.5-octave range. Strong beats
   always land on chord tones.
5. **Placement on the harmony.** A motif is transposed diatonically with the
   chord root, wrapped to ±3 steps so it stays in register, and its
   strong-beat notes are re-snapped to the actual chord. Phrase endings are
   explicit. An **open** (antecedent) ending is a non-root chord tone. A
   **closed** (consequent) ending is the tonic, or the chord tone nearest
   it. Variation drops an inner note of the answer.
6. **Form.** The song uses one of 4 templates, each a sequence of 8- and
   16-bar sections, for example intro 8 → verse 16 → chorus 16 → bridge 8 →
   breakdown 8 → drop 16 → outro 8. Sections are shortened or added until
   the song lasts 2–3 minutes at its BPM. Per section:
   - **intro:** kick (from the middle when calm), hats, sustained pad, arp
     layer; a swell into the verse.
   - **verse:** call and response. The lead plays motif A in bars 1–2, and
     the arpeggiator answers in bars 3–4. A repeated verse adds open hats
     and stabs.
   - **chorus / drop:** an 8-bar melody (motif B, open answer, motif B,
     closed answer), with a background arp layer, stabs, a crash on the
     downbeat and an impact on the drop. In the drop the second 8 bars go
     an octave up.
   - **bridge:** the roles reverse: the arp calls and the lead answers with
     a variation of motif A.
   - **breakdown:** no kick, hats or bass. Pads (2 bars per chord), a sub
     layer, and motif A in augmentation (double length). A riser over the
     last 4 bars, a snare roll (8ths, then 16ths), and one empty beat
     before the drop.
   - **outro:** drums and pad thin out. The last bar is a kick, the tonic
     chord and a crash, and the effect tails ring out.
   - **transitions:** the last bar of a section has a snare or tom fill
     (the kick leaves room on beat 4 before a chorus or drop) and a
     2-bar riser layer into big sections.
7. **Voice leading.** Each pad chord takes the inversion and octave, within
   the pad register, closest to the previous voicing.

### Intensity layers

Events carry a layer tag: always, ≥ 0.4, ≥ 0.7, < 0.4 or < 0.7. The
complementary tags give alternatives rather than duplicates: for example
"root" below 0.7 and "root + octave" from 0.7 up. The player decides at
note-on from the live intensity, so layers come and go on note boundaries,
never mid-note. Layered parts:

- 16th hats, ghost snares, open-vs-closed hats in verses;
- a second backbeat sound, and rim/shaker 16ths at high intensity;
- the bass octave and the lead octave doubling at high intensity;
- the background arps and the breakdown sub;
- the riser into choruses.

Intensity also sets the brightness of lead, arp and pad: their cutoff is
multiplied by 2^((i − 0.55) · 1.6). Intensity is smoothed with a ~6 s time
constant, which gives gentle cross-fades.

## Mood mapping

`app.cpp` (`derive_music_mood`) reads only what the HUD already shows, every
2 s:

- `stress` = share of indicators coloured *bad* + 0.15 × bad alerts of the
  last 120 days (max 4) + 0.4 if a war alert involves the player;
- **crisis** if there is a war or stress ≥ 0.65; **tense** if ≥ 0.3;
  **triumph** if at least 40 % of indicators are *good* and none is bad, or
  after a peace with low stress; otherwise **calm**;
- intensity = 0.25 + 0.65 × stress (triumph: 0.55 + 0.3 × good share),
  +0.1 at speed ≥ 4, clamped to 0.15–1.

`music_system::set_mood(m, intensity)`:

- Intensity applies at once, smoothed in the player.
- A different mood held for 20 s makes the jukebox **wrap the song up**: at
  the next 8-bar line (not in the intro or outro) the player jumps to the
  outro.
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

Offline, a full song renders in about 2.4 s, roughly 65× real time on this
machine, so live playback costs about 1.5 % of one core.

**No device:** if `SDL_InitSubSystem(SDL_INIT_AUDIO)` or opening the device
fails, the client logs `music: ... continuing without music` and runs
normally. Both cases were checked: `SDL_AUDIO_DRIVER=nonexistent`, and this
machine's missing ALSA device. `SDL_AUDIO_DRIVER=dummy` exercises the full
path without sound.

Keys and options: **M** mutes / unmutes; `--music 0`,
`--music-volume 0..1` (default 0.6), `--music-seed N` (default random).

## Offline rendering

```
geosim --render-music out.wav [--seed N] [--mood calm|tense|crisis|triumph]
       [--intensity 0..1] [--songs N] [--switch SECONDS:MOOD] [--solo CHANNEL]
```

This renders through the same jukebox and player, with no window and no
audio device, to 16-bit stereo 48 kHz. The report gives:

- duration, peak, RMS, and non-finite and clipped sample counts (the exit
  code is non-zero if any sample is non-finite or clipped);
- per song: score and played length, BPM, and an **independent tempo
  estimate** (autocorrelation of the low-band onset envelope);
- the share of low-band onsets on the 16th grid.

`--solo kick` isolates the kick for an exact grid check. `--switch` tests a
mid-song mood change and the wrap-up. `tests/test_music.cpp` (built with
`GEOSIM_BUILD_TESTS`) checks determinism, 2–3 min length, BPM range, the
score's invariants, and 40 s of finite output below 0 dBFS at full
intensity, for every mood and three seeds.

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

## Limitations and next steps

- The loudness range is about 4 LU against the reference's 9. Breakdowns
  could drop further, and the intro and outro could fade more.
- Only 4 form templates and 2 motifs per song. A motif bank (inversion,
  retrograde, sequence) and per-section key changes would add variety.
- Pads are paraphonic (one filter per chord) and there is no per-note
  stereo spread. The reverb is a basic Schroeder design.
- The mood comes only from HUD tones and alerts. Explicit inputs such as
  unrest, a war's state or an election night would be better once the HUD
  shows them.
- There is no fade-out on quit, no settings UI (only `M` and command-line
  options) and no sound effects bus yet.
