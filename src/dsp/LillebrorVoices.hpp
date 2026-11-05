#pragma once
#include <cmath>
#include <cstdint>

// Rack-independent DSP of Lillebror (three drum voices), so it can be unit-tested without Rack.
// All voices produce a normalized signal (roughly -1..+1 for a non-accented hit at full level);
// the module scales it to voltages.
// Real-time safe: no allocation, no I/O, fixed-size state. Time-dependent values are derived
// from the sample time passed to process(), so sample rate changes are handled automatically.

namespace lillebror {

static const float PI_F = 3.14159265358979f;

inline float clampf(float x, float lo, float hi) {
	return x < lo ? lo : (x > hi ? hi : x);
}

// Replaces NaN/Inf with a fallback value.
inline float finiteOr(float x, float fallback) {
	return std::isfinite(x) ? x : fallback;
}

// Exponential mapping of a 0..1 control to minValue..maxValue.
inline float expMap(float x, float minValue, float maxValue) {
	return minValue * std::pow(maxValue / minValue, clampf(x, 0.f, 1.f));
}

// White noise has a constant power per sample, so its power per Hz falls with rising sample rate.
// Scaling by sqrt(fs / 48 kHz) keeps the audible (band-limited) noise level independent of fs.
inline float noiseScale(float sampleTime) {
	return std::sqrt(1.f / (48000.f * sampleTime));
}

// Small, fast white noise source (xorshift32), output in -1..+1.
struct Noise {
	uint32_t state = 0x9E3779B9u;
	void seed(uint32_t s) {
		state = s ? s : 0x9E3779B9u;
	}
	float next() {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return (float) (int32_t) state * (1.f / 2147483648.f);
	}
};

// Topology-preserving state variable filter (Zavalishin), 12 dB/oct, provides LP/BP/HP.
struct Svf {
	float ic1 = 0.f, ic2 = 0.f;
	float lp = 0.f, bp = 0.f, hp = 0.f;
	void reset() {
		ic1 = ic2 = lp = bp = hp = 0.f;
	}
	void process(float in, float cutoff, float q, float sampleTime) {
		// Keep the cutoff safely below Nyquist for any sample rate.
		float fc = clampf(cutoff, 10.f, 0.45f / sampleTime);
		float g = std::tan(PI_F * fc * sampleTime);
		float k = 1.f / clampf(q, 0.3f, 20.f);
		float a1 = 1.f / (1.f + g * (g + k));
		float a2 = g * a1;
		float a3 = g * a2;
		float v3 = in - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		lp = v2;
		bp = v1;
		hp = in - k * v1 - v2;
	}
};

// PolyBLEP residual for band-limited discontinuities. t = phase in 0..1, dt = phase increment.
inline float polyBlep(float t, float dt) {
	if (t < dt) {
		t /= dt;
		return t + t - t * t - 1.f;
	}
	if (t > 1.f - dt) {
		t = (t - 1.f) / dt;
		return t * t + t + t + 1.f;
	}
	return 0.f;
}

// Oscillator with continuous wave morph: 0 = sine, 1/3 = triangle, 2/3 = saw, 1 = square.
// All shapes start at zero with a rising slope at phase 0, so crossfades do not cancel out.
// Saw and square are band-limited with PolyBLEP; the triangle is naive (soft corners, low aliasing).
struct MorphOsc {
	float phase = 0.f;
	void reset(float p = 0.f) {
		phase = p;
	}
	static float shape(int index, float p, float dt) {
		switch (index) {
			case 0:
				return std::sin(2.f * PI_F * p);
			case 1:
				if (p < 0.25f)
					return 4.f * p;
				if (p < 0.75f)
					return 2.f - 4.f * p;
				return 4.f * p - 4.f;
			case 2: {
				// Rising saw with its jump at phase 0.5.
				float t = p + 0.5f;
				if (t >= 1.f)
					t -= 1.f;
				return 2.f * t - 1.f - polyBlep(t, dt);
			}
			default: {
				float t2 = p + 0.5f;
				if (t2 >= 1.f)
					t2 -= 1.f;
				float y = p < 0.5f ? 1.f : -1.f;
				return y + polyBlep(p, dt) - polyBlep(t2, dt);
			}
		}
	}
	float process(float freq, float wave, float sampleTime) {
		float dt = clampf(freq * sampleTime, 0.f, 0.45f);
		float w = clampf(wave, 0.f, 1.f) * 3.f;
		int i = (int) w;
		if (i > 2)
			i = 2;
		float frac = w - (float) i;
		float y = shape(i, phase, dt);
		if (frac > 0.f)
			y += (shape(i + 1, phase, dt) - y) * frac;
		phase += dt;
		if (phase >= 1.f)
			phase -= 1.f;
		return y;
	}
};

// Exponential decay envelope with a short linear attack (avoids clicks, also on retrigger).
struct DecayEnv {
	static constexpr float ATTACK_TIME = 0.0005f;
	float value = 0.f;
	bool attacking = false;
	void trigger() {
		attacking = true;
	}
	void reset() {
		value = 0.f;
		attacking = false;
	}
	// decayTime = time to fall by 60 dB, in seconds.
	float process(float decayTime, float sampleTime) {
		if (attacking) {
			value += sampleTime / ATTACK_TIME;
			if (value >= 1.f) {
				value = 1.f;
				attacking = false;
			}
		}
		else {
			float tau = clampf(decayTime, 0.001f, 30.f) / 6.9078f;
			value *= std::exp(-sampleTime / tau);
			if (value < 1e-6f)
				value = 0.f;
		}
		return value;
	}
};

// Accent detection: the accent is read at the trigger and for a short window afterwards,
// so an accent that arrives a few samples late still counts.
struct AccentLatch {
	static constexpr float WINDOW = 0.001f;
	bool accent = false;
	float window = 0.f;
	void trigger(bool high) {
		accent = high;
		window = WINDOW;
	}
	void process(bool high, float sampleTime) {
		if (window > 0.f) {
			if (high)
				accent = true;
			window -= sampleTime;
		}
	}
	void reset() {
		accent = false;
		window = 0.f;
	}
};

// Accent: +6 dB on every voice; the timbre change differs per voice.
static const float ACCENT_GAIN = 2.f;

// Common controls, all normalized to 0..1 (CV already added and clamped by the caller),
// except tuneOct, which is in octaves relative to the voice's base frequency.
struct VoiceControls {
	float tuneOct = 0.f;
	float wave = 0.f;
	float a = 0.5f;      // kick: sweep, snare: tone, hi-hat: color
	float b = 0.5f;      // kick: click, snare: snap, hi-hat: bright
	float decay = 0.5f;
};

// KICK: morphing oscillator with exponential pitch sweep and a noise click.
struct KickVoice {
	static constexpr float BASE_FREQ = 55.f;
	static constexpr float DECAY_MIN = 0.03f, DECAY_MAX = 2.f;
	static constexpr float SWEEP_OCTAVES = 4.f;
	static constexpr float SWEEP_TIME = 0.03f;     // pitch envelope time constant
	static constexpr float CLICK_TIME = 0.0015f;   // click time constant
	static constexpr float ACCENT_DECAY = 1.5f;    // accent: longer and fuller

