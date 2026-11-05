#include "plugin.hpp"
#include "dsp/LillebrorVoices.hpp"

// Lillebror: three drum voices (kick, snare, hi-hat), the "little brother" of Mutable Instruments Grids.
// Patch the three trigger and three accent outputs of Grids 1:1 into Lillebror.
// The sound generation lives in dsp/LillebrorVoices.hpp (Rack-independent, unit-tested).

static const int NUM_VOICES = 3;
// Output level of a non-accented hit at full level; an accent adds +6 dB.
static const float VOICE_VOLTS = 5.f;
// The mix is attenuated so three simultaneous hits stay in a sensible range.
static const float MIX_GAIN = 0.6f;
// CV scaling for WAVE CV and DEC CV: 1 V = 10 % of the knob range.
static const float CV_SCALE = 0.1f;
// Fade time when muting / unmuting a voice.
static const float MUTE_RAMP_TIME = 0.005f;

struct Lillebror : Module {
	// IDs are stored in patches. Never reorder; only append new entries.
	// Layout per voice (kick, snare, hi-hat): TUNE, WAVE, A, B, DECAY.
	// A/B = kick: SWEEP/CLICK, snare: TONE/SNAP, hi-hat: COLOR/BRIGHT.
	enum ParamId {
		KICK_TUNE_PARAM,
		KICK_WAVE_PARAM,
		KICK_SWEEP_PARAM,
		KICK_CLICK_PARAM,
		KICK_DECAY_PARAM,
		SNARE_TUNE_PARAM,
		SNARE_WAVE_PARAM,
		SNARE_TONE_PARAM,
		SNARE_SNAP_PARAM,
		SNARE_DECAY_PARAM,
		HAT_TUNE_PARAM,
		HAT_WAVE_PARAM,
		HAT_COLOR_PARAM,
		HAT_BRIGHT_PARAM,
		HAT_DECAY_PARAM,
		// Appended in v0.2
		KICK_MUTE_PARAM,
		SNARE_MUTE_PARAM,
		HAT_MUTE_PARAM,
		PARAMS_LEN
	};
	// Layout per voice: TRIG, ACC, WAVE CV, DECAY CV.
	enum InputId {
		KICK_TRIG_INPUT,
		KICK_ACC_INPUT,
		KICK_WAVE_CV_INPUT,
		KICK_DECAY_CV_INPUT,
		SNARE_TRIG_INPUT,
		SNARE_ACC_INPUT,
		SNARE_WAVE_CV_INPUT,
		SNARE_DECAY_CV_INPUT,
		HAT_TRIG_INPUT,
		HAT_ACC_INPUT,
		HAT_WAVE_CV_INPUT,
		HAT_DECAY_CV_INPUT,
		// Appended in v0.2
		KICK_TUNE_CV_INPUT,
		SNARE_TUNE_CV_INPUT,
		HAT_TUNE_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		KICK_OUTPUT,
		SNARE_OUTPUT,
		HAT_OUTPUT,
		MIX_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		KICK_MUTE_LIGHT,
		SNARE_MUTE_LIGHT,
		HAT_MUTE_LIGHT,
		LIGHTS_LEN
	};

	static const int PARAMS_PER_VOICE = 5;
	static const int INPUTS_PER_VOICE = 4;
	enum { P_TUNE, P_WAVE, P_A, P_B, P_DECAY };
	enum { I_TRIG, I_ACC, I_WAVE_CV, I_DECAY_CV };

	lillebror::KickVoice kick;
	lillebror::SnareVoice snare;
	lillebror::HiHatVoice hat;
	dsp::SchmittTrigger trigTrigger[NUM_VOICES];
	dsp::SchmittTrigger accTrigger[NUM_VOICES];
	// Smoothed mute gain per voice (1 = audible), avoids clicks when muting during a hit.
	float muteGain[NUM_VOICES] = {1.f, 1.f, 1.f};

	Lillebror() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		// TUNE is stored in octaves relative to the base frequency and displayed in Hz.
		configParam(KICK_TUNE_PARAM, -1.5f, 1.5f, 0.f, "Kick tune", " Hz", 2.f, lillebror::KickVoice::BASE_FREQ);
		configParam(KICK_WAVE_PARAM, 0.f, 1.f, 0.f, "Kick wave (sine – triangle – saw – square)", "%", 0.f, 100.f);
		configParam(KICK_SWEEP_PARAM, 0.f, 1.f, 0.5f, "Kick pitch sweep", "%", 0.f, 100.f);
		configParam(KICK_CLICK_PARAM, 0.f, 1.f, 0.3f, "Kick click", "%", 0.f, 100.f);
		configParam(KICK_DECAY_PARAM, 0.f, 1.f, 0.4f, "Kick decay", "%", 0.f, 100.f);

