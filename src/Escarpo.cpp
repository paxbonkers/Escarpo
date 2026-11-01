#include "plugin.hpp"
#include "dsp/SwingCore.hpp"
#include <atomic>

// Escarpo: clock swing / drift / burst.
// Every clock pulse is delayed by a fraction of the measured clock period (CV-modulatable),
// and can optionally be turned into a burst of up to 8 evenly spaced pulses.
// The timing logic lives in dsp/SwingCore.hpp (Rack-independent, unit-tested).

struct Escarpo : Module {
	// IDs are stored in patches. Never reorder; only append new entries.
	enum ParamId {
		DELAY_PARAM,
		DELAY_CV_ATTEN_PARAM,
		BURST_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		RESET_INPUT,
		DELAY_CV_INPUT,
		BURST_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		THRU_OUTPUT,
		OUT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		OUT_LIGHT,
		LIGHTS_LEN
	};

	Escarpo() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		// Delay as a fraction of the measured clock period (0..0.9), displayed in percent.
		configParam(DELAY_PARAM, 0.f, 0.9f, 0.f, "Delay", "%", 0.f, 100.f);
		// Attenuverter for the delay CV, displayed in percent.
		configParam(DELAY_CV_ATTEN_PARAM, -1.f, 1.f, 0.f, "Delay CV amount", "%", 0.f, 100.f);
		// Pulses per input pulse, 1 = burst off.
		ParamQuantity* burstQ = configParam(BURST_PARAM, 1.f, 8.f, 1.f, "Burst", " pulses");
		burstQ->snapEnabled = true;

		configInput(CLOCK_INPUT, "Clock");
		configInput(RESET_INPUT, "Reset");
		configInput(DELAY_CV_INPUT, "Delay CV (1 V = 10 % of period)");
		configInput(BURST_CV_INPUT, "Burst CV (1 V = 1 step)");

		configOutput(THRU_OUTPUT, "Clock thru");
		configOutput(OUT_OUTPUT, "Swung clock");

		configLight(OUT_LIGHT, "Output");

		// When bypassed, both outputs carry the unmodified clock.
		configBypass(CLOCK_INPUT, THRU_OUTPUT);
		configBypass(CLOCK_INPUT, OUT_OUTPUT);
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		quantizeDelay = false;
		core.reset();
		clockTrigger.reset();
		resetTrigger.reset();
	}

	void process(const ProcessArgs& args) override {
		float clock = inputs[CLOCK_INPUT].getVoltage();

		// Edge detection with hysteresis: high at >= 1 V, low at <= 0.1 V.
		dsp::SchmittTrigger::Event ev = clockTrigger.processEvent(clock, 0.1f, 1.f);
		bool rising = (ev == dsp::SchmittTrigger::TRIGGERED);
		bool falling = (ev == dsp::SchmittTrigger::UNTRIGGERED);
		bool reset = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);

		// Delay: knob + CV * attenuverter, 1 V = 10 % of the period. Clamped to 0..90 % in the core.
		float delay = params[DELAY_PARAM].getValue()
			+ inputs[DELAY_CV_INPUT].getVoltage() * params[DELAY_CV_ATTEN_PARAM].getValue() * 0.1f;
		// Burst: knob + CV, 1 V = 1 step, rounded. Clamped to 1..8 in the core.
		float burstF = params[BURST_PARAM].getValue() + inputs[BURST_CV_INPUT].getVoltage();
		// Guard against NaN/Inf from misbehaving upstream modules before converting to int.
		if (!std::isfinite(delay))
			delay = 0.f;
		if (!std::isfinite(burstF))
			burstF = 1.f;
		// Optional: snap the total delay (knob + CV) to musical subdivisions of the period.
		if (quantizeDelay)
			delay = (float) escarpo::SwingCore::quantizeDelay(delay);
		int burst = (int) std::round(clamp(burstF, 1.f, 8.f));

		// Both values are only sampled by the core when a new clock pulse arrives.
		bool gate = core.process(rising, falling, reset, delay, burst, args.sampleTime);

		outputs[THRU_OUTPUT].setVoltage(clock);
		outputs[OUT_OUTPUT].setVoltage(gate ? 10.f : 0.f);

		lights[OUT_LIGHT].setBrightnessSmooth(gate ? 1.f : 0.f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "quantizeDelay", json_boolean(quantizeDelay));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		// Missing or invalid values fall back to the default (off).
		json_t* quantizeJ = json_object_get(rootJ, "quantizeDelay");
		quantizeDelay = quantizeJ && json_is_true(quantizeJ);
	}

	// Written by the UI thread (context menu), read by the audio thread.
	std::atomic<bool> quantizeDelay{false};

	escarpo::SwingCore core;
	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger resetTrigger;
};


struct EscarpoWidget : ModuleWidget {
	EscarpoWidget(Escarpo* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Escarpo.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Positions in px (1 HP = 15 px), see docs/modules/Swing-Panel-Bauplan.md.
		addParam(createParamCentered<RoundLargeBlackKnob>(Vec(60, 72), module, Escarpo::DELAY_PARAM));
		addParam(createParamCentered<Trimpot>(Vec(30, 122), module, Escarpo::DELAY_CV_ATTEN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(Vec(30, 178), module, Escarpo::BURST_PARAM));

		addInput(createInputCentered<PJ301MPort>(Vec(90, 122), module, Escarpo::DELAY_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(90, 178), module, Escarpo::BURST_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(30, 248), module, Escarpo::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(90, 248), module, Escarpo::RESET_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(Vec(30, 308), module, Escarpo::THRU_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(90, 308), module, Escarpo::OUT_OUTPUT));

		addChild(createLightCentered<SmallLight<GreenLight>>(Vec(106, 289), module, Escarpo::OUT_LIGHT));
	}

	void appendContextMenu(Menu* menu) override {
		Escarpo* module = getModule<Escarpo>();
		if (!module)
			return;

		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolMenuItem("Quantize delay", "1/6 1/4 1/3 1/2 2/3 3/4",
			[=]() { return module->quantizeDelay.load(); },
			[=](bool value) { module->quantizeDelay = value; }
		));
	}
};


Model* modelEscarpo = createModel<Escarpo, EscarpoWidget>("Escarpo");
