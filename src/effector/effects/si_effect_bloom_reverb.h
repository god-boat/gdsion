/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_BLOOM_REVERB_H
#define SI_EFFECT_BLOOM_REVERB_H

#include <godot_cpp/templates/vector.hpp>
#include "dsp/fractional_delay.h"
#include "dsp/lfo.h"
#include "dsp/one_pole.h"
#include "effector/si_effect_base.h"

using namespace godot;

class SiEffectBloomReverb : public SiEffectBase {
	GDCLASS(SiEffectBloomReverb, SiEffectBase)

public:
	enum ArgIndex {
		ARG_SIZE = 0,
		ARG_DECAY,
		ARG_BLOOM,
		ARG_TONE,
		ARG_AIR,
		ARG_MOTION,
		ARG_WIDTH,
		ARG_MIX,
		ARG_PREDELAY_MS,
		ARG_DUCK,
		ARG_LOW_CUT_HZ,
		ARG_HIGH_CUT_HZ,
		ARG_FREEZE,
		ARG_COUNT,
	};

private:
	static const int TANK_LINE_COUNT = 4;
	static const int INPUT_DIFFUSER_COUNT = 2;
	static const int EARLY_TAP_COUNT = 4;
	static const int AIR_DELAY_COUNT = 2;
	static const int PARAM_UPDATE_INTERVAL = 16;

	struct BloomParams {
		double size = 0.56;
		double decay = 0.48;
		double bloom = 0.32;
		double tone = 0.60;
		double air = 0.30;
		double motion = 0.28;
		double width = 0.72;
		double mix = 0.28;
		double predelay_ms = 22.0;
		double duck = 0.0;
		double low_cut_hz = 150.0;
		double high_cut_hz = 11000.0;
		double freeze = 0.0;
	};

	struct BloomEnvelope {
		double fast_attack_coeff = 1.0;
		double fast_release_coeff = 1.0;
		double slow_attack_coeff = 1.0;
		double slow_release_coeff = 1.0;
		double fast_env = 0.0;
		double slow_env = 0.0;

		void set_attack_ms(double p_ms, double p_sample_rate);
		double process(double p_input);
		void reset();
	};

	struct AllpassStage {
		sion::dsp::FractionalDelay delay;

		double process(double p_input, double p_delay_samples, double p_feedback);
	};

	struct TankLine {
		sion::dsp::FractionalDelay delay;
		sion::dsp::OnePole hp;
		sion::dsp::OnePole lp;
		sion::dsp::Lfo mod;
	};

	struct AirSide {
		sion::dsp::FractionalDelay delays[AIR_DELAY_COUNT];
		sion::dsp::Lfo mods[AIR_DELAY_COUNT];
		AllpassStage diffuser_a;
		AllpassStage diffuser_b;
		sion::dsp::OnePole hp;

		void reset();
	};

	struct DerivedParams {
		double dry_gain = 1.0;
		double wet_gain = 0.0;
		double predelay_samples = 0.0;
		double bloom_attack_ms = 1.0;
		double direct_late_amount = 0.9;
		double bloom_amount = 0.1;
		double diffusion_feedback = 0.45;
		double early_gain = 0.8;
		double late_gain = 0.85;
		double drive = 1.0;
		double size_scale = 1.0;
		double decay_seconds = 1.0;
		double input_hp_coeff = 0.0;
		double feedback_hp_coeff = 0.0;
		double damping_lp_coeff = 0.0;
		double wet_lp_coeff = 0.0;
		double mod_depth_samples = 0.0;
		double width_gain = 1.0;
		double air_gain = 0.0;
		double air_hp_coeff = 0.0;
		double wet_makeup_gain = 1.0;
		double duck = 0.0;
		double duck_attack_coeff = 1.0;
		double duck_release_coeff = 1.0;
		double freeze = 0.0;
		double tank_delay_samples[TANK_LINE_COUNT] = {};
		double tank_feedback_gain[TANK_LINE_COUNT] = {};
		double tank_mod_rates[TANK_LINE_COUNT] = {};
		double early_tap_left_samples[EARLY_TAP_COUNT] = {};
		double early_tap_right_samples[EARLY_TAP_COUNT] = {};
		double diffuser_left_samples[INPUT_DIFFUSER_COUNT] = {};
		double diffuser_right_samples[INPUT_DIFFUSER_COUNT] = {};
		double air_delay_left_samples[AIR_DELAY_COUNT] = {};
		double air_delay_right_samples[AIR_DELAY_COUNT] = {};
		double air_mod_rates[AIR_DELAY_COUNT] = {};
	};

	BloomParams _current_params;
	BloomParams _target_params;
	BloomParams _block_steps;

	sion::dsp::FractionalDelay _predelay_left;
	sion::dsp::FractionalDelay _predelay_right;
	sion::dsp::FractionalDelay _early_left;
	sion::dsp::FractionalDelay _early_right;

	AllpassStage _diffusers_left[INPUT_DIFFUSER_COUNT];
	AllpassStage _diffusers_right[INPUT_DIFFUSER_COUNT];
	TankLine _tank[TANK_LINE_COUNT];
	AirSide _air_left;
	AirSide _air_right;

	sion::dsp::OnePole _input_hp_left;
	sion::dsp::OnePole _input_hp_right;
	sion::dsp::OnePole _wet_lp_left;
	sion::dsp::OnePole _wet_lp_right;
	BloomEnvelope _bloom_envelope_left;
	BloomEnvelope _bloom_envelope_right;

	double _duck_env = 0.0;
	double _cached_sample_rate = 0.0;

	void _ensure_delay_buffers();
	void _reset_signal_state();
	void _derive_params(const BloomParams &p_params, DerivedParams &r_out) const;
	void _begin_block_smoothing(int p_length);
	void _advance_smoothed_params(int p_segment_length);
	void _set_arg_value(int p_arg_index, double p_value, bool p_snap_current);
	double _process_air_side(
			AirSide &r_side,
			double p_input,
			double p_hp_coeff,
			const double *p_delay_samples,
			double p_mod_depth_samples
	);

	static double _clamp01(double p_value);
	static double _exp_lerp(double p_min, double p_max, double p_t);
	static double _softclip(double p_input);
	static void _copy_params(BloomParams &r_dst, const BloomParams &p_src);

protected:
	static void _bind_methods();

public:
	virtual int prepare_process() override;
	virtual int process(const ProcessContext &p_context, int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual bool set_arg(int p_arg_index, double p_value) override;
	virtual void reset() override;

	SiEffectBloomReverb();
	~SiEffectBloomReverb() {}
};

#endif // SI_EFFECT_BLOOM_REVERB_H