		configParam(SNARE_TUNE_PARAM, -1.5f, 1.5f, 0.f, "Snare tune", " Hz", 2.f, lillebror::SnareVoice::BASE_FREQ);
		configParam(SNARE_WAVE_PARAM, 0.f, 1.f, 0.f, "Snare wave (sine – triangle – saw – square)", "%", 0.f, 100.f);
		configParam(SNARE_TONE_PARAM, 0.f, 1.f, 0.6f, "Snare tone (body – noise)", "%", 0.f, 100.f);
		configParam(SNARE_SNAP_PARAM, 0.f, 1.f, 0.5f, "Snare snap", "%", 0.f, 100.f);
		configParam(SNARE_DECAY_PARAM, 0.f, 1.f, 0.35f, "Snare decay", "%", 0.f, 100.f);

		configParam(HAT_TUNE_PARAM, -1.5f, 1.5f, 0.f, "Hi-hat tune", " Hz", 2.f, lillebror::HiHatVoice::BASE_FREQ);
		configParam(HAT_WAVE_PARAM, 0.f, 1.f, 1.f, "Hi-hat wave (sine – triangle – saw – square)", "%", 0.f, 100.f);
		configParam(HAT_COLOR_PARAM, 0.f, 1.f, 0.3f, "Hi-hat color (metal – noise)", "%", 0.f, 100.f);
		configParam(HAT_BRIGHT_PARAM, 0.f, 1.f, 0.5f, "Hi-hat brightness", "%", 0.f, 100.f);
		configParam(HAT_DECAY_PARAM, 0.f, 1.f, 0.15f, "Hi-hat decay", "%", 0.f, 100.f);

		const char* names[NUM_VOICES] = {"Kick", "Snare", "Hi-hat"};
		for (int v = 0; v < NUM_VOICES; v++) {
			std::string n = names[v];
			configInput(v * INPUTS_PER_VOICE + I_TRIG, n + " trigger");
			configInput(v * INPUTS_PER_VOICE + I_ACC, n + " accent");
			configInput(v * INPUTS_PER_VOICE + I_WAVE_CV, n + " wave CV (1 V = 10 %)");
			configInput(v * INPUTS_PER_VOICE + I_DECAY_CV, n + " decay CV (1 V = 10 %)");
			configOutput(KICK_OUTPUT + v, n);
			configInput(KICK_TUNE_CV_INPUT + v, n + " tune CV (1 V/oct)");
			configSwitch(KICK_MUTE_PARAM + v, 0.f, 1.f, 0.f, n + " mute", {"Off", "On"});
			configLight(KICK_MUTE_LIGHT + v, n + " muted");
		}
		configOutput(MIX_OUTPUT, "Mix");
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		kick.reset();
		snare.reset();
		hat.reset();
		for (int v = 0; v < NUM_VOICES; v++) {
			trigTrigger[v].reset();
			accTrigger[v].reset();
			muteGain[v] = 1.f;
		}
	}

	// Reads the knob value plus CV (1 V = 10 %), clamped to 0..1. Non-finite CV is ignored.
	float knobWithCv(int paramId, int inputId) {
		float cv = lillebror::finiteOr(inputs[inputId].getVoltage(), 0.f);
		return lillebror::clampf(params[paramId].getValue() + cv * CV_SCALE, 0.f, 1.f);
	}

	lillebror::VoiceControls readControls(int v) {
		int p = v * PARAMS_PER_VOICE;
		int i = v * INPUTS_PER_VOICE;
		lillebror::VoiceControls c;
		// TUNE CV: 1 V/oct, added to the knob. The total is limited to +-3 octaves.
		float tuneCv = lillebror::finiteOr(inputs[KICK_TUNE_CV_INPUT + v].getVoltage(), 0.f);
		c.tuneOct = lillebror::clampf(params[p + P_TUNE].getValue() + tuneCv, -3.f, 3.f);
		c.wave = knobWithCv(p + P_WAVE, i + I_WAVE_CV);
		c.a = params[p + P_A].getValue();
		c.b = params[p + P_B].getValue();
		c.decay = knobWithCv(p + P_DECAY, i + I_DECAY_CV);
		return c;
	}

	void process(const ProcessArgs& args) override {
		float y[NUM_VOICES];
		for (int v = 0; v < NUM_VOICES; v++) {
			int i = v * INPUTS_PER_VOICE;
			// Trigger: rising edge with hysteresis (high at >= 1 V, low at <= 0.1 V).
			bool trig = trigTrigger[v].process(inputs[i + I_TRIG].getVoltage(), 0.1f, 1.f);
			accTrigger[v].process(inputs[i + I_ACC].getVoltage(), 0.1f, 1.f);
			bool accHigh = accTrigger[v].isHigh();
			lillebror::VoiceControls c = readControls(v);
			float out = 0.f;
			switch (v) {
				case 0:
					if (trig)
						kick.trigger(accHigh);
					out = kick.process(c, accHigh, args.sampleTime);
					break;
				case 1:
					if (trig)
						snare.trigger(accHigh);
					out = snare.process(c, accHigh, args.sampleTime);
					break;
				default:
					if (trig)
						hat.trigger(accHigh);
					out = hat.process(c, accHigh, args.sampleTime);
					break;
			}
			// Mute: the voice keeps running, only its output is faded out (5 ms ramp).
			bool muted = params[KICK_MUTE_PARAM + v].getValue() > 0.5f;
			float target = muted ? 0.f : 1.f;
			float step = args.sampleTime / MUTE_RAMP_TIME;
			muteGain[v] += lillebror::clampf(target - muteGain[v], -step, step);
			lights[KICK_MUTE_LIGHT + v].setBrightness(muted ? 1.f : 0.f);
			y[v] = out * VOICE_VOLTS * muteGain[v];
			outputs[KICK_OUTPUT + v].setVoltage(y[v]);
		}
		outputs[MIX_OUTPUT].setVoltage((y[0] + y[1] + y[2]) * MIX_GAIN);
	}
};


