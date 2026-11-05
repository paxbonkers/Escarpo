// Standalone tests for the Lillebror drum voices (no Rack needed).
// Build & run:  c++ -std=c++11 -O2 -Isrc tests/test_lillebror.cpp -o build/test_lillebror && ./build/test_lillebror
#include "dsp/LillebrorVoices.hpp"
#include <cstdio>
#include <cmath>
#include <initializer_list>

using namespace lillebror;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

struct Stats {
	float peak = 0.f;        // max |y| over the whole run
	float tailPeak = 0.f;    // max |y| in the last 10 % of the run
	bool finite = true;
	double sumSq = 0.0;
};

// Triggers the voice at t = 0 (and optionally again at retriggerAt) and runs for `seconds`.
template <typename V>
static Stats run(V& v, const VoiceControls& c, float sampleRate, float seconds, bool accent,
                 float retriggerAt = -1.f, float accentDelay = 0.f) {
	Stats s;
	float dt = 1.f / sampleRate;
	long n = (long) (seconds * sampleRate);
	long retrig = retriggerAt >= 0.f ? (long) (retriggerAt * sampleRate) : -1;
	long accSamples = (long) (accentDelay * sampleRate);
	for (long i = 0; i < n; i++) {
		// The accent input goes high accentDelay seconds after the trigger.
		bool accHigh = accent && i >= accSamples;
		if (i == 0 || i == retrig)
			v.trigger(accHigh);
		float y = v.process(c, accHigh, dt);
		if (!std::isfinite(y))
			s.finite = false;
		float a = std::fabs(y);
		if (a > s.peak)
			s.peak = a;
		if (i > n * 9 / 10 && a > s.tailPeak)
			s.tailPeak = a;
		s.sumSq += (double) y * y;
	}
	return s;
}

template <typename V>
static void testVoice(const char* name, float maxDecay) {
	const float rates[] = {44100.f, 48000.f, 96000.f, 192000.f};
	for (float sr : rates) {
		// Silence before any trigger.
		{
			V v;
			VoiceControls c;
			float dt = 1.f / sr;
			float peak = 0.f;
			for (int i = 0; i < (int) sr / 10; i++)
				peak = std::fmax(peak, std::fabs(v.process(c, false, dt)));
			CHECK(peak == 0.f, "%s @%g: output before trigger (%g)", name, sr, peak);
		}
		// Extreme control settings: always finite and bounded, decays to silence.
		const float corners[] = {0.f, 1.f};
		for (float wave : corners)
			for (float a : corners)
				for (float b : corners)
					for (float dec : corners)
						for (float tune : {-1.5f, 0.f, 1.5f}) {
							V v;
							VoiceControls c;
							c.tuneOct = tune;
							c.wave = wave;
							c.a = a;
							c.b = b;
							c.decay = dec;
							// Long enough for the envelope to fall far below -60 dB.
							float seconds = (dec > 0.5f ? maxDecay : 0.2f) * 2.5f;
							Stats s = run(v, c, sr, seconds, true);
							CHECK(s.finite, "%s @%g: non-finite output", name, sr);
							CHECK(s.peak < 4.f, "%s @%g: peak too high %g (wave %g a %g b %g dec %g tune %g)",
							      name, sr, s.peak, wave, a, b, dec, tune);
							CHECK(s.tailPeak < 1e-3f, "%s @%g: does not decay (tail %g, dec %g)", name, sr, s.tailPeak, dec);
						}
		// Accent is louder than a normal hit.
		{
			V v1, v2;
			VoiceControls c;
			Stats normal = run(v1, c, sr, 0.5f, false);
			Stats accented = run(v2, c, sr, 0.5f, true);
			CHECK(normal.peak > 0.1f, "%s @%g: normal hit too quiet (%g)", name, sr, normal.peak);
			CHECK(accented.peak > normal.peak * 1.5f, "%s @%g: accent not louder (%g vs %g)", name, sr, accented.peak, normal.peak);
		}
		// Accent arriving 0.5 ms after the trigger still counts; 5 ms later does not.
		{
			V v1, v2, v3;
			VoiceControls c;
			Stats on = run(v1, c, sr, 0.3f, true, -1.f, 0.f);
			Stats late = run(v2, c, sr, 0.3f, true, -1.f, 0.0005f);
			Stats tooLate = run(v3, c, sr, 0.3f, true, -1.f, 0.005f);
			CHECK(std::fabs(late.sumSq - on.sumSq) < on.sumSq * 0.05, "%s @%g: late accent ignored", name, sr);
			CHECK(tooLate.sumSq < on.sumSq * 0.5, "%s @%g: accent 5 ms after trigger was accepted", name, sr);
		}
		// Fast retriggering stays finite and bounded.
		{
			V v;
			VoiceControls c;
			c.decay = 1.f;
			Stats s = run(v, c, sr, 0.2f, true, 0.01f);
			CHECK(s.finite && s.peak < 4.f, "%s @%g: retrigger problem (peak %g)", name, sr, s.peak);
		}
	}
}

int main() {
	testVoice<KickVoice>("kick", KickVoice::DECAY_MAX * KickVoice::ACCENT_DECAY);
	testVoice<SnareVoice>("snare", SnareVoice::DECAY_MAX);
	testVoice<HiHatVoice>("hihat", HiHatVoice::DECAY_MAX * HiHatVoice::ACCENT_DECAY);

	// Wave morph stays within -1..1 (plus a small PolyBLEP overshoot) at a high frequency.
	{
		MorphOsc o;
		float peak = 0.f;
		for (int w = 0; w <= 30; w++)
			for (int i = 0; i < 48000; i++)
				peak = std::fmax(peak, std::fabs(o.process(5000.f, w / 30.f, 1.f / 48000.f)));
		CHECK(peak < 1.3f, "morph osc peak %g", peak);
	}

	if (failures == 0)
		std::printf("All tests passed.\n");
	else
		std::printf("%d test(s) failed.\n", failures);
	return failures ? 1 : 0;
}
