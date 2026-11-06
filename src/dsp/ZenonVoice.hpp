#pragma once
#include <cmath>
#include <cstdint>
#include "ZenonSequencer.hpp"

// Rack-independent sound generation of Zenon: two oscillators (FM 1->2, hard sync), noise / external
// input, mixer, state variable filter (LP/HP) and three decay envelopes (pitch, filter, amp).
// See docs/modules/Zenon-Signalvertrag.md. Unit-tested in tests/test_zenon_voice.cpp.
// Real-time safe: no allocation, no I/O, fixed-size state. All time constants are derived from
// the sample time passed to process(), so sample rate changes are handled automatically.
// Signals are normalized (oscillators -1..+1, envelopes 0..1); the module scales them to volts.

namespace zenon {

static const float PI_F = 3.14159265358979f;
static const float C4_HZ = 261.6256f;

// Exponential mapping of a 0..1 control to minValue..maxValue.
inline float expMap(float x, float minValue, float maxValue) {
	return minValue * std::pow(maxValue / minValue, clampf(x, 0.f, 1.f));
}

// Decay ranges (time to -60 dB) for the slider positions 0..1.
static const float PITCH_DEC_MIN = 0.005f, PITCH_DEC_MAX = 1.f;
static const float FILTER_DEC_MIN = 0.005f, FILTER_DEC_MAX = 2.f;
static const float AMP_DEC_MIN = 0.01f, AMP_DEC_MAX = 3.f;
static const float ENV_ATTACK = 0.0005f;
static const float PITCH_ENV_OCT = 4.f;
static const float FILTER_ENV_OCT = 6.f;
static const float FM_RANGE_OCT = 4.f;

// Small, fast white noise source (xorshift32), output in -1..+1.
struct Noise {
	uint32_t state = 0x2545F491u;
	float next() {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return (float) (int32_t) state * (1.f / 2147483648.f);
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

// Oscillator with triangle or square. Both start at phase 0; the triangle starts at zero with a
// rising slope. The square is band-limited with PolyBLEP; the triangle is naive (low aliasing).
struct Osc {
	float phase = 0.f;
	void reset() {
		phase = 0.f;
	}
	static float shape(bool square, float p, float dt) {
		if (square) {
			float t2 = p + 0.5f;
			if (t2 >= 1.f)
				t2 -= 1.f;
			float y = p < 0.5f ? 1.f : -1.f;
			return y + polyBlep(p, dt) - polyBlep(t2, dt);
		}
		if (p < 0.25f)
			return 4.f * p;
		if (p < 0.75f)
			return 2.f - 4.f * p;
		return 4.f * p - 4.f;
	}
	// Returns the output for the current phase, then advances. wrapped is set when the phase
	// wrapped; wrapFraction is the part of the sample after the wrap (for hard sync).
	float process(bool square, float dt, bool& wrapped, float& wrapFraction) {
		float y = shape(square, phase, dt);
		phase += dt;
		wrapped = false;
		wrapFraction = 0.f;
		if (phase >= 1.f) {
			phase -= std::floor(phase);
			wrapped = true;
			wrapFraction = dt > 0.f ? phase / dt : 0.f;
		}
		return y;
	}
};

// Topology-preserving state variable filter (Zavalishin), 12 dB/oct, LP and HP outputs.
struct Svf {
	float ic1 = 0.f, ic2 = 0.f;
	float lp = 0.f, hp = 0.f;
	void reset() {
		ic1 = ic2 = lp = hp = 0.f;
	}
	// res 0..1: damping k from 2 (no resonance) down to 0.04 (just below self-oscillation).
	// v2 = ic2 + g * v1 is the TPT integrator form; it equals the linear formula when v1 is not saturated.
	void process(float in, float cutoff, float res, float sampleTime) {
		float fc = clampf(cutoff, 10.f, 0.45f / sampleTime);
		float g = std::tan(PI_F * fc * sampleTime);
		float k = 2.f - 1.96f * clampf(res, 0.f, 1.f);
		float a1 = 1.f / (1.f + g * (g + k));
		float a2 = g * a1;
		float v3 = in - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		// Soft saturation of the band-pass state (about +-2.5) limits the resonant peak, like an
		// analog filter that saturates. Normal levels (|v1| < 1) are hardly affected.
		float v1s = v1 / std::sqrt(1.f + v1 * v1 * (1.f / 6.25f));
		v1 = v1s;
		float v2 = ic2 + g * v1;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		lp = v2;
		hp = in - k * v1 - v2;
	}
};

// Attack-decay envelope. Retrigger rises from the current value (no clicks); exponential decay.
struct DecayEnv {
	float value = 0.f;
	bool attacking = false;
	void reset() {
		value = 0.f;
		attacking = false;
	}
	void trigger() {
		attacking = true;
	}
	// decayTime = time to fall by 60 dB.
	float process(float decayTime, float sampleTime) {
		if (attacking) {
			value += sampleTime / ENV_ATTACK;
			if (value >= 1.f) {
				value = 1.f;
				attacking = false;
			}
		}
		else {
			// exp(-6.9078) = 1/1000 (-60 dB) after decayTime.
			value *= std::exp(-6.9078f * sampleTime / std::fmax(decayTime, 1e-4f));
			if (value < 1e-7f)
				value = 0.f;
		}
		return value;
	}
};

// All controls of the voice, already combined (knobs + CV + track modulation).
struct VoiceControls {
	float osc1Oct = -2.f;      // octaves relative to C4
	float osc2Oct = -1.f;
	bool osc1Square = false;
	bool osc2Square = false;
	float fm = 0.f;            // 0..1
	bool sync = false;
	float mixOsc1 = 0.8f, mixOsc2 = 0.f, mixNoise = 0.f;  // 0..1
	bool extConnected = false;
	float ext = 0.f;           // external input, normalized (5 V = 1)
	float cutoffLog2 = 11.f;   // log2(Hz), 11 = 2048 Hz
	float res = 0.2f;          // 0..1
	bool highpass = false;
	float level = 0.8f;        // 0..1
	float pitchDec = 0.15f, filterDec = 0.3f, ampDec = 0.35f;  // slider positions 0..1
	float pitchAmt = 0.3f, filterAmt = 0.4f;                    // -1..+1
};

struct VoiceOutputs {
	float osc1 = 0.f, osc2 = 0.f;              // -1..+1
	float pitchEnv = 0.f, filterEnv = 0.f;     // 0..1
	float ampEnv = 0.f;                        // 0..1, includes the velocity of the hit
	float audio = 0.f;                         // normalized, about -1..+1 for a normal hit
};

struct Voice {
	Osc osc1, osc2;
	Noise noise;
	Svf svf;
	DecayEnv pitchEnv, filterEnv, ampEnv;
	float velocity = 1.f;

	void reset() {
		osc1.reset();
		osc2.reset();
		svf.reset();
		pitchEnv.reset();
		filterEnv.reset();
		ampEnv.reset();
		velocity = 1.f;
	}

	// Starts a hit. Oscillator phases are reset so every hit starts the same way.
	void trigger(float vel) {
		velocity = clampf(finiteOr(vel, 1.f), 0.f, 1.f);
		osc1.reset();
		osc2.reset();
		pitchEnv.trigger();
		filterEnv.trigger();
		ampEnv.trigger();
	}

	static float oscFreq(float oct, float sampleTime) {
		float f = C4_HZ * std::pow(2.f, clampf(finiteOr(oct, 0.f), -12.f, 8.f));
		return clampf(f, 0.1f, 0.45f / sampleTime);
	}

	VoiceOutputs process(const VoiceControls& c, float sampleTime) {
		VoiceOutputs o;
		float pe = pitchEnv.process(expMap(c.pitchDec, PITCH_DEC_MIN, PITCH_DEC_MAX), sampleTime);
		float fe = filterEnv.process(expMap(c.filterDec, FILTER_DEC_MIN, FILTER_DEC_MAX), sampleTime);
		float ae = ampEnv.process(expMap(c.ampDec, AMP_DEC_MIN, AMP_DEC_MAX), sampleTime);

		float pitchMod = clampf(c.pitchAmt, -1.f, 1.f) * PITCH_ENV_OCT * pe;
		float dt1 = oscFreq(c.osc1Oct + pitchMod, sampleTime) * sampleTime;
		bool wrapped;
		float frac;
		float y1 = osc1.process(c.osc1Square, dt1, wrapped, frac);

		// Exponential FM of oscillator 2 by oscillator 1.
		float fmOct = clampf(c.fm, 0.f, 1.f) * FM_RANGE_OCT * y1;
		float dt2 = oscFreq(c.osc2Oct + pitchMod + fmOct, sampleTime) * sampleTime;
		bool w2;
		float f2;
		float y2 = osc2.process(c.osc2Square, dt2, w2, f2);
		// Hard sync: restart oscillator 2 where oscillator 1 wrapped (sub-sample position).
		if (c.sync && wrapped)
			osc2.phase = clampf(frac * dt2, 0.f, 0.999999f);

		float src3 = c.extConnected ? finiteOr(c.ext, 0.f) : noise.next();
		float mix = y1 * clampf(c.mixOsc1, 0.f, 1.f) + y2 * clampf(c.mixOsc2, 0.f, 1.f)
			+ src3 * clampf(c.mixNoise, 0.f, 1.f);

		float cutLog2 = finiteOr(c.cutoffLog2, 11.f) + clampf(c.filterAmt, -1.f, 1.f) * FILTER_ENV_OCT * fe;
		float cutoff = std::pow(2.f, clampf(cutLog2, 3.3f, 15.f));
		svf.process(mix, cutoff, c.res, sampleTime);
		float filtered = c.highpass ? svf.hp : svf.lp;

		float amp = ae * velocity;
		o.osc1 = y1;
		o.osc2 = y2;
		o.pitchEnv = pe;
		o.filterEnv = fe;
		o.ampEnv = amp;
		o.audio = filtered * amp * clampf(c.level, 0.f, 1.f);
		if (!std::isfinite(o.audio)) {
			// Never let a non-finite value stick in the filter state.
			svf.reset();
			o.audio = 0.f;
		}
		return o;
	}
};

} // namespace zenon
