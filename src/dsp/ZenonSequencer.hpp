#pragma once
#include <cmath>

// Rack-independent sequencer logic of Zenon (two polymetric tracks), unit-tested in
// tests/test_zenon_sequencer.cpp. See docs/modules/Zenon-Signalvertrag.md.
// Real-time safe: no allocation, no I/O, fixed-size state.

namespace zenon {

static const int NUM_TRACKS = 2;
static const int NUM_STEPS = 8;

// Destinations of a track. The order is stored in patches (DEST param value): never reorder.
enum Dest {
	DEST_PITCH = 0,   // both oscillators
	DEST_OSC2,        // oscillator 2 only
	DEST_FM,
	DEST_CUTOFF,
	DEST_FILTER_DEC,
	DEST_AMP_DEC,
	DEST_VEL,
	NUM_DESTS
};

// Short names for the destination display.
inline const char* destName(int d) {
	static const char* names[NUM_DESTS] = {"PITCH", "OSC 2", "FM", "CUTOFF", "FLT DEC", "AMP DEC", "VEL"};
	return (d >= 0 && d < NUM_DESTS) ? names[d] : "";
}

inline float clampf(float x, float lo, float hi) {
	return x < lo ? lo : (x > hi ? hi : x);
}

// Replaces NaN/Inf with a fallback value.
inline float finiteOr(float x, float fallback) {
	return std::isfinite(x) ? x : fallback;
}

// LENGTH knob (1..8) plus LENGTH CV (1 V = 1 step), rounded and clamped to 1..8.
inline int lengthFrom(float knob, float cv) {
	float v = finiteOr(knob, (float) NUM_STEPS) + finiteOr(cv, 0.f);
	int n = (int) std::lround(clampf(v, 1.f, (float) NUM_STEPS));
	return n < 1 ? 1 : (n > NUM_STEPS ? NUM_STEPS : n);
}

// DEST knob value to a valid destination index.
inline int destFrom(float knob) {
	int d = (int) std::lround(finiteOr(knob, 0.f));
	return d < 0 ? 0 : (d >= NUM_DESTS ? NUM_DESTS - 1 : d);
}

// One track: only the position; step values and length are read from the controls.
struct Track {
	// -1 = before the first step: the next advance() lands on step 1 (index 0).
	int pos = -1;

	void reset() {
		pos = -1;
	}

	// Moves one step forward. If the length was reduced below the current position,
	// the track continues at step 1.
	// Returns true at the end of a cycle: the track wrapped from a step back to step 1
	// (also when a shorter length cut the cycle). The very first step after reset does not count.
	bool advance(int length) {
		int old = pos;
		pos++;
		if (pos >= length || pos < 0)
			pos = 0;
		return pos == 0 && old >= 0;
	}

	// Index of the step that currently sounds (step 1 before the first advance).
	int current() const {
		return pos < 0 ? 0 : pos;
	}
};

// Internal step clock. Tempo in steps per minute.
struct InternalClock {
	double phase = 0.0;

	// Makes the next process() call tick immediately (used when RUN is switched on).
	void tickNow() {
		phase = 1.0;
	}

	// Restarts the phase: the next tick follows one full period later.
	void restart() {
		phase = 0.0;
	}

	bool process(float stepsPerMinute, float sampleTime) {
		if (phase >= 1.0) {
			phase -= std::floor(phase);
			return true;
		}
		phase += (double) stepsPerMinute / 60.0 * sampleTime;
		if (phase >= 1.0) {
			phase -= std::floor(phase);
			return true;
		}
		return false;
	}
};

// TEMPO param is stored as log2(steps per minute); TEMPO CV adds octaves (1 V doubles the tempo).
static const float TEMPO_MIN_SPM = 40.f;
static const float TEMPO_MAX_SPM = 800.f;
inline float stepsPerMinute(float tempoParamLog2, float cv) {
	float l = finiteOr(tempoParamLog2, std::log2(480.f)) + clampf(finiteOr(cv, 0.f), -5.f, 5.f);
	// The CV may leave the knob range; keep it within a sane absolute range.
	return std::pow(2.f, clampf(l, std::log2(5.f), std::log2(6400.f)));
}

// External clock factor: index -2..+2 = /4, /2, x1, x2, x4.
static const int CLOCK_FACTOR_MIN = -2;
static const int CLOCK_FACTOR_MAX = 2;

// With an external clock, the TEMPO knob selects the factor: its range is split into 5 equal zones.
// TEMPO CV adds one factor step per volt (consistent with "1 V doubles the tempo").
inline int clockFactorIndex(float tempoParamLog2, float cv) {
	float lo = std::log2(TEMPO_MIN_SPM), hi = std::log2(TEMPO_MAX_SPM);
	float pos = clampf((finiteOr(tempoParamLog2, lo) - lo) / (hi - lo), 0.f, 1.f);
	float idx = pos * (CLOCK_FACTOR_MAX - CLOCK_FACTOR_MIN) + CLOCK_FACTOR_MIN + clampf(finiteOr(cv, 0.f), -5.f, 5.f);
	int i = (int) std::lround(idx);
	return i < CLOCK_FACTOR_MIN ? CLOCK_FACTOR_MIN : (i > CLOCK_FACTOR_MAX ? CLOCK_FACTOR_MAX : i);
}

// Divides or multiplies an external clock.
// Divide by N: every N-th clock edge is a step, counted from the first edge after reset/start.
// Multiply by N: every edge is a step, plus N-1 evenly spaced steps in between, based on the
// measured clock period. Until a period is known (first edge), only the edges themselves step.
// A gap longer than 4 periods counts as a restart: the old period is kept.
struct ClockScaler {
	long edgeCount = 0;          // edges since reset
	double sinceEdge = 0.0;      // seconds since the last edge
	double period = 0.0;         // measured clock period in seconds, 0 = unknown
	int subTicksDone = 0;        // extra steps already emitted since the last edge
	bool haveEdge = false;