struct LillebrorWidget : ModuleWidget {
	LillebrorWidget(Lillebror* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Lillebror.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		// The footer is dark, so the bottom screws are black.
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Positions in px (1 HP = 15 px), see docs/modules/Lillebror-Panel-Bauplan.md.
		// Each voice column is 80 px wide; left axis at x0 + 20, right axis at x0 + 60.
		// Row centres (y) of the six rows per voice column.
		const float ROW1 = 74.f, ROW2 = 120.f, ROW3 = 166.f, ROW4 = 212.f, ROW5 = 258.f, ROW6 = 302.f;
		for (int v = 0; v < 3; v++) {
			float x0 = 80.f * v;
			float xl = x0 + 20.f, xr = x0 + 60.f;
			int p = v * Lillebror::PARAMS_PER_VOICE;
			int i = v * Lillebror::INPUTS_PER_VOICE;
			// TUNE is left-aligned with the knobs below it, so its centre sits 4 px right of the axis.
			addParam(createParamCentered<RoundBlackKnob>(Vec(xl + 4.f, ROW1), module, p + Lillebror::P_TUNE));
			addParam(createParamCentered<RoundSmallBlackKnob>(Vec(xr, ROW1), module, p + Lillebror::P_WAVE));
			addParam(createParamCentered<RoundSmallBlackKnob>(Vec(xl, ROW2), module, p + Lillebror::P_A));
			addParam(createParamCentered<RoundSmallBlackKnob>(Vec(xr, ROW2), module, p + Lillebror::P_B));
			addParam(createParamCentered<RoundSmallBlackKnob>(Vec(xl, ROW3), module, p + Lillebror::P_DECAY));
			addInput(createInputCentered<PJ301MPort>(Vec(xr, ROW3), module, i + Lillebror::I_DECAY_CV));

			addInput(createInputCentered<PJ301MPort>(Vec(xl, ROW4), module, Lillebror::KICK_TUNE_CV_INPUT + v));
			addInput(createInputCentered<PJ301MPort>(Vec(xr, ROW4), module, i + Lillebror::I_WAVE_CV));

			addInput(createInputCentered<PJ301MPort>(Vec(xl, ROW5), module, i + Lillebror::I_TRIG));
			addInput(createInputCentered<PJ301MPort>(Vec(xr, ROW5), module, i + Lillebror::I_ACC));

			addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
				Vec(xl, ROW6), module, Lillebror::KICK_MUTE_PARAM + v, Lillebror::KICK_MUTE_LIGHT + v));
			addOutput(createOutputCentered<PJ301MPort>(Vec(xr, ROW6), module, Lillebror::KICK_OUTPUT + v));
		}
		addOutput(createOutputCentered<PJ301MPort>(Vec(220, 347.5f), module, Lillebror::MIX_OUTPUT));
	}
};


Model* modelLillebror = createModel<Lillebror, LillebrorWidget>("Lillebror");
