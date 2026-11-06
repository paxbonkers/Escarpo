// Standalone tests for the Zenon sequencer logic (no Rack needed).
// Build & run:  c++ -std=c++11 -O2 -Wall -Isrc tests/test_zenon_sequencer.cpp -o build/test_zenon_sequencer && ./build/test_zenon_sequencer
#include "dsp/ZenonSequencer.hpp"
#include <cstdio>
#include <cmath>
#include <limits>
#include <algorithm>

using namespace zenon;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

static bool near(float a, float b, float eps = 1e-5f) {
	return std::fabs(a - b) <= eps;
}

static void testPolymetry() {
	// 5 against 7 steps: both are on step 1 together again after 35 steps, not before.
	Track a, b;
	int firstCoincidence = -1;
	for (int i = 0; i < 100; i++) {
		a.advance(5);
		b.advance(7);
		if (i > 0 && a.pos == 0 && b.pos == 0) {
			firstCoincidence = i;
			break;
		}
	}
	CHECK(firstCoincidence == 35, "5 vs 7 should realign after 35 steps, got %d", firstCoincidence);
}

static void testFirstStepAndReset() {
	Track t;
	CHECK(t.current() == 0, "before the first step the display shows step 1");
	t.advance(8);
	CHECK(t.pos == 0, "first advance lands on step 1, got %d", t.pos);
	for (int i = 0; i < 4; i++)
		t.advance(8);
	CHECK(t.pos == 4, "expected step 5, got %d", t.pos + 1);
	t.reset();
	t.advance(8);
	CHECK(t.pos == 0, "after reset the next step is step 1, got %d", t.pos + 1);
}

static void testLengthChange() {
	Track t;
	for (int i = 0; i < 7; i++)
		t.advance(8);
	CHECK(t.pos == 6, "expected step 7, got %d", t.pos + 1);
	t.advance(4);
	CHECK(t.pos == 0, "shortening below the position continues at step 1, got %d", t.pos + 1);
	t.advance(1);
	CHECK(t.pos == 0, "length 1 always stays on step 1");
	t.advance(1);
	CHECK(t.pos == 0, "length 1 always stays on step 1");
}

static void testLengthFrom() {
	CHECK(lengthFrom(8.f, 0.f) == 8, "knob 8");
	CHECK(lengthFrom(5.f, 2.4f) == 7, "5 + 2.4 V rounds to 7");
	CHECK(lengthFrom(5.f, 2.6f) == 8, "5 + 2.6 V rounds to 8");
	CHECK(lengthFrom(5.f, 10.f) == 8, "clamped to 8");
	CHECK(lengthFrom(5.f, -10.f) == 1, "clamped to 1");
	CHECK(lengthFrom(3.f, std::numeric_limits<float>::quiet_NaN()) == 3, "NaN CV ignored");
	CHECK(lengthFrom(3.f, std::numeric_limits<float>::infinity()) == 3, "Inf CV ignored");
	CHECK(destFrom(-1.f) == 0 && destFrom(99.f) == NUM_DESTS - 1 && destFrom(2.4f) == 2, "destFrom clamps and rounds");
}

static void testMods() {
	{
		// Same destination on both tracks: contributions add up.
		int d[2] = {DEST_PITCH, DEST_PITCH};
		float s[2] = {0.5f, 0.25f}, a[2] = {1.f, 1.f};
		Mods m = computeMods(d, s, a);
		CHECK(near(m.pitchOct, 0.75f * PITCH_RANGE_OCT), "pitch sum, got %f", m.pitchOct);
		CHECK(near(m.velGain, 1.f), "no VEL destination -> full level");
	}
	{
		// Negative amount inverts the direction.
		int d[2] = {DEST_CUTOFF, DEST_FM};
		float s[2] = {1.f, 0.5f}, a[2] = {-0.5f, 1.f};
		Mods m = computeMods(d, s, a);
		CHECK(near(m.cutoffOct, -0.5f * CUTOFF_RANGE_OCT), "cutoff, got %f", m.cutoffOct);
		CHECK(near(m.fm, 0.5f), "fm, got %f", m.fm);
	}
	{
		// VEL at +100 %: level follows the step value.
		int d[2] = {DEST_VEL, DEST_PITCH};
		float s[2] = {0.3f, 0.f}, a[2] = {1.f, 0.f};
		CHECK(near(computeMods(d, s, a).velGain, 0.3f), "VEL +100 %%");
		// VEL at -100 %: a high step is quiet.
		a[0] = -1.f;
		s[0] = 0.8f;
		CHECK(near(computeMods(d, s, a).velGain, 0.2f), "VEL -100 %%");
		// VEL at 0 %: constant full level.
		a[0] = 0.f;
		CHECK(near(computeMods(d, s, a).velGain, 1.f), "VEL 0 %%");
	}
	{
		// Both tracks on VEL: contributions add up, result clamped to 0..1.
		int d[2] = {DEST_VEL, DEST_VEL};
		float s[2] = {0.f, 0.f}, a[2] = {1.f, 1.f};
		CHECK(near(computeMods(d, s, a).velGain, 0.f), "VEL sum clamped to 0");
		float s2[2] = {0.5f, 1.f};
		CHECK(near(computeMods(d, s2, a).velGain, 0.5f), "VEL sum 1 - 0.5 + 0");
	}
	{
		// Non-finite values never propagate.
		int d[2] = {DEST_AMP_DEC, DEST_FILTER_DEC};
		float nan = std::numeric_limits<float>::quiet_NaN();
		float s[2] = {nan, 1.f}, a[2] = {1.f, nan};
		Mods m = computeMods(d, s, a);
		CHECK(std::isfinite(m.ampDec) && std::isfinite(m.filterDec), "NaN handled");
	}
}

