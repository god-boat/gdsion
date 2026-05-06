/***************************************************/
/* Part of GDSiON software synthesizer             */
/* Copyright (c) 2024 Yuri Sizov and contributors  */
/* Provided under MIT                              */
/***************************************************/

#ifndef SI_EFFECT_BLOOM_REVERB_H
#define SI_EFFECT_BLOOM_REVERB_H

#include <godot_cpp/templates/vector.hpp>
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

	struct FractionalDelay {
		Vector<double> buffer;
		int write_index = 0;

		void resize_samples(int p_length);
		void reset();
		void write(double p_sample);
		double read_samples(double p_delay_samples) const;
	};

	struct OnePoleLowPass {
		double alpha = 0.0;
		double z = 0.0;

		void set_cutoff(double p_hz, double p_sample_rate);
		double process(double p_input);
		void reset();
	};

	struct OnePoleHighPass {
		double alpha = 0.0;
		double x1 = 0.0;
		double y1 = 0.0;

		void set_cutoff(double p_hz, double p_sample_rate);
		double process(double p_input);
		void reset();
	};

	struct AttackSmoother {
		double coeff = 1.0;
		double state = 0.0;

		void set_attack_ms(double p_ms, double p_sample_rate);
		double process(double p_input);
		void reset();
	};

	struct Lfo {
		double phase = 0.0;
		double phase_offset = 0.0;

		double process(double p_rate_hz, double p_depth_samples, double p_sample_rate);
		void reset(double p_phase_offset = 0.0);
	};

	struct AllpassStage {
		FractionalDelay delay;

		double process(double p_input, double p_delay_samples, double p_feedback);
		void reset();
	};

	struct TankLine {
		FractionalDelay delay;
		OnePoleHighPass hp;
		OnePoleLowPass lp;
		Lfo mod;
	};

	struct AirSide {
		FractionalDelay delays[AIR_DELAY_COUNT];
		Lfo mods[AIR_DELAY_COUNT];
		AllpassStage diffuser_a;
		AllpassStage diffuser_b;
		OnePoleHighPass hp;

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
		double high_damp_hz = 7000.0;
		double low_cut_hz = 150.0;
		double feedback_low_cut_hz = 100.0;
		double high_cut_hz = 11000.0;
		double mod_depth_samples = 0.0;
		double width_gain = 1.0;
		double air_gain = 0.0;
		double air_hp_hz = 6500.0;
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

	FractionalDelay _predelay_left;
	FractionalDelay _predelay_right;
	FractionalDelay _early_left;
	FractionalDelay _early_right;

	AllpassStage _diffusers_left[INPUT_DIFFUSER_COUNT];
	AllpassStage _diffusers_right[INPUT_DIFFUSER_COUNT];
	TankLine _tank[TANK_LINE_COUNT];
	AirSide _air_left;
	AirSide _air_right;

	OnePoleHighPass _input_hp_left;
	OnePoleHighPass _input_hp_right;
	OnePoleLowPass _wet_lp_left;
	OnePoleLowPass _wet_lp_right;
	AttackSmoother _bloom_smoother_left;
	AttackSmoother _bloom_smoother_right;

	double _duck_env = 0.0;
	int _cached_sample_rate = 0;

	void _ensure_delay_buffers();
	void _reset_signal_state();
	void _derive_params(const BloomParams &p_params, DerivedParams &r_out) const;
	void _begin_block_smoothing(int p_length);
	void _advance_smoothed_params(int p_segment_length);
	void _set_arg_value(int p_arg_index, double p_value, bool p_snap_current);
	double _process_air_side(
			AirSide &r_side,
			double p_input,
			double p_hp_hz,
			const double *p_delay_samples,
			const double *p_rates,
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
	virtual int process(int p_channels, Vector<double> *r_buffer, int p_start_index, int p_length) override;

	virtual void set_by_mml(Vector<double> p_args) override;
	virtual bool set_arg(int p_arg_index, double p_value) override;
	virtual void reset() override;

	SiEffectBloomReverb();
	~SiEffectBloomReverb() {}
};

#endif // SI_EFFECT_BLOOM_REVERB_H