	MorphOsc osc;
	DecayEnv amp;
	AccentLatch acc;
	Noise noise;
	Svf clickFilter, clickLp;
	float pitchEnv = 0.f;
	float clickEnv = 0.f;

	void reset() {
		osc.reset();
		amp.reset();
		acc.reset();
		clickFilter.reset();
		clickLp.reset();
		pitchEnv = clickEnv = 0.f;
	}
	void trigger(bool accentHigh) {
		osc.reset();
		amp.trigger();
		acc.trigger(accentHigh);
		pitchEnv = 1.f;
		clickEnv = 1.f;
	}
	float process(const VoiceControls& c, bool accentHigh, float sampleTime) {
		acc.process(accentHigh, sampleTime);
		float decay = expMap(c.decay, DECAY_MIN, DECAY_MAX) * (acc.accent ? ACCENT_DECAY : 1.f);
		float env = amp.process(decay, sampleTime);
		pitchEnv *= std::exp(-sampleTime / SWEEP_TIME);
		clickEnv *= std::exp(-sampleTime / CLICK_TIME);
		float freq = BASE_FREQ * std::pow(2.f, c.tuneOct + clampf(c.a, 0.f, 1.f) * SWEEP_OCTAVES * pitchEnv);
		float tone = osc.process(freq, c.wave, sampleTime);
		clickFilter.process(noise.next() * noiseScale(sampleTime), 3000.f, 0.7f, sampleTime);
		clickLp.process(clickFilter.hp, 12000.f, 0.7f, sampleTime);
		float click = clickLp.lp * clickEnv * clampf(c.b, 0.f, 1.f) * 0.9f;
		float y = tone * env + click;
		return y * (acc.accent ? ACCENT_GAIN : 1.f);
	}
};

// SNARE: morphing body oscillator (with a short pitch drop) plus filtered noise and a snap burst.
struct SnareVoice {
	static constexpr float BASE_FREQ = 180.f;
	static constexpr float DECAY_MIN = 0.03f, DECAY_MAX = 1.f;
	static constexpr float BODY_DROP_OCTAVES = 0.5f;
	static constexpr float BODY_DROP_TIME = 0.012f;
	static constexpr float SNAP_TIME = 0.008f;
	static constexpr float ACCENT_DECAY = 0.7f;   // accent: shorter ...
	static constexpr float ACCENT_SNAP = 1.5f;    // ... and snappier

	MorphOsc osc;
	DecayEnv bodyEnv, noiseEnv;
	AccentLatch acc;
	Noise noise;
	Svf noiseHp, noiseLp;
	float dropEnv = 0.f;
	float snapEnv = 0.f;