	void reset() {
		edgeCount = 0;
		subTicksDone = 0;
	}

	bool process(bool edge, int factorIndex, float sampleTime) {
		sinceEdge += sampleTime;
		if (edge) {
			if (haveEdge) {
				double gap = sinceEdge;
				if (period <= 0.0 || gap <= 4.0 * period)
					period = gap;
			}
			haveEdge = true;
			sinceEdge = 0.0;
			subTicksDone = 0;
			long n = edgeCount++;
			if (factorIndex < 0) {
				long div = 1L << (-factorIndex);
				return n % div == 0;
			}
			return true;
		}
		if (factorIndex > 0 && haveEdge && period > 0.0) {
			int mult = 1 << factorIndex;
			// Next extra step k (1..mult-1) is due at k * period / mult after the edge.
			if (subTicksDone < mult - 1 && sinceEdge >= (subTicksDone + 1) * period / mult) {
				subTicksDone++;
				return true;
			}
		}
		return false;
	}
};

// Decides per sample whether the sequencer steps. Edge detection is done by the caller.
struct Transport {
	bool running = false;
	InternalClock clock;
	ClockScaler scaler;

	// Switches RUN on or off. Starting plays the first step immediately (internal clock) or with
	// the next clock edge (external clock).
	void setRunning(bool on) {
		if (on && !running) {
			clock.tickNow();
			scaler.reset();
		}
		running = on;
	}

	void reset() {
		clock.restart();
		scaler.reset();
	}

	// clockConnected: external CLOCK input is patched (internal clock is off then).
	// clockEdge: rising edge at CLOCK (only counts while running).
	// advEdge: ADV button pressed (always steps, also when stopped).
	// factorIndex: external clock factor, see clockFactorIndex().
	bool process(bool clockConnected, bool clockEdge, bool advEdge, float spm, float sampleTime, int factorIndex = 0) {
		bool tick = advEdge;
		if (clockConnected) {
			// The scaler always measures the period, but only steps while running.
			bool t = scaler.process(clockEdge && running, factorIndex, sampleTime);
			if (running && t)
				tick = true;
		}
		else if (running) {
			if (clock.process(spm, sampleTime))
				tick = true;
		}
		return tick;
	}
};

// Combined modulation of both tracks. Same destination on both tracks: contributions add up.
struct Mods {
	float pitchOct = 0.f;      // both oscillators, octaves
	float osc2Oct = 0.f;       // oscillator 2 only, octaves
	float fm = 0.f;            // added to FM amount (0..1 scale)
	float cutoffOct = 0.f;     // octaves
	float filterDec = 0.f;     // added to slider position (0..1 scale)
	float ampDec = 0.f;        // added to slider position (0..1 scale)
	float velGain = 1.f;       // 0..1
};

static const float PITCH_RANGE_OCT = 4.f;
static const float CUTOFF_RANGE_OCT = 6.f;

// VEL contribution: at a = +1 the level follows the step value, at a = -1 a high step is quiet.
inline float velContribution(float s, float a) {
	return a >= 0.f ? a * (s - 1.f) : a * s;
}

// dest[t], step value s[t] (0..1) and amount a[t] (-1..+1) of both tracks.
inline Mods computeMods(const int* dest, const float* s, const float* a) {
	Mods m;
	float vel = 0.f;
	for (int t = 0; t < NUM_TRACKS; t++) {
		float st = clampf(finiteOr(s[t], 0.f), 0.f, 1.f);
		float at = clampf(finiteOr(a[t], 0.f), -1.f, 1.f);
		float v = st * at;
		switch (dest[t]) {
			case DEST_PITCH: m.pitchOct += v * PITCH_RANGE_OCT; break;
			case DEST_OSC2: m.osc2Oct += v * PITCH_RANGE_OCT; break;
			case DEST_FM: m.fm += v; break;
			case DEST_CUTOFF: m.cutoffOct += v * CUTOFF_RANGE_OCT; break;
			case DEST_FILTER_DEC: m.filterDec += v; break;
			case DEST_AMP_DEC: m.ampDec += v; break;
			case DEST_VEL: vel += velContribution(st, at); break;
			default: break;
		}
	}
	m.velGain = clampf(1.f + vel, 0.f, 1.f);
	return m;
}

} // namespace zenon
