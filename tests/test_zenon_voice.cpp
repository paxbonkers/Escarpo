// Standalone tests for the Zenon voice (no Rack needed).
// Build & run:  c++ -std=c++11 -O2 -Wall -Isrc tests/test_zenon_voice.cpp -o build/test_zenon_voice && ./build/test_zenon_voice
#include "dsp/ZenonVoice.hpp"
#include <cstdio>
#include <cmath>

using namespace zenon;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

static const float RATES[] = {44100.f, 48000.f, 96000.f, 192000.f};

struct Stats {
	float peak = 0.f;
	bool finite = true;
	double sumSq = 0.0;
	long n = 0;
	float rms() const { return n ? (float) std::sqrt(sumSq / n) : 0.f; }
};

static Stats runAudio(Voice& v, const VoiceControls& c, float sr, float seconds, bool trig = true) {
	Stats s;
	if (trig)
		v.trigger(1.f);
	long n = (long) (seconds * sr);
	for (long i = 0; i < n; i++) {
		float y = v.process(c, 1.f / sr).audio;
		if (!std::isfinite(y))
			s.finite = false;
		s.peak = std::fmax(s.peak, std::fabs(y));
		s.sumSq += (double) y * y;
		s.n++;
	}
	return s;
}

static void testSilenceBeforeTrigger() {
	for (float sr : RATES) {
		Voice v;
		VoiceControls c;
		c.mixNoise = 1.f;
		Stats s = runAudio(v, c, sr, 0.1f, false);
		CHECK(s.peak == 0.f, "%.0f Hz: audio before the first trigger, peak %f", sr, s.peak);
	}
}

static void testExtremesFinite() {
	for (float sr : RATES) {
		VoiceControls c;
		c.osc1Square = c.osc2Square = true;
		c.fm = 1.f;
		c.sync = true;
		c.mixOsc1 = c.mixOsc2 = c.mixNoise = 1.f;
		c.res = 1.f;
		c.pitchAmt = 1.f;
		c.filterAmt = 1.f;
		c.level = 1.f;
		const float octs[] = {-12.f, -5.f, 4.f, 8.f};
		const float cuts[] = {0.f, 3.3f, 11.f, 20.f};
		for (float o : octs) {
			for (float cut : cuts) {
				Voice v;
				c.osc1Oct = o;
				c.osc2Oct = o + 1.f;
				c.cutoffLog2 = cut;
				Stats s = runAudio(v, c, sr, 0.2f);
				CHECK(s.finite, "%.0f Hz oct %.0f cut %.1f: non-finite output", sr, o, cut);
				CHECK(s.peak < 12.f, "%.0f Hz oct %.0f cut %.1f: peak too high %f", sr, o, cut, s.peak);
			}
		}
		// HP mode and negative amounts.
		Voice v;
		c.highpass = true;
		c.pitchAmt = -1.f;
		c.filterAmt = -1.f;
		c.osc1Oct = 0.f;
		c.cutoffLog2 = 11.f;
		Stats s = runAudio(v, c, sr, 0.2f);
		CHECK(s.finite && s.peak < 12.f, "%.0f Hz: HP extreme, peak %f", sr, s.peak);
	}
}

static void testDecayTo60dB() {
	// AMP decay at the maximum (3 s): below -60 dB of the peak after 3.1 s, at every sample rate.
	for (float sr : RATES) {
		Voice v;
		VoiceControls c;
		c.ampDec = 1.f;
		v.trigger(1.f);
		long n = (long) (3.1f * sr);
		float env = 0.f;
		for (long i = 0; i < n; i++)
			env = v.process(c, 1.f / sr).ampEnv;
		CHECK(env < 0.001f, "%.0f Hz: amp env after 3.1 s = %f", sr, env);
	}
}

static void testEnvelopeSampleRateIndependent() {
	// Envelope value 100 ms after the trigger is the same at all sample rates.
	float ref = -1.f;
	for (float sr : RATES) {
		Voice v;
		VoiceControls c;
		v.trigger(1.f);
		long n = (long) (0.1f * sr);
		float env = 0.f;
		for (long i = 0; i < n; i++)
			env = v.process(c, 1.f / sr).ampEnv;
		if (ref < 0.f)
			ref = env;
		CHECK(std::fabs(env - ref) < 0.01f, "%.0f Hz: env %f vs %f", sr, env, ref);
	}
}

static void testOscFrequency() {
	// Oscillator 1 at C4 - 1 octave (130.8 Hz), pitch env off: count rising zero crossings in 1 s.
	for (float sr : RATES) {
		Voice v;
		VoiceControls c;
		c.osc1Oct = -1.f;
		c.pitchAmt = 0.f;
		v.trigger(1.f);
		long n = (long) sr;
		float prev = 0.f;
		int crossings = 0;
		for (long i = 0; i < n; i++) {
			float y = v.process(c, 1.f / sr).osc1;
			if (prev < 0.f && y >= 0.f)
				crossings++;
			prev = y;
		}
		CHECK(std::abs(crossings - 131) <= 1, "%.0f Hz: expected ~131 cycles, got %d", sr, crossings);
	}
}

