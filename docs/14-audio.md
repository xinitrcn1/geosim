# 14 — Audio: procedural dynamic music

All music is generated in-game. No recorded songs ship. A seeded **composer**
writes a tracker-style score of 2–3 minutes (about one song in four is a
30–80 s intermission). A small **synthesizer** plays it
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
echo is what the ping-pong delay already does. Changed for this game: the tempo is
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

1. **Basics.** BPM is fixed for the song: calm 112–118, tense 116–122,
   triumph 120–126, crisis 122–128. The tonic is A2–G#3. The mode depends on
   the mood (calm: ionian / lydian / mixolydian / dorian; tense: dorian /
   aeolian; crisis: aeolian / phrygian / harmonic minor; triumph: ionian /
   mixolydian / lydian). There are three diatonic 4-chord progressions (A,
   B, bridge), taken from a table of progressions that only use major or
   minor triads of the mode (e.g. I–V–vi–IV, i–VI–III–VII, i–i–VII–VI,
   i–bII–i–bVII). Harmonic rhythm is 1 or 2 bars per chord. Crisis BPM is
   clamped to 128 (rounding could give 129). Swing is 0 or 1
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
5. **Placement on the harmony.** A motif is placed as 2-bar **cells**. A cell
   is transposed diatonically with the chord root (wrapped to ±3 steps), plus
   an optional melodic **sequence** step, and may be transformed:
   *invert* (contour mirrored around its first note), *retro-tail* (bar 2
   backwards) or *cadence* (bar 1 of the motif, then a long resolving note on
   beat 1 or 2 of bar 2, held to the cell's end). A transformed cell is moved
   by an octave if it drifts more than half an octave from the plain one.
   Strong-beat notes snap to the actual chord. Endings:
   - **open** (antecedent): a non-root chord tone, not the note just sung,
     held up to half a bar;
   - **closed** (consequent): the most stable chord tone near the last note
     — the tonic, else the key's fifth, else its third (the tonic weighs
     more at a section's end) — held to the end of the cell.
   A cell cut by the section's end gives its ending to its last placed note.
   In augmented cells (breakdown) a repeated pitch moves to a neighbour or to
   another chord tone.
   **Register:** pads voice within 17 semitones from `key + 7` (or `key − 5`
   for high keys, so the floor is D3–C#4). The lead's window starts a fifth
   above the pad floor and never below D4 (`lead_lo` 62–68, 19 semitones
   wide). Each cell takes the octave that keeps its notes in the window and
   joins the previous note smoothly; a stray note folds in by an octave. The
   octave doubling of the high-intensity layer is dropped above C7.
   **Phrase plans** (4 cells = 8 bars): statement, sequence (open),
   restatement — or, in a repeated phrase, a development (inversion or
   retro-tail with a sequence step) — then the cadence. Verses alternate
   lead cells and arp answers (open, closed, sequence, cadence); bridges
   answer the arp with sequences and an inversion; the breakdown sings the
   theme of the harmony it borrows, augmented, then its sequence a third
   higher, resolving. The last cell of every section is a cadence. If a
   section's last chord shares no tone with the tonic triad (II in lydian,
   bVII, bII, ii...), that last bar becomes the tonic: a modal cadence.
6. **Form.** The song uses one of 4 templates, each a sequence of 8- and
   16-bar sections, for example intro 8 → verse 16 → chorus 16 → bridge 8 →
   breakdown 8 → drop 16 → outro 8. Sections are shortened or added until
   the song lasts 2–3 minutes at its BPM. About one song in four (its own
   generator, so other seeds keep their form) is an **intermission** instead:
   intro 4 → verse 8 → outro 4 (30–34 s), intro 4 → verse 8 → chorus 8 →
   outro 4, or intro 8 → chorus 8 → breakdown 8 → drop 8 → outro 4
   (≤ 77 s). Per section:
   - **intro:** kick (from the middle when calm), hats, sustained pad, arp
     layer. In ~30 % of songs it is beat-first instead: kick, hats and bass
     from bar 1, the chords from halfway.
   - **verse:** call and response. The lead plays motif A in bars 1–2, and
     the arpeggiator answers in bars 3–4. A repeated verse adds open hats
     and stabs.
   - **chorus / drop:** an 8-bar melody (see the phrase plans), with a
     background arp layer, stabs (punchy `stab` chords on the saw pad), a
     crash on the downbeat and often an impact on the drop. In the drop the
     second 8 bars go an octave up when that stays below C7. In some songs
     the drop is **hardtrance**: the bass switches to off-beat roots (octave
     on top at high intensity) and the quiet pad layer is trance-gated.
   - **bridge:** the roles reverse: the arp calls and the lead answers with
     a variation of motif A.
   - **breakdown:** no kick, hats or bass. Pads (2 bars per chord, gated in
     songs with a trance gate), a sub layer, and the augmented theme. How it
     builds back depends on its hand-over (below).
   - **outro:** drums and pad thin out. The lead states motif A once more;
     the last two bars are the **final cadence**: a cadence chord (V, or
     bVII in mixolydian / dorian / aeolian, bII in phrygian) with its root in
     the bass and the lead a step from the tonic, then the tonic chord, the
     tonic in the bass and the lead on the tonic (or the third) held for the
     whole bar, a kick and a crash. The player lets them ring out.
7. **Transitions** (`choose_arrangement`, own generator). Each section
   boundary gets one hand-over, weighted by context and never the same
   twice in a row:

   | Kind | What happens | Used for |
   |---|---|---|
   | cut | nothing but the crash on the next downbeat | anywhere |
   | fill | snare fill on the last 2 beats (the kick leaves beat 4) | anywhere |
   | toms | tom fill on the last beat | anywhere |
   | roll | snare roll: 8ths, then 16ths, the last beat in 32nds, crescendo (2 bars; 4 out of a breakdown) | into chorus / drop, out of a breakdown |
   | riser | noise riser, 2 bars (4 out of a breakdown, with a roll) | at most once per song, in ~40 % of songs |
   | sweep | one-bar pitched sweep up | at most once per song |
   | filter-close | the bus low-pass closes over the last 2 bars | into a breakdown or outro |
   | filter-open | the section starts dark and the low-pass opens across it | intro, breakdown |
   | kick-out | the kick (and the bass in long sections) drops out for the last bar, with a short snare pickup | into chorus / drop, verse, outro |
   | stop | everything but the lead (and fx tails) stops for the last 2 beats, or the whole last bar | anywhere |
   | kick-return | the breakdown's kick comes back for its last bars, with a roll | out of a breakdown |
   | drum-break | lead, arp and chords drop out for the last 4 bars (the melody ends with its cadence before); drums and bass only | 16-bar verse / chorus / drop |

   Extras: a reverse cymbal into the next downbeat, a downlifter (and a
   crash) at the start of a calmer section, an impact on the drop. The
   chosen hand-overs are listed at the end of the song's description.
8. **Voice leading.** Each pad chord takes the inversion and octave, within
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
- the background arps and the breakdown sub.

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
`GEOSIM_BUILD_TESTS`) checks determinism, 30 s – 3 min length (full songs
2–3 min), BPM range, the score's invariants, the final tonic chord and the
lead's held final tonic, and 40 s of finite output below 0 dBFS at full
intensity, for every mood and seven seeds (including two intermissions).

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

## Limitations and next steps

- The loudness range is about 4 LU against the reference's 9. Breakdowns
  could drop further, and the intro and outro could fade more.
- Only 4 form templates (+3 intermissions) and 2 motifs per song; cells are
  developed (sequence, inversion, retro-tail, cadence) but there are no
  per-section key changes yet.
- Pads are paraphonic (one filter per chord) and there is no per-note
  stereo spread. The reverb is a basic Schroeder design.
- The mood comes only from HUD tones and alerts. Explicit inputs such as
  unrest, a war's state or an election night would be better once the HUD
  shows them.
- There is no fade-out on quit, no settings UI (only `M` and command-line
  options) and no sound effects bus yet.