	SnareVoice() {
		noise.seed(0x1234567u);
	}
	void reset() {
		osc.reset();
		bodyEnv.reset();
		noiseEnv.reset();
		acc.reset();
		noiseHp.reset();
		noiseLp.reset();
		dropEnv = snapEnv = 0.f;
	}
	void trigger(bool accentHigh) {
		osc.reset();
		bodyEnv.trigger();
		noiseEnv.trigger();
		acc.trigger(accentHigh);
		dropEnv = 1.f;
		snapEnv = 1.f;
	}
	float process(const VoiceControls& c, bool accentHigh, float sampleTime) {
		acc.process(accentHigh, sampleTime);
		float decay = expMap(c.decay, DECAY_MIN, DECAY_MAX) * (acc.accent ? ACCENT_DECAY : 1.f);
		// The body decays faster than the noise, like on a real snare.
		float be = bodyEnv.process(decay * 0.6f, sampleTime);
		float ne = noiseEnv.process(decay, sampleTime);
		dropEnv *= std::exp(-sampleTime / BODY_DROP_TIME);
		snapEnv *= std::exp(-sampleTime / SNAP_TIME);
		float freq = BASE_FREQ * std::pow(2.f, c.tuneOct + BODY_DROP_OCTAVES * dropEnv);
		float body = osc.process(freq, c.wave, sampleTime);
		float n = noise.next() * noiseScale(sampleTime);
		noiseHp.process(n, 1200.f, 0.7f, sampleTime);
		noiseLp.process(noiseHp.hp, 9000.f, 0.7f, sampleTime);
		float filtered = noiseLp.lp;
		float toneMix = clampf(c.a, 0.f, 1.f);   // 0 = only body, 1 = only noise
		float snap = clampf(c.b * (acc.accent ? ACCENT_SNAP : 1.f), 0.f, 1.5f);
		float y = body * be * (1.f - toneMix) + filtered * ne * toneMix * 1.2f + noiseLp.lp * snapEnv * snap * 0.5f;
		return y * (acc.accent ? ACCENT_GAIN : 1.f);
	}
};

// HI-HAT: cluster of six detuned morphing oscillators (808-style ratios) mixed with noise,
// through a 2-pole high-pass (BRIGHT) and a fixed 16 kHz low-pass.
struct HiHatVoice {
	static constexpr int NUM_OSC = 6;
	static constexpr float BASE_FREQ = 400.f;
	static constexpr float DECAY_MIN = 0.01f, DECAY_MAX = 1.5f;
	static constexpr float BRIGHT_MIN = 400.f, BRIGHT_MAX = 7000.f;
	static constexpr float ACCENT_DECAY = 2.f;   // accent: more open
	static constexpr float HH_GAIN = 2.2f;

	MorphOsc osc[NUM_OSC];
	DecayEnv amp;
	AccentLatch acc;
	Noise noise;
	Svf hp1, lp;

	HiHatVoice() {
		noise.seed(0xBADC0DEu);
		// Different start phases so the cluster does not start with all edges aligned.
		for (int i = 0; i < NUM_OSC; i++)
			osc[i].reset(0.13f * (float) i);
	}
	static float ratio(int i) {
		// Frequency ratios of the classic 808 hi-hat square cluster, relative to the lowest one.
		static const float r[NUM_OSC] = {1.f, 1.4826f, 1.8004f, 2.5461f, 2.6304f, 3.9005f};
		return r[i];
	}
	void reset() {
		amp.reset();
		acc.reset();
		hp1.reset();
		lp.reset();
	}
	void trigger(bool accentHigh) {
		amp.trigger();
		acc.trigger(accentHigh);
	}
	float process(const VoiceControls& c, bool accentHigh, float sampleTime) {
		acc.process(accentHigh, sampleTime);
		float decay = expMap(c.decay, DECAY_MIN, DECAY_MAX) * (acc.accent ? ACCENT_DECAY : 1.f);
		float env = amp.process(decay, sampleTime);
		float base = BASE_FREQ * std::pow(2.f, c.tuneOct);
		float cluster = 0.f;
		for (int i = 0; i < NUM_OSC; i++)
			cluster += osc[i].process(base * ratio(i), c.wave, sampleTime);
		// Harder waves have a much higher level; compensate so WAVE mainly changes the timbre.
		cluster *= (1.f - 0.45f * clampf(c.wave, 0.f, 1.f)) / NUM_OSC;
		float color = clampf(c.a, 0.f, 1.f);   // 0 = metallic cluster, 1 = noise
		float src = cluster * (1.f - color) + noise.next() * noiseScale(sampleTime) * 0.5f * color;
		float cutoff = expMap(c.b, BRIGHT_MIN, BRIGHT_MAX);
		hp1.process(src, cutoff, 0.7f, sampleTime);
		// Band-limit to the audible range so the level does not depend on the sample rate.
		lp.process(hp1.hp, 16000.f, 0.7f, sampleTime);
		float y = lp.lp * env * HH_GAIN;
		return y * (acc.accent ? ACCENT_GAIN : 1.f);
	}
};

}  // namespace lillebror