static void testClockRate() {
	// 480 steps/min = 8 steps/s: 80 ticks in 10 s at every sample rate.
	const float rates[] = {44100.f, 48000.f, 96000.f, 192000.f};
	for (float sr : rates) {
		InternalClock c;
		long n = (long) (10.f * sr);
		int ticks = 0;
		for (long i = 0; i < n; i++)
			ticks += c.process(480.f, 1.f / sr);
		CHECK(ticks >= 79 && ticks <= 80, "%.0f Hz: expected 80 ticks, got %d", sr, ticks);
	}
	CHECK(near(stepsPerMinute(std::log2(480.f), 0.f), 480.f, 0.01f), "tempo knob");
	CHECK(near(stepsPerMinute(std::log2(480.f), 1.f), 960.f, 0.05f), "1 V doubles the tempo");
	CHECK(std::isfinite(stepsPerMinute(std::log2(480.f), std::numeric_limits<float>::quiet_NaN())), "NaN tempo CV");
}

static void testTransport() {
	const float dt = 1.f / 48000.f;
	{
		// Stopped: internal clock and external clock do not step, ADV does.
		Transport t;
		int ticks = 0;
		for (int i = 0; i < 48000; i++)
			ticks += t.process(false, false, false, 480.f, dt);
		CHECK(ticks == 0, "stopped internal clock must not tick, got %d", ticks);
		CHECK(!t.process(true, true, false, 480.f, dt), "external clock ignored while stopped");
		CHECK(t.process(true, false, true, 480.f, dt), "ADV steps while stopped");
	}
	{
		// Starting plays the first step immediately.
		Transport t;
		t.setRunning(true);
		CHECK(t.process(false, false, false, 480.f, dt), "first step on start");
		CHECK(!t.process(false, false, false, 480.f, dt), "no double tick after start");
	}
	{
		// Running with external clock: steps on clock edges only, internal clock is off.
		Transport t;
		t.setRunning(true);
		int ticks = 0;
		for (int i = 0; i < 48000; i++)
			ticks += t.process(true, i % 4800 == 0, false, 480.f, dt);
		CHECK(ticks == 10, "10 clock edges -> 10 steps, got %d", ticks);
	}
	{
		// Reset restarts the clock phase: next tick one full period later.
		Transport t;
		t.setRunning(true);
		t.process(false, false, false, 480.f, dt);
		for (int i = 0; i < 3000; i++)
			t.process(false, false, false, 480.f, dt);
		t.reset();
		int firstTick = -1;
		for (int i = 0; i < 12000 && firstTick < 0; i++)
			if (t.process(false, false, false, 480.f, dt))
				firstTick = i;
		CHECK(firstTick >= 5990 && firstTick <= 6000, "after reset next tick after one period (6000 samples), got %d", firstTick);
	}
}


static int countTicks(Transport& t, int factor, int edgeEvery, int samples, float dt) {
	int ticks = 0;
	for (int i = 0; i < samples; i++)
		ticks += t.process(true, i % edgeEvery == 0, false, 480.f, dt, factor);
	return ticks;
}

