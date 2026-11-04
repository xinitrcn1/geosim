// Song player: sequencer, voices, mixer, effects, limiter (synth.hpp).
// Everything in render() is allocation- and lock-free.

#include "synth.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::client::audio {

namespace {

constexpr float pi = 3.14159265358979f;
constexpr float two_pi = 6.28318530717959f;

float midi_hz(float note) { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }

// PolyBLEP residual for band-limited saw / pulse edges.
float blep(float t, float dt) {
    if(t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if(t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

float white(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float>(static_cast<int32_t>(s)) * (1.0f / 2147483648.0f);
}

// Topology-preserving state-variable filter (Zavalishin). Returns low-pass;
// band/high-pass through out-params.
struct svf_out {
    float lp, bp, hp;
};
svf_out svf(float x, float g, float k, float& ic1, float& ic2) {
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1, a3 = g * a2;
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    return {v2, v1, x - k * v1 - v2};
}

float svf_g(float hz, float sr) { return std::tan(pi * std::clamp(hz, 20.0f, sr * 0.45f) / sr); }

float quantize8(float x) { return std::round(std::clamp(x, -1.0f, 1.0f) * 127.0f) / 127.0f; }

// TR-808 hat oscillator frequencies.
constexpr float metal_hz[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};

} // namespace

player::player(float sample_rate) : sr_(sample_rate), inv_sr_(1.0f / sample_rate) {
    delay_l_.assign(static_cast<std::size_t>(sample_rate * 2.0f) + 1, 0.0f);
    delay_r_.assign(delay_l_.size(), 0.0f);
    // Freeverb tunings at 44.1 kHz, scaled; right channel slightly longer.
    static constexpr int comb_t[comb_count] = {1116, 1188, 1277, 1356};
    static constexpr int ap_t[allpass_count] = {556, 441};
    const float scale = std::min(sample_rate, 96000.0f) / 44100.0f;
    for(int side = 0; side < 2; ++side) {
        for(int i = 0; i < comb_count; ++i) combs_[static_cast<std::size_t>(side * comb_count + i)].len = std::min(comb_max, static_cast<int>(static_cast<float>(comb_t[i] + side * 23) * scale));
        for(int i = 0; i < allpass_count; ++i) allpasses_[static_cast<std::size_t>(side * allpass_count + i)].len = std::min(allpass_max, static_cast<int>(static_cast<float>(ap_t[i] + side * 23) * scale));
    }
    // "Sample-like" single-cycle waves, 8-bit like Amiga chip samples.
    for(int i = 0; i < wavetable_len; ++i) {
        const float x = two_pi * static_cast<float>(i) / wavetable_len;
        const float organ = std::sin(x) + 0.5f * std::sin(2 * x) + 0.25f * std::sin(4 * x);
        float bass = 0;
        for(int h = 1; h <= 6; ++h) bass += std::sin(static_cast<float>(h) * x) / static_cast<float>(h) * (h % 2 ? 1.0f : 0.6f);
        const float vox = std::sin(x) + 0.2f * std::sin(2 * x) + 0.6f * std::sin(3 * x) + 0.3f * std::sin(5 * x);
        const float pluck = (std::sin(x) >= 0 ? 0.6f : -0.6f) + 0.4f * std::sin(2 * x);
        tables_[0][i] = organ / 1.6f;
        tables_[1][i] = bass / 1.9f;
        tables_[2][i] = vox / 1.8f;
        tables_[3][i] = std::round(pluck * 31.0f) / 31.0f; // 6-bit
    }
    for(int t = 0; t < 3; ++t) {
        for(float& v : tables_[t]) v = quantize8(v);
    }
}

void player::set_song(song const* s) noexcept {
    // A song that played to its end lets its last chord and note ring out
    // (the final cadence decays instead of being cut by the patch release).
    const bool natural_end = song_ != nullptr && finished_ && s == nullptr;
    if(natural_end) {
        for(int c : {ch_bass, ch_lead, ch_pad}) release_channel(c, ~0u, false, 1.4f);
    }
    song_ = s;
    finished_ = false;
    sweep_dir_ = 0;
    next_event_ = 0;
    tick_ = 0;
    section_ = 0;
    outro_requested_ = false;
    to_next_tick_ = 0;
    if(!s) return;
    for(int c = 0; c < num_channels; ++c) {
        patches_[c] = s->patches[c];
        release_channel(c, ~0u, false);
    }
    mix_ = s->mix;
    for(int c = 0; c < num_channels; ++c) {
        const float a = (std::clamp(mix_.pan[c], -1.0f, 1.0f) + 1.0f) * pi * 0.25f;
        pan_l_[c] = std::cos(a);
        pan_r_[c] = std::sin(a);
    }
    samples_per_tick_ = s->seconds_per_tick() * static_cast<double>(sr_);
    delay_len_ = std::clamp<std::size_t>(static_cast<std::size_t>(static_cast<double>(mix_.delay_rows) * ticks_per_row * samples_per_tick_), 1, delay_l_.size() - 1);
    process_tick(); // events at tick 0
    to_next_tick_ = samples_per_tick_;
}

bool player::layer_on(uint8_t layer) const noexcept {
    switch(layer) {
    case layer_min_mid: return intensity_ >= layer_mid;
    case layer_min_high: return intensity_ >= layer_high;
    case layer_max_mid: return intensity_ < layer_mid;
    case layer_max_high: return intensity_ < layer_high;
    default: return true;
    }
}

void player::release_channel(int ch, uint32_t before_tick, bool fast, float min_release) noexcept {
    for(voice& v : voices_[static_cast<std::size_t>(ch)]) {
        if(!v.active || v.start_tick >= before_tick) continue;
        v.gate = false;
        if(v.stage != 2 || fast) {
            v.stage = 2;
            const float t = fast ? 0.006f : std::max({0.005f, patches_[ch].release, min_release});
            v.release_coef = std::exp(-4.6f * inv_sr_ / t);
        }
    }
}

player::voice& player::allocate(int ch) noexcept {
    auto& pool = voices_[static_cast<std::size_t>(ch)];
    voice* best = &pool[0];
    for(voice& v : pool) {
        if(!v.active) return v;
        if(v.age > best->age) best = &v;
    }
    return *best; // steal the oldest
}

void player::jump_to_outro() noexcept {
    outro_requested_ = false;
    if(!song_) return;
    for(std::size_t i = 0; i < song_->sections.size(); ++i) {
        if(song_->sections[i].kind != section_kind::outro) continue;
        if(static_cast<int>(i) <= section_) return; // already there
        tick_ = song_->sections[i].start_bar * ticks_per_bar;
        section_ = static_cast<int>(i);
        auto it = std::lower_bound(song_->events.begin(), song_->events.end(), tick_, [](note_event const& e, uint32_t t) { return e.tick < t; });
        next_event_ = static_cast<std::size_t>(it - song_->events.begin());
        for(int c = 0; c < num_channels; ++c) release_channel(c, ~0u, false);
        return;
    }
}

void player::process_tick() noexcept {
    if(!song_) return;
    if(tick_ % ticks_per_bar == 0) {
        const uint32_t bar = tick_ / ticks_per_bar;
        // track the section; honour a pending outro request at an 8-bar boundary
        while(section_ + 1 < static_cast<int>(song_->sections.size()) && song_->sections[static_cast<std::size_t>(section_ + 1)].start_bar <= bar) ++section_;
        if(outro_requested_ && bar % 8 == 0 && bar > 0) {
            const section_kind k = song_->sections[static_cast<std::size_t>(section_)].kind;
            if(k != section_kind::intro && k != section_kind::outro) jump_to_outro();
            else if(k == section_kind::outro) outro_requested_ = false;
        }
    }
    if(tick_ >= song_->total_ticks()) {
        finished_ = true;
        return;
    }
    auto const& ev = song_->events;
    while(next_event_ < ev.size() && ev[next_event_].tick <= tick_) {
        note_event const& e = ev[next_event_++];
        if(e.tick == tick_ && layer_on(e.layer)) trigger(e);
    }
    // tracker-style arpeggio: next chord note every tick
    for(auto& pool : voices_) {
        for(voice& v : pool) {
            if(v.active && v.fx == note_fx::arp && v.count > 1) v.arp_index = static_cast<uint8_t>((v.arp_index + 1) % v.count);
        }
    }
}

void player::trigger(note_event const& e) noexcept {
    patch const& p = patches_[e.ch];
    const float vel = std::pow(static_cast<float>(e.vel) / 127.0f, 1.4f);
    if(p.kit && (e.notes[0] == dr_lp_close || e.notes[0] == dr_lp_open)) {
        sweep_dir_ = e.notes[0] == dr_lp_close ? -1 : 1;
        sweep_t_ = 0.0f;
        sweep_len_ = std::max(0.05f, static_cast<float>(static_cast<double>(e.len) * samples_per_tick_) * inv_sr_);
        if(sweep_dir_ > 0) {
            for(int c = 0; c < num_channels; ++c) sweep_ic1_[c] = sweep_ic2_[c] = 0.0f;
        }
        return;
    }
    if(p.kit) {
        // Each note is its own drum voice; mono kits choke earlier hits
        // (closed hat cuts the open hat), hits on the same tick layer.
        if(p.mono) release_channel(e.ch, tick_, true);
        for(int i = 0; i < e.count; ++i) {
            voice& v = allocate(e.ch);
            v = voice{};
            v.active = v.gate = true;
            v.ch = e.ch;
            v.drum = e.notes[i];
            v.start_tick = tick_;
            v.vel = vel;
            v.len_s = static_cast<float>(static_cast<double>(e.len) * samples_per_tick_) * inv_sr_;
            v.gate_left = static_cast<double>(e.len) * samples_per_tick_;
            v.noise = 0x9E3779B9u ^ (tick_ * 2654435761u) ^ (static_cast<uint32_t>(e.notes[i]) << 8);
            if(v.drum == dr_kick) {
                duck_t_ = 0.0f;
                ++kicks_;
            }
            for(int m = 0; m < 6; ++m) v.metal[m] = static_cast<float>(m) * 0.17f;
            float hz = 1000.0f;
            switch(v.drum) {
            case dr_snare: hz = 1800.0f; break;
            case dr_clap: hz = 1150.0f; break;
            case dr_hat_closed:
            case dr_hat_open: hz = 7000.0f; break;
            case dr_crash: hz = 3500.0f; break;
            case dr_shaker: hz = 5500.0f; break;
            case dr_impact: hz = 900.0f; break;
            default: break;
            }
            v.g = svf_g(hz, sr_);
        }
        return;
    }
    // Tonal: one (paraphonic) voice per event. Mono channels cut the
    // previous note; with glide the new note slides from the old pitch.
    float from_ratio = 1.0f;
    if(p.mono) {
        voice const* prev = nullptr;
        for(voice const& v : voices_[e.ch]) {
            if(v.active && v.gate && v.start_tick < tick_ && (!prev || v.age < prev->age)) prev = &v;
        }
        if(prev && p.glide > 0.0f) from_ratio = prev->freq[0] * prev->freq_mul / std::max(1.0f, midi_hz(e.notes[0]));
        release_channel(e.ch, tick_, p.release < 0.05f || e.ch != ch_pad);
    }
    voice& v = allocate(e.ch);
    const uint32_t seed = v.noise;
    v = voice{};
    v.noise = seed * 1664525u + 1013904223u;
    v.active = v.gate = true;
    v.ch = e.ch;
    v.start_tick = tick_;
    v.fx = e.fx;
    v.count = std::max<uint8_t>(1, std::min<uint8_t>(e.count, 4));
    for(int i = 0; i < v.count; ++i) {
        v.freq[i] = midi_hz(e.notes[i]);
        v.phase1[i] = 0.0f;
        v.phase2[i] = 0.25f * static_cast<float>(i);
    }
    v.glide_from = from_ratio;
    v.vel = vel;
    v.attack = p.attack;
    v.sustain = p.sustain;
    float decay = p.decay;
    if(e.fx == note_fx::stab) {
        v.attack = 0.002f;
        v.sustain = 0.0f;
        decay = 0.16f;
    }
    v.gate_mask = e.gate;
    v.gate_amp = e.gate ? 0.0f : 1.0f;
    v.gate_left = static_cast<double>(e.len) * samples_per_tick_;
    v.duty = p.duty;
    v.dcoef = std::exp(-4.6f * inv_sr_ / std::max(0.005f, decay));
    v.fcoef = std::exp(-4.6f * inv_sr_ / std::max(0.005f, p.fdecay));
    v.ratio2 = std::exp2((p.semi2 + p.detune * 0.01f) / 12.0f);
    update_control(v);
}

// Control-rate update (every 16 samples): pitch modulation, pulse width,
// filter coefficients.
void player::update_control(voice& v) noexcept {
    patch const& p = patches_[v.ch];
    if(p.kit) return;
    float semis = 0.0f;
    if(p.vib_depth > 0.0f && v.age > p.vib_delay) {
        const float ramp = std::min(1.0f, (v.age - p.vib_delay) / 0.25f);
        semis += p.vib_depth * ramp * std::sin(two_pi * p.vib_rate * v.age);
    }
    float glide = 1.0f;
    if(v.glide_from != 1.0f && p.glide > 0.0f) {
        const float u = std::min(1.0f, v.age / p.glide);
        glide = std::exp2(std::log2(v.glide_from) * (1.0f - u));
        if(u >= 1.0f) v.glide_from = 1.0f;
    }
    v.freq_mul = std::exp2(semis / 12.0f) * glide;
    v.duty = std::clamp(p.duty + p.pwm_depth * std::sin(two_pi * p.pwm_rate * v.age), 0.05f, 0.95f);
    const float f0 = v.freq[0] * v.freq_mul;
    float cutoff = p.cutoff * std::exp2(p.fenv * v.fenv) * std::pow(f0 / 261.6f, p.key_track) * (0.6f + 0.4f * v.vel);
    if(mix_.bright[v.ch]) cutoff *= bright_mul_;
    v.g = svf_g(cutoff, sr_);
    v.k = 2.0f - 1.9f * std::clamp(p.resonance, 0.0f, 0.98f);
}

float player::render_tonal(voice& v, patch const& p) noexcept {
    float sum = 0.0f;
    const int first = v.fx == note_fx::arp ? v.arp_index : 0;
    const int last = v.fx == note_fx::arp ? v.arp_index + 1 : v.count;
    for(int i = first; i < last; ++i) {
        const float f1 = v.freq[i] * v.freq_mul;
        const float dt1 = f1 * inv_sr_;
        const float f2 = f1 * v.ratio2;
        const float dt2 = f2 * inv_sr_;
        auto osc = [&](wave w, float& ph, float dt, int table) {
            float y = 0.0f;
            switch(w) {
            case wave::saw: y = 2.0f * ph - 1.0f - blep(ph, dt); break;
            case wave::pulse: {
                y = ph < v.duty ? 1.0f : -1.0f;
                y += blep(ph, dt);
                float t2 = ph - v.duty;
                if(t2 < 0) t2 += 1.0f;
                y -= blep(t2, dt);
                break;
            }
            case wave::triangle: y = 4.0f * std::fabs(ph - 0.5f) - 1.0f; break;
            case wave::sine: y = std::sin(two_pi * ph); break;
            case wave::noise: y = white(v.noise); break;
            case wave::table: {
                const float pos = ph * wavetable_len;
                const int i0 = static_cast<int>(pos) & (wavetable_len - 1);
                const float fr = pos - std::floor(pos);
                y = tables_[table][i0] * (1.0f - fr) + tables_[table][(i0 + 1) & (wavetable_len - 1)] * fr;
                break;
            }
            }
            ph += dt;
            if(ph >= 1.0f) ph -= 1.0f;
            return y;
        };
        float s = osc(p.w1, v.phase1[i], dt1, p.table1);
        if(p.mix2 > 0.0f) s += p.mix2 * osc(p.w2, v.phase2[i], dt2, p.table2);
        if(p.sub > 0.0f) {
            s += p.sub * std::sin(two_pi * v.phase_sub[i]);
            v.phase_sub[i] += dt1 * 0.5f;
            if(v.phase_sub[i] >= 1.0f) v.phase_sub[i] -= 1.0f;
        }
        sum += s;
    }
    if(v.fx != note_fx::arp && v.count > 1) sum *= 1.0f / std::sqrt(static_cast<float>(v.count));
    sum *= 1.0f / (1.0f + p.mix2 + p.sub);
    if(p.drive > 0.0f) sum = std::tanh(sum * (1.0f + 3.0f * p.drive));
    if(p.crush_rate > 0.0f) { // Paula-style: sample-and-hold at a low rate, 8-bit
        v.crush_phase += p.crush_rate * inv_sr_;
        if(v.crush_phase >= 1.0f) {
            v.crush_phase -= 1.0f;
            v.crush_hold = quantize8(sum);
        }
        sum = v.crush_hold;
    }
    const float y = svf(sum, v.g, v.k, v.ic1, v.ic2).lp;

    // envelopes
    switch(v.stage) {
    case 0:
        v.env += inv_sr_ / std::max(0.001f, v.attack);
        if(v.env >= 1.0f) v.env = 1.0f, v.stage = 1;
        break;
    case 1: v.env = v.sustain + (v.env - v.sustain) * v.dcoef; break;
    default: v.env *= v.release_coef; break;
    }
    float gate_gain = 1.0f;
    if(v.gate_mask) {
        // trance gate: open for the first 70 % of each enabled 16th,
        // ~1.5 ms attack and ~5 ms release so the chops do not click
        const float rows = v.age / row_seconds_;
        const int row = static_cast<int>(rows) & 15;
        const bool open = ((v.gate_mask >> row) & 1u) && (rows - std::floor(rows)) < 0.7f;
        v.gate_amp += ((open ? 1.0f : 0.0f) - v.gate_amp) * (open ? 0.015f : 0.0045f);
        gate_gain = v.gate_amp;
    }
    v.fenv *= v.fcoef;
    if(v.gate) {
        v.gate_left -= 1.0;
        if(v.gate_left <= 0) {
            v.gate = false;
            v.stage = 2;
            v.release_coef = std::exp(-4.6f * inv_sr_ / std::max(0.005f, p.release));
        }
    }
    if(v.stage == 2 && v.env < 1e-4f) v.active = false;
    return y * v.env * v.vel * p.gain * gate_gain;
}

float player::render_drum(voice& v, patch const& p, bool control) noexcept {
    const float t = v.age;
    float y = 0.0f;
    bool done = false;
    auto ex = [](float t_, float decay) { return std::exp(-4.6f * t_ / decay); };
    switch(v.drum) {
    case dr_kick: {
        const float f = p.kick_end + (p.kick_pitch - p.kick_end) * std::exp(-t / 0.032f);
        v.dphase += f * inv_sr_;
        if(v.dphase >= 1.0f) v.dphase -= 1.0f;
        const float body = std::sin(two_pi * v.dphase) * ex(t, p.kick_decay);
        const float click = white(v.noise) * std::exp(-t / 0.0012f) * p.kick_click;
        y = std::tanh(1.6f * (body + 0.5f * click));
        done = t > p.kick_decay * 1.2f;
        break;
    }
    case dr_snare: {
        v.dphase += p.snare_tone * inv_sr_;
        if(v.dphase >= 1.0f) v.dphase -= 1.0f;
        const float tone = (std::sin(two_pi * v.dphase) * 0.6f + std::sin(two_pi * v.dphase * 1.62f) * 0.3f) * ex(t, 0.07f);
        const float n = svf(white(v.noise), v.g, 1.2f, v.ic1, v.ic2).hp * ex(t, p.snare_decay);
        y = 0.9f * (tone + 0.8f * n);
        done = t > p.snare_decay * 1.3f;
        break;
    }
    case dr_clap: {
        float env;
        if(t < 0.027f) env = std::exp(-std::fmod(t, 0.009f) / 0.0028f);
        else env = ex(t - 0.027f, 0.16f);
        y = 1.6f * svf(white(v.noise), v.g, 0.9f, v.ic1, v.ic2).bp * env;
        done = t > 0.22f;
        break;
    }
    case dr_hat_closed:
    case dr_hat_open:
    case dr_crash: {
        float metal = 0.0f;
        for(int m = 0; m < 6; ++m) {
            v.metal[m] += metal_hz[m] * p.hat_tone * (v.drum == dr_crash ? 1.45f : 1.0f) * inv_sr_;
            if(v.metal[m] >= 1.0f) v.metal[m] -= 1.0f;
            metal += v.metal[m] < 0.5f ? 1.0f : -1.0f;
        }
        const float src = metal * 0.12f + white(v.noise) * 0.5f;
        const float hp = svf(src, v.g, 1.0f, v.ic1, v.ic2).hp;
        const float decay = v.drum == dr_hat_closed ? p.hat_decay : v.drum == dr_hat_open ? 0.32f : 1.7f;
        const float attack = v.drum == dr_crash ? std::min(1.0f, t / 0.004f) : 1.0f;
        y = hp * ex(t, decay) * attack * (v.drum == dr_crash ? 0.8f : 1.0f);
        done = t > decay * 1.1f;
        break;
    }
    case dr_rim: {
        v.dphase += 1700.0f * inv_sr_;
        if(v.dphase >= 1.0f) v.dphase -= 1.0f;
        y = (std::sin(two_pi * v.dphase) * 0.7f + white(v.noise) * 0.3f) * ex(t, 0.025f);
        done = t > 0.03f;
        break;
    }
    case dr_shaker: {
        const float hp = svf(white(v.noise), v.g, 1.2f, v.ic1, v.ic2).hp;
        y = hp * std::min(1.0f, t / 0.006f) * ex(t, 0.06f);
        done = t > 0.07f;
        break;
    }
    case dr_tom_low:
    case dr_tom_mid:
    case dr_tom_high: {
        const float base = v.drum == dr_tom_low ? 95.0f : v.drum == dr_tom_mid ? 125.0f : 165.0f;
        const float f = base * (0.75f + 0.25f * std::exp(-t / 0.08f));
        v.dphase += f * inv_sr_;
        if(v.dphase >= 1.0f) v.dphase -= 1.0f;
        y = std::sin(two_pi * v.dphase) * ex(t, 0.35f) + white(v.noise) * 0.1f * ex(t, 0.02f);
        done = t > 0.4f;
        break;
    }
    case dr_downlifter: {
        // band-passed noise falling 4 octaves with a falling tone, fading out
        const float len = std::max(0.1f, v.len_s);
        const float u = std::min(1.0f, t / len);
        if(control) v.g = svf_g(6000.0f * std::exp2(-4.0f * u), sr_);
        float s = svf(white(v.noise), v.g, 0.7f, v.ic1, v.ic2).bp;
        v.dphase += (880.0f * std::exp2(-3.0f * u)) * inv_sr_;
        if(v.dphase >= 1.0f) v.dphase -= 1.0f;
        s += 0.12f * std::sin(two_pi * v.dphase) * (1.0f - u);
        const float amp = std::min(1.0f, t / 0.01f) * (1.0f - u) * (1.0f - u);
        y = s * amp * 1.1f;
        done = t > len;
        break;
    }
    case dr_riser:
    case dr_swell: {
        const float len = std::max(0.1f, v.len_s);
        const float u = std::min(1.0f, t / len);
        const bool riser = v.drum == dr_riser;
        const float centre = riser ? 300.0f * std::exp2(u * 5.0f) : 2500.0f + 4000.0f * u;
        const float n = white(v.noise);
        if(control) v.g = svf_g(centre, sr_);
        float s = riser ? svf(n, v.g, 0.6f, v.ic1, v.ic2).bp : svf(n, v.g, 1.0f, v.ic1, v.ic2).hp;
        if(riser) { // plus a rising tone
            v.dphase += (110.0f * std::exp2(u * 3.0f)) * inv_sr_;
            if(v.dphase >= 1.0f) v.dphase -= 1.0f;
            s += 0.15f * std::sin(two_pi * v.dphase) * u;
        }
        const float amp = riser ? u * u : u * u * u;
        const float tail = t > len ? ex(t - len, 0.03f) : 1.0f;
        y = s * amp * tail * (riser ? 1.2f : 0.9f);
        done = t > len + 0.05f;
        break;
    }
    case dr_impact: {
        const float f = 32.0f + 30.0f * std::exp(-t / 0.15f);
        v.dphase += f * inv_sr_;
        if(v.dphase >= 1.0f) v.dphase -= 1.0f;
        const float boom = std::sin(two_pi * v.dphase) * ex(t, 1.4f);
        const float n = svf(white(v.noise), v.g, 1.0f, v.ic1, v.ic2).lp * ex(t, 0.25f);
        y = 0.8f * boom + 0.6f * n;
        done = t > 1.5f;
        break;
    }
    default: done = true; break;
    }
    // A choked drum fades over a few ms instead of clicking off.
    if(v.stage == 2) {
        v.env = v.env <= 0.0f ? 1.0f : v.env;
        v.env *= v.release_coef;
        y *= v.env;
        if(v.env < 1e-4f) done = true;
    }
    if(done) v.active = false;
    return y * v.vel * p.gain;
}

void player::render(float* out, int frames) noexcept {
    // block-rate smoothing of intensity and volume
    const float block_s = static_cast<float>(frames) * inv_sr_;
    intensity_ += (intensity_target_ - intensity_) * (1.0f - std::exp(-block_s / 6.0f));
    volume_ += (volume_target_ - volume_) * (1.0f - std::exp(-block_s / 0.15f));
    bright_mul_ = std::exp2((intensity_ - 0.55f) * 1.6f);
    row_seconds_ = static_cast<float>(samples_per_tick_ * ticks_per_row) * inv_sr_;

    const float fb = mix_.delay_feedback;
    const float room = 0.7f + 0.28f * std::clamp(mix_.reverb_size, 0.0f, 1.0f);
    const float damp = 0.3f, dly_damp = 0.35f;

    for(int f = 0; f < frames; ++f) {
        if(song_) {
            to_next_tick_ -= 1.0;
            if(to_next_tick_ <= 0) {
                to_next_tick_ += samples_per_tick_;
                ++tick_;
                process_tick();
            }
        }
        const bool control = (control_counter_++ & 15) == 0;
        if(sweep_dir_ != 0) {
            // closing: 16 kHz -> 280 Hz, then open again at the end; opening:
            // 280 Hz -> 16 kHz (exponential, slow at first), then bypassed
            sweep_t_ += inv_sr_;
            const float u = std::min(1.0f, sweep_t_ / sweep_len_);
            if(control) {
                const float x = sweep_dir_ < 0 ? u * u * (3.0f - 2.0f * u) : 1.0f - u * u;
                sweep_g_ = svf_g(16000.0f * std::exp2(-5.84f * x), sr_);
            }
            if(u >= 1.0f) sweep_dir_ = 0;
        }
        float l = 0, r = 0, dsend = 0, rsend = 0;
        // sidechain: a short dip after each kick
        const float duck_env = duck_t_ < 0.004f ? duck_t_ / 0.004f : std::exp(-(duck_t_ - 0.004f) / 0.09f);
        duck_t_ += inv_sr_;
        for(int c = 0; c < num_channels; ++c) {
            patch const& p = patches_[c];
            float s = 0.0f;
            for(voice& v : voices_[static_cast<std::size_t>(c)]) {
                if(!v.active) continue;
                if(p.kit) s += render_drum(v, p, control || v.age == 0.0f);
                else {
                    if(control) {
                        update_control(v);
                    }
                    s += render_tonal(v, p);
                }
                v.age += inv_sr_;
            }
            if(sweep_dir_ != 0 && c != ch_kick && c != ch_fx) s = svf(s, sweep_g_, sweep_k_, sweep_ic1_[c], sweep_ic2_[c]).lp;
            if(s == 0.0f) continue;
            s *= mix_.gain[c] * (1.0f - mix_.duck[c] * duck_env);
            l += s * pan_l_[c];
            r += s * pan_r_[c];
            dsend += s * mix_.delay[c];
            rsend += s * mix_.reverb[c];
        }

        // ping-pong delay with a damped feedback path
        {
            const std::size_t n = delay_l_.size();
            const std::size_t rp = (delay_pos_ + n - delay_len_) % n;
            const float dl = delay_l_[rp], dr = delay_r_[rp];
            delay_lp_l_ += (dl - delay_lp_l_) * (1.0f - dly_damp);
            delay_lp_r_ += (dr - delay_lp_r_) * (1.0f - dly_damp);
            delay_l_[delay_pos_] = dsend + fb * delay_lp_r_;
            delay_r_[delay_pos_] = fb * delay_lp_l_;
            delay_pos_ = (delay_pos_ + 1) % n;
            l += dl * 0.8f;
            r += dr * 0.8f;
        }
        // Schroeder/Freeverb-style reverb
        {
            const float in = rsend * 0.06f;
            float outs[2] = {0, 0};
            for(int side = 0; side < 2; ++side) {
                float acc = 0;
                for(int i = 0; i < comb_count; ++i) {
                    comb& cb = combs_[static_cast<std::size_t>(side * comb_count + i)];
                    const float o = cb.buf[static_cast<std::size_t>(cb.pos)];
                    cb.store = o * (1.0f - damp) + cb.store * damp;
                    cb.buf[static_cast<std::size_t>(cb.pos)] = in + cb.store * room;
                    if(++cb.pos >= cb.len) cb.pos = 0;
                    acc += o;
                }
                for(int i = 0; i < allpass_count; ++i) {
                    allpass& ap = allpasses_[static_cast<std::size_t>(side * allpass_count + i)];
                    const float b = ap.buf[static_cast<std::size_t>(ap.pos)];
                    ap.buf[static_cast<std::size_t>(ap.pos)] = acc + b * 0.5f;
                    acc = b - acc;
                    if(++ap.pos >= ap.len) ap.pos = 0;
                }
                outs[side] = acc;
            }
            l += outs[0];
            r += outs[1];
        }

        // DC blocker, master volume
        dc_y_l_ = l - dc_x_l_ + 0.9995f * dc_y_l_;
        dc_x_l_ = l;
        dc_y_r_ = r - dc_x_r_ + 0.9995f * dc_y_r_;
        dc_x_r_ = r;
        l = dc_y_l_ * volume_;
        r = dc_y_r_ * volume_;
        if(!std::isfinite(l) || !std::isfinite(r)) l = r = 0.0f; // never propagate NaN/inf to the device

        // look-ahead peak limiter, ceiling -1 dBFS
        constexpr float ceiling = 0.891f;
        const float peak = std::max(std::fabs(l), std::fabs(r));
        const float req = peak > ceiling ? ceiling / peak : 1.0f;
        if(req <= lim_hold_) {
            lim_hold_ = req;
            lim_hold_count_ = lookahead;
        } else if(lim_hold_count_ > 0) {
            --lim_hold_count_;
        } else {
            lim_hold_ += (1.0f - lim_hold_) * 0.0004f; // ~50 ms release
            lim_hold_ = std::min(lim_hold_, 1.0f);
        }
        if(lim_hold_ < lim_gain_) lim_gain_ += (lim_hold_ - lim_gain_) * (3.0f / lookahead);
        else lim_gain_ = lim_hold_;
        const std::size_t li = static_cast<std::size_t>(la_pos_) * 2;
        const float ol = la_buf_[li] * lim_gain_, orr = la_buf_[li + 1] * lim_gain_;
        la_buf_[li] = l;
        la_buf_[li + 1] = r;
        la_pos_ = (la_pos_ + 1) % lookahead;
        // soft clip above 0.9 (bounded below 1.0)
        auto soft = [](float x) {
            const float a = std::fabs(x);
            if(a <= 0.9f) return x;
            const float y = 0.9f + 0.0999f * std::tanh((a - 0.9f) / 0.0999f);
            return x < 0 ? -y : y;
        };
        out[2 * f] = soft(ol);
        out[2 * f + 1] = soft(orr);
    }
}

} // namespace geosim::client::audio
