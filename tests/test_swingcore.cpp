// Standalone tests for escarpo::SwingCore (no Rack needed).
// Build & run:  c++ -std=c++11 -O2 -I../src tests/test_swingcore.cpp -o build/test_swingcore && ./build/test_swingcore
#include "dsp/SwingCore.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

struct Result {
	std::vector<double> inRises, outRises, outFalls;
};

// Simulates a square clock (50 % duty) and returns edge times.
// delayFn(i) gives the delay fraction for pulse i, burstFn(i) the burst count.
template <typename DelayFn, typename BurstFn>
static Result run(double sampleRate, double period, int pulses, DelayFn delayFn, BurstFn burstFn,
                  double pauseAfterPulse = -1, double pauseLength = 0, int resetAtPulse = -1) {
	escarpo::SwingCore core;
	Result r;
	double dt = 1.0 / sampleRate;
	bool lastIn = false, lastOut = false;
	int pulseIndex = -1;
	double t = 0.0;
	double clockStart = 0.0;
	double total = pulses * period + 2 * period + (pauseLength > 0 ? pauseLength : 0);
	long n = (long) (total * sampleRate);
	for (long s = 0; s < n; s++) {
		double tc = t - clockStart;
		// Optional pause: shift the clock phase after a given pulse.
		if (pauseAfterPulse >= 0 && tc >= (pauseAfterPulse + 1) * period && clockStart == 0.0) {
			clockStart = pauseLength;
			tc = t - clockStart;
		}
		bool in = false;
		if (tc >= 0) {
			double phase = std::fmod(tc, period);
			int idx = (int) std::floor(tc / period);
			in = (idx < pulses) && phase < 0.5 * period;
		}
		bool rising = in && !lastIn;
		bool falling = !in && lastIn;
		if (rising) {
			pulseIndex++;
			r.inRises.push_back(t);
		}
		bool reset = rising && pulseIndex == resetAtPulse;
		int idx = pulseIndex < 0 ? 0 : pulseIndex;
		bool out = core.process(rising, falling, reset, delayFn(idx), burstFn(idx), dt);
		if (out && !lastOut) r.outRises.push_back(t);
		if (!out && lastOut) r.outFalls.push_back(t);
		lastIn = in;
		lastOut = out;
		t += dt;
	}
	return r;
}

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main() {
	const double P = 0.25;  // 240 BPM quarter / 120 BPM eighths

	// 1. No delay, no burst: output follows input.
	{
		Result r = run(48000, P, 16, [](int) { return 0.0; }, [](int) { return 1; });
		CHECK(r.outRises.size() == r.inRises.size(), "pass-through: %zu vs %zu", r.outRises.size(), r.inRises.size());
		for (size_t i = 0; i < r.outRises.size() && i < r.inRises.size(); i++)
			CHECK(near(r.outRises[i], r.inRises[i], 1.5 / 48000), "pass-through edge %zu", i);
	}

	// 2. Delay 50 % at several sample rates: first pulse undelayed, then +P/2, gate length kept.
	for (double sr : {44100.0, 48000.0, 96000.0}) {
		Result r = run(sr, P, 16, [](int) { return 0.5; }, [](int) { return 1; });
		CHECK(r.outRises.size() == r.inRises.size(), "delay count @%.0f: %zu vs %zu", sr, r.outRises.size(), r.inRises.size());
		CHECK(near(r.outRises[0], r.inRises[0], 1.5 / sr), "first pulse undelayed @%.0f", sr);
		for (size_t i = 1; i < r.outRises.size(); i++) {
			CHECK(near(r.outRises[i] - r.inRises[i], 0.5 * P, 2.0 / sr), "delay %zu @%.0f: %f", i, sr, r.outRises[i] - r.inRises[i]);
			CHECK(near(r.outFalls[i] - r.outRises[i], 0.5 * P, 2.0 / sr), "gate length %zu @%.0f", i, sr);
		}
	}

	// 3. Burst 4: four evenly spaced pulses per clock (after the tempo is known).
	{
		Result r = run(48000, P, 16, [](int) { return 0.0; }, [](int) { return 4; });
		// pulse 0 single (tempo unknown), pulses 1..15 bursts of 4
		CHECK(r.outRises.size() == 1 + 15 * 4, "burst count: %zu", r.outRises.size());
		for (size_t i = 2; i < 5 && i < r.outRises.size(); i++)
			CHECK(near(r.outRises[i] - r.outRises[i - 1], P / 4, 2.0 / 48000), "burst spacing %zu", i);
		if (r.outFalls.size() > 2)
			CHECK(near(r.outFalls[1] - r.outRises[1], P / 8, 2.0 / 48000), "burst gate = 50 %% of interval");
	}

	// 4. Delay 90 % + burst 4: remaining burst pulses are cut by the next clock; never more than 4 per clock.
	{
		Result r = run(48000, P, 16, [](int) { return 0.9; }, [](int) { return 4; });
		for (size_t i = 1; i < r.outRises.size(); i++)
			CHECK(r.outRises[i] > r.outRises[i - 1], "monotonic");
		CHECK(r.outRises.size() >= 16 && r.outRises.size() <= 1 + 15 * 4, "cut burst count: %zu", r.outRises.size());
	}

	// 5. Random per-pulse delay (LFO-like): no pulse is lost, order is kept.
	{
		std::srand(1);
		std::vector<double> delays(64);
		for (double& d : delays) d = (std::rand() % 1000) / 1000.0;
		Result r = run(48000, P, 64, [&](int i) { return delays[i]; }, [](int) { return 1; });
		CHECK(r.outRises.size() == r.inRises.size(), "random delay keeps all pulses: %zu vs %zu", r.outRises.size(), r.inRises.size());
		for (size_t i = 1; i < r.outRises.size(); i++)
			CHECK(r.outRises[i] > r.outRises[i - 1], "random delay order %zu", i);
	}

	// 6. Clock pause of 3 s: the first pulse after the pause uses the old period, not 3 s.
	{
		Result r = run(48000, P, 16, [](int) { return 0.5; }, [](int) { return 1; }, 7, 3.0);
		CHECK(r.outRises.size() == r.inRises.size(), "pause count");
		for (size_t i = 1; i < r.outRises.size(); i++)
			CHECK(r.outRises[i] - r.inRises[i] < 0.5 * P + 0.01, "pause: pulse %zu delayed %f s", i, r.outRises[i] - r.inRises[i]);
	}

	// 7. Reset at pulse 8: tempo is forgotten, that pulse passes undelayed, the next ones are delayed again.
	{
		Result r = run(48000, P, 16, [](int) { return 0.5; }, [](int) { return 1; }, -1, 0, 8);
		CHECK(near(r.outRises[8], r.inRises[8], 1.5 / 48000), "reset: pulse 8 undelayed");
		CHECK(near(r.outRises[9] - r.inRises[9], 0.5 * P, 2.0 / 48000), "reset: pulse 9 delayed again");
	}

	// 9. Clock stops: no burst pulses after the time the next clock was due.
	//    Running, a 90 % delay + burst 4 yields 1 pulse per clock (the rest is cut by the next clock);
	//    after the clock stops, the last burst must not suddenly play out in full.
	{
		Result r = run(48000, P, 16, [](int) { return 0.9; }, [](int) { return 4; });
		double lastIn = r.inRises.back();
		int tail = 0;
		for (double t : r.outRises) {
			if (t > lastIn) tail++;
			CHECK(t < lastIn + P, "stop: output pulse %f s after the last clock (due window %f s)", t - lastIn, P);
		}
		CHECK(tail == 1, "stop: expected 1 tail pulse, got %d", tail);
	}

	// 8. Delay quantization: nearest musical subdivision, input clamped first.
	{
		using C = escarpo::SwingCore;
		CHECK(C::quantizeDelay(0.0) == 0.0, "q 0");
		CHECK(C::quantizeDelay(0.05) == 0.0, "q 0.05 -> 0");
		CHECK(near(C::quantizeDelay(0.12), 1.0 / 6.0, 1e-12), "q 0.12 -> 1/6");
		CHECK(C::quantizeDelay(0.24) == 0.25, "q 0.24 -> 1/4");
		CHECK(near(C::quantizeDelay(0.30), 1.0 / 3.0, 1e-12), "q 0.30 -> 1/3");
		CHECK(C::quantizeDelay(0.45) == 0.5, "q 0.45 -> 1/2");
		CHECK(near(C::quantizeDelay(0.60), 2.0 / 3.0, 1e-12), "q 0.60 -> 2/3");
		CHECK(C::quantizeDelay(0.9) == 0.75, "q 0.9 -> 3/4");
		CHECK(C::quantizeDelay(5.0) == 0.75, "q above range");
		CHECK(C::quantizeDelay(-1.0) == 0.0, "q below range");
	}

	if (failures == 0) std::printf("All SwingCore tests passed.\n");
	return failures == 0 ? 0 : 1;
}