static void testFilter() {
	// A 2 kHz square through LP at 50 Hz is much quieter than through LP at 16 kHz.
	VoiceControls c;
	c.osc1Oct = std::log2(2000.f / C4_HZ);
	c.osc1Square = true;
	c.pitchAmt = 0.f;
	c.filterAmt = 0.f;
	c.res = 0.f;
	c.ampDec = 1.f;
	Voice a, b;
	c.cutoffLog2 = std::log2(50.f);
	float lowRms = runAudio(a, c, 48000.f, 0.2f).rms();
	c.cutoffLog2 = std::log2(16000.f);
	float highRms = runAudio(b, c, 48000.f, 0.2f).rms();
	CHECK(lowRms < highRms * 0.05f, "LP 50 Hz rms %f vs 16 kHz rms %f", lowRms, highRms);
	// HP at 16 kHz removes most of a 100 Hz triangle.
	VoiceControls h;
	h.osc1Oct = std::log2(100.f / C4_HZ);
	h.pitchAmt = 0.f;
	h.filterAmt = 0.f;
	h.res = 0.f;
	h.ampDec = 1.f;
	h.highpass = true;
	h.cutoffLog2 = std::log2(16000.f);
	Voice d, e;
	float hpRms = runAudio(d, h, 48000.f, 0.2f).rms();
	h.highpass = false;
	float lpRms = runAudio(e, h, 48000.f, 0.2f).rms();
	CHECK(hpRms < lpRms * 0.05f, "HP rms %f vs LP rms %f", hpRms, lpRms);
}

static void testPhaseResetRepeatable() {
	// Without noise, two hits produce exactly the same first 10 ms.
	VoiceControls c;
	c.osc2Oct = 0.3f;
	c.mixOsc2 = 0.5f;
	c.fm = 0.4f;
	Voice v;
	const float sr = 48000.f;
	const int n = 480;
	float first[n];
	v.trigger(1.f);
	for (int i = 0; i < n; i++)
		first[i] = v.process(c, 1.f / sr).audio;
	// Let it ring out, then hit again.
	for (int i = 0; i < 48000 * 4; i++)
		v.process(c, 1.f / sr);
	v.reset();
	v.trigger(1.f);
	float maxDiff = 0.f;
	for (int i = 0; i < n; i++)
		maxDiff = std::fmax(maxDiff, std::fabs(v.process(c, 1.f / sr).audio - first[i]));
	CHECK(maxDiff < 1e-5f, "hits differ by %f", maxDiff);
}

static void testRetriggerNoJump() {
	// A retrigger during the decay rises from the current value: no jump in the amp envelope.
	Voice v;
	VoiceControls c;
	const float sr = 48000.f;
	v.trigger(1.f);
	float prev = 0.f, maxStep = 0.f;
	for (int i = 0; i < 4800; i++) {
		if (i == 2400)
			v.trigger(1.f);
		float e = v.process(c, 1.f / sr).ampEnv;
		if (i > 0)
			maxStep = std::fmax(maxStep, std::fabs(e - prev));
		prev = e;
	}
	// Attack is 0.5 ms = 24 samples: at most 1/24 per sample.
	CHECK(maxStep < 1.f / 24.f + 1e-4f, "envelope jumped by %f", maxStep);
}

static void testVelocityAndLevel() {
	VoiceControls c;
	c.pitchAmt = 0.f;
	Voice a, b;
	a.trigger(1.f);
	b.trigger(0.5f);
	float pa = 0.f, pb = 0.f;
	for (int i = 0; i < 4800; i++) {
		pa = std::fmax(pa, std::fabs(a.process(c, 1.f / 48000.f).audio));
		pb = std::fmax(pb, std::fabs(b.process(c, 1.f / 48000.f).audio));
	}
	CHECK(std::fabs(pb - 0.5f * pa) < 0.01f * pa, "velocity 0.5 halves the level: %f vs %f", pb, pa);
	c.level = 0.f;
	Voice z;
	CHECK(runAudio(z, c, 48000.f, 0.1f).peak == 0.f, "level 0 is silent");
}

int main() {
	testSilenceBeforeTrigger();
	testExtremesFinite();
	testDecayTo60dB();
	testEnvelopeSampleRateIndependent();
	testOscFrequency();
	testFilter();
	testPhaseResetRepeatable();
	testRetriggerNoJump();
	testVelocityAndLevel();
	if (failures == 0)
		std::printf("All Zenon voice tests passed.\n");
	return failures == 0 ? 0 : 1;
}
