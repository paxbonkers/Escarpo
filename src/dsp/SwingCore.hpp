#pragma once
#include <cmath>

// Rack-independent timing core of Escarpo, so it can be unit-tested without Rack.
// All times are in seconds; the caller advances time by the sample duration (dt).
// Real-time safe: no allocation, no I/O, fixed-size state.

namespace escarpo {

struct SwingCore {
	static constexpr int MAX_BURST = 8;
	static constexpr double MAX_DELAY_FRACTION = 0.9;
	// A gap longer than this factor times the known period is treated as a clock restart,
	// not as a new tempo (otherwise the first pulse after a pause would be delayed for ages).
	static constexpr double RESTART_FACTOR = 4.0;
	static constexpr double FLUSH_PULSE_TIME = 1e-3;

	enum JobType { JOB_NONE, JOB_SINGLE, JOB_BURST };

	double now = 0.0;

	// Tempo measurement
	bool hasLastRise = false;
	double lastRise = 0.0;
	bool periodValid = false;
	double period = 0.0;

	// Current output job
	JobType job = JOB_NONE;
	double jobDelay = 0.0;
	double riseTime = 0.0;
	double fallTime = INFINITY;  // single: unknown until the input gate falls
	double burstInterval = 0.0;
	int burstCount = 0;
	// Time the next clock is due. Burst pulses after this point are dropped, so a stopped clock
	// behaves like a running one (where the next clock would have cut them off anyway).
	double burstEnd = INFINITY;

	// Short trigger for a pending pulse that was overtaken by a newer clock pulse
	double flushUntil = -1.0;

	bool lastOut = false;

	void reset() {
		hasLastRise = false;
		lastRise = 0.0;
		periodValid = false;
		period = 0.0;
		job = JOB_NONE;
		fallTime = INFINITY;
		burstCount = 0;
		burstEnd = INFINITY;
		flushUntil = -1.0;
		// `now` keeps running; `lastOut` is updated on the next process() call.
	}

	static double clampd(double x, double lo, double hi) {
		return x < lo ? lo : (x > hi ? hi : x);
	}

	/** Snaps a delay fraction to the nearest musical subdivision of the period:
	0, 1/6, 1/4, 1/3, 1/2, 2/3, 3/4. The input is clamped to 0..0.9 first.
	*/
	static double quantizeDelay(double fraction) {
		static const double steps[] = {0.0, 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75};
		static const int numSteps = sizeof(steps) / sizeof(steps[0]);
		double x = clampd(fraction, 0.0, MAX_DELAY_FRACTION);
		double best = steps[0];
		for (int i = 1; i < numSteps; i++) {
			if (std::fabs(steps[i] - x) < std::fabs(best - x))
				best = steps[i];
		}
		return best;
	}

	/** Advances one sample.
	rising/falling: clock edges detected in this sample.
	resetEvent: reset trigger detected in this sample.
	delayFraction: requested delay as fraction of the period (clamped to 0..0.9).
	burst: requested pulses per clock (clamped to 1..8).
	dt: sample duration in seconds.
	Returns the output gate state for this sample.
	*/
	bool process(bool rising, bool falling, bool resetEvent, double delayFraction, int burst, double dt) {
		if (resetEvent) {
			reset();
		}

		if (falling && job == JOB_SINGLE && std::isinf(fallTime)) {
			fallTime = now + jobDelay;
		}

		if (rising) {
			onRise(delayFraction, burst, dt);
		}

		bool out = jobOutput();
		if (now < flushUntil) {
			out = true;
		}

		now += dt;
		lastOut = out;
		return out;
	}

	void onRise(double delayFraction, int burst, double dt) {
		// 1. Measure tempo from the last two rising edges.
		if (hasLastRise) {
			double gap = now - lastRise;
			bool isRestart = periodValid && gap > RESTART_FACTOR * period;
			if (gap > 0.0 && !isRestart) {
				period = gap;
				periodValid = true;
			}
		}
		hasLastRise = true;
		lastRise = now;

		// 2. Finish the previous job so pulses never overtake each other.
		if (job == JOB_SINGLE && now < riseTime) {
			// Pending single pulse that has not started yet: emit it now as a short trigger.
			flushUntil = now + FLUSH_PULSE_TIME;
		}
		job = JOB_NONE;

		// 3. Schedule the new job.
		double d = 0.0;
		int n = 1;
		if (periodValid) {
			d = clampd(delayFraction, 0.0, MAX_DELAY_FRACTION) * period;
			n = burst < 1 ? 1 : (burst > MAX_BURST ? MAX_BURST : burst);
		}
		double start = now + d;
		// Guarantee at least one low sample before a new rising edge.
		if (lastOut && start < now + dt) {
			start = now + dt;
		}
		if (start < flushUntil + dt && flushUntil > now) {
			start = flushUntil + dt;
		}

		jobDelay = start - now;
		riseTime = start;
		if (n == 1) {
			job = JOB_SINGLE;
			fallTime = INFINITY;
		}
		else {
			job = JOB_BURST;
			burstCount = n;
			burstInterval = period / n;
			burstEnd = lastRise + period;
		}
	}

	bool jobOutput() {
		if (job == JOB_SINGLE) {
			if (now >= fallTime) {
				job = JOB_NONE;
				return false;
			}
			return now >= riseTime;
		}
		if (job == JOB_BURST) {
			double t = now - riseTime;
			if (t < 0.0) {
				return false;
			}
			double k = std::floor(t / burstInterval);
			// A pulse that starts after the next clock was due is dropped (clock stopped).
			if (k >= burstCount || riseTime + k * burstInterval >= burstEnd) {
				job = JOB_NONE;
				return false;
			}
			// Gate length = 50 % of the burst interval.
			return (t - k * burstInterval) < 0.5 * burstInterval;
		}
		return false;
	}
};

} // namespace escarpo