static void testClockFactor() {
	const float dt = 1.f / 48000.f;
	// Knob zones: minimum = /4, centre = x1, maximum = x4. TEMPO CV adds one step per volt.
	float lo = std::log2(TEMPO_MIN_SPM), hi = std::log2(TEMPO_MAX_SPM);
	CHECK(clockFactorIndex(lo, 0.f) == -2, "knob min -> /4");
	CHECK(clockFactorIndex(0.5f * (lo + hi), 0.f) == 0, "knob centre -> x1");
	CHECK(clockFactorIndex(hi, 0.f) == 2, "knob max -> x4");
	CHECK(clockFactorIndex(0.5f * (lo + hi), 1.f) == 1, "+1 V -> x2");
	CHECK(clockFactorIndex(0.5f * (lo + hi), -9.f) == -2, "clamped to /4");
	CHECK(clockFactorIndex(0.5f * (lo + hi), std::numeric_limits<float>::quiet_NaN()) == 0, "NaN CV ignored");
	{
		// /2 and /4: 10 edges -> 5 and 3 steps (the first edge always steps).
		Transport t;
		t.setRunning(true);
		int ticks = countTicks(t, -1, 4800, 48000, dt);
		CHECK(ticks == 5, "/2: expected 5 steps, got %d", ticks);
		Transport u;
		u.setRunning(true);
		ticks = countTicks(u, -2, 4800, 48000, dt);
		CHECK(ticks == 3, "/4: expected 3 steps, got %d", ticks);
	}
	{
		// x2 and x4: after the first period is known, every edge gives 2 or 4 evenly spaced steps.
		// Edges at 0, 4800, ..., 43200 (10 edges): first edge alone, then 9 periods.
		Transport t;
		t.setRunning(true);
		int ticks = countTicks(t, 1, 4800, 48000, dt);
		CHECK(ticks == 1 + 9 + 9, "x2: expected 19 steps, got %d", ticks);
		Transport u;
		u.setRunning(true);
		ticks = countTicks(u, 2, 4800, 48000, dt);
		CHECK(ticks == 1 + 9 + 9 * 3, "x4: expected 37 steps, got %d", ticks);
	}
	{
		// x4 spacing: extra steps 1200 samples apart.
		Transport t;
		t.setRunning(true);
		int last = -1, minGap = 1 << 30, maxGap = 0;
		for (int i = 0; i < 48000; i++) {
			if (t.process(true, i % 4800 == 0, false, 480.f, dt, 2)) {
				if (last >= 0 && i > 9600) {
					minGap = std::min(minGap, i - last);
					maxGap = std::max(maxGap, i - last);
				}
				last = i;
			}
		}
		CHECK(minGap >= 1199 && maxGap <= 1201, "x4 spacing %d..%d, expected 1200", minGap, maxGap);
	}
	{
		// Stopped: external clock does not step in any factor; reset restarts the divider count.
		Transport t;
		int ticks = countTicks(t, 2, 4800, 48000, dt);
		CHECK(ticks == 0, "stopped: expected 0 steps, got %d", ticks);
		t.setRunning(true);
		CHECK(t.process(true, true, false, 480.f, dt, -1), "first edge after start steps (/2)");
		CHECK(!t.process(true, true, false, 480.f, dt, -1), "second edge skipped (/2)");
		t.reset();
		CHECK(t.process(true, true, false, 480.f, dt, -1), "after reset the next edge steps (/2)");
	}
}

static void testEndOfCycle() {
	// Length 5: EOC on advances 6, 11, 16 (when the track returns to step 1), not on the first step.
	Track t;
	int fired[20] = {0};
	for (int i = 1; i <= 16; i++)
		fired[i] = t.advance(5);
	for (int i = 1; i <= 16; i++) {
		bool expected = (i == 6 || i == 11 || i == 16);
		CHECK(fired[i] == expected, "length 5: advance %d EOC %d, expected %d", i, fired[i], expected);
	}
	// Length 1: every step after the first is the end of a cycle.
	Track u;
	CHECK(!u.advance(1), "length 1: first step is not EOC");
	CHECK(u.advance(1) && u.advance(1), "length 1: EOC on every following step");
	// Shortening below the position ends the cycle.
	Track v;
	for (int i = 0; i < 7; i++)
		v.advance(8);
	CHECK(v.advance(4), "cut by a shorter length counts as EOC");
	// Reset: the next step is step 1 but not an EOC.
	v.advance(4);
	v.reset();
	CHECK(!v.advance(4), "first step after reset is not EOC");
}

int main() {
	testPolymetry();
	testFirstStepAndReset();
	testLengthChange();
	testLengthFrom();
	testMods();
	testClockRate();
	testTransport();
	testClockFactor();
	testEndOfCycle();
	if (failures == 0)
		std::printf("All Zenon sequencer tests passed.\n");
	return failures == 0 ? 0 : 1;
}
